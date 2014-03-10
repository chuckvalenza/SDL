/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_D3D11 && !SDL_RENDER_DISABLED

#ifdef __WINRT__
#include <windows.ui.core.h>
#include <windows.foundation.h>

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
#include <windows.ui.xaml.media.dxinterop.h>
#endif

using namespace Windows::UI::Core;
using namespace Windows::Graphics::Display;
#endif  /* __WINRT__ */

#define COBJMACROS
#include "../../core/windows/SDL_windows.h"
#include "SDL_hints.h"
#include "SDL_loadso.h"
#include "SDL_syswm.h"
#include "../SDL_sysrender.h"

#include <d3d11_1.h>


#define SAFE_RELEASE(X) if ( (X) ) { IUnknown_Release( SDL_static_cast(IUnknown*, X ) ); X = NULL; }

typedef struct
{
    float x;
    float y;
} Float2;

typedef struct
{
    float x;
    float y;
    float z;
} Float3;

typedef struct
{
    float x;
    float y;
    float z;
    float w;
} Float4;

typedef struct
{
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} Float4X4;

/* Vertex shader, common values */
typedef struct
{
    Float4X4 model;
    Float4X4 projectionAndView;
} VertexShaderConstants;

/* Per-vertex data */
typedef struct
{
    Float3 pos;
    Float2 tex;
    Float4 color;
} VertexPositionColor;

/* Per-texture data */
typedef struct
{
    ID3D11Texture2D *mainTexture;
    ID3D11ShaderResourceView *mainTextureResourceView;
    ID3D11RenderTargetView *mainTextureRenderTargetView;
    ID3D11Texture2D *stagingTexture;
    int lockedTexturePositionX;
    int lockedTexturePositionY;
    D3D11_FILTER scaleMode;
} D3D11_TextureData;

/* Private renderer data */
typedef struct
{
    void *hD3D11Mod;
    ID3D11Device1 *d3dDevice;
    ID3D11DeviceContext1 *d3dContext;
    IDXGISwapChain1 *swapChain;
    DXGI_SWAP_EFFECT swapEffect;
    ID3D11RenderTargetView *mainRenderTargetView;
    ID3D11RenderTargetView *currentOffscreenRenderTargetView;
    ID3D11InputLayout *inputLayout;
    ID3D11Buffer *vertexBuffer;
    ID3D11VertexShader *vertexShader;
    ID3D11PixelShader *texturePixelShader;
    ID3D11PixelShader *colorPixelShader;
    ID3D11BlendState *blendModeBlend;
    ID3D11BlendState *blendModeAdd;
    ID3D11BlendState *blendModeMod;
    ID3D11SamplerState *nearestPixelSampler;
    ID3D11SamplerState *linearSampler;
    D3D_FEATURE_LEVEL featureLevel;

    /* Rasterizers */
    ID3D11RasterizerState *mainRasterizer;
    ID3D11RasterizerState *clippedRasterizer;

    /* Vertex buffer constants */
    VertexShaderConstants vertexShaderConstantsData;
    ID3D11Buffer *vertexShaderConstants;

    /* Cached renderer properties */
    DXGI_MODE_ROTATION rotation;
    ID3D11RenderTargetView *currentRenderTargetView;
    ID3D11RasterizerState *currentRasterizerState;
    ID3D11BlendState *currentBlendState;
    ID3D11PixelShader *currentShader;
    ID3D11ShaderResourceView *currentShaderResource;
    ID3D11SamplerState *currentSampler;
} D3D11_RenderData;


/* Defined here so we don't have to include uuid.lib */
static const GUID IID_IDXGIFactory2 = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const GUID IID_IDXGIDevice1 = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
static const GUID IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID IID_ID3D11Device1 = { 0xa04bfb29, 0x08ef, 0x43d6, { 0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86 } };
static const GUID IID_ID3D11DeviceContext1 = { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1 } };
static const GUID IID_ID3D11Debug = { 0x79cf2233, 0x7536, 0x4948, { 0x9d, 0x36, 0x1e, 0x46, 0x92, 0xdc, 0x57, 0x60 } };

/* Direct3D 11.x shaders

   SDL's shaders are compiled into SDL itself, to simplify distribution.

   All Direct3D 11.x shaders were compiled with the following:

   fxc /E"main" /T "<TYPE>" /Fo"<OUTPUT FILE>" "<INPUT FILE>"

     Variables:
     - <TYPE>: the type of shader.  A table of utilized shader types is
       listed below.
     - <OUTPUT FILE>: where to store compiled output
     - <INPUT FILE>: where to read shader source code from

     Shader types:
     - ps_4_0_level_9_1: Pixel shader for Windows 8+, including Windows RT
     - vs_4_0_level_9_1: Vertex shader for Windows 8+, including Windows RT
     - ps_4_0_level_9_3: Pixel shader for Windows Phone 8
     - vs_4_0_level_9_3: Vertex shader for Windows Phone 8
   

   Shader object code was converted to a list of DWORDs via the following
   *nix style command (available separately from Windows + MSVC):

     hexdump -v -e '6/4 "0x%08.8x, " "\n"' <FILE>
  */
#if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
#define D3D11_USE_SHADER_MODEL_4_0_level_9_3
#else
#define D3D11_USE_SHADER_MODEL_4_0_level_9_1
#endif

/* The texture-rendering pixel shader:

    --- D3D11_PixelShader_Textures.hlsl ---
    Texture2D theTexture : register(t0);
    SamplerState theSampler : register(s0);

    struct PixelShaderInput
    {
        float4 pos : SV_POSITION;
        float2 tex : TEXCOORD0;
        float4 color : COLOR0;
    };

    float4 main(PixelShaderInput input) : SV_TARGET
    {
        return theTexture.Sample(theSampler, input.tex) * input.color;
    }
*/
#if defined(D3D11_USE_SHADER_MODEL_4_0_level_9_1)
static const DWORD D3D11_PixelShader_Textures[] = {
    0x43425844, 0x6299b59f, 0x155258f2, 0x873ab86a, 0xfcbb6dcd, 0x00000001,
    0x00000330, 0x00000006, 0x00000038, 0x000000c0, 0x0000015c, 0x000001d8,
    0x00000288, 0x000002fc, 0x396e6f41, 0x00000080, 0x00000080, 0xffff0200,
    0x00000058, 0x00000028, 0x00280000, 0x00280000, 0x00280000, 0x00240001,
    0x00280000, 0x00000000, 0xffff0200, 0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x80000000, 0xb00f0001, 0x0200001f, 0x90000000, 0xa00f0800,
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000005, 0x800f0000,
    0x80e40000, 0xb0e40001, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff,
    0x52444853, 0x00000094, 0x00000040, 0x00000025, 0x0300005a, 0x00106000,
    0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x03001062,
    0x00101032, 0x00000001, 0x03001062, 0x001010f2, 0x00000002, 0x03000065,
    0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x09000045, 0x001000f2,
    0x00000000, 0x00101046, 0x00000001, 0x00107e46, 0x00000000, 0x00106000,
    0x00000000, 0x07000038, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000,
    0x00101e46, 0x00000002, 0x0100003e, 0x54415453, 0x00000074, 0x00000003,
    0x00000001, 0x00000000, 0x00000003, 0x00000001, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x46454452, 0x000000a8,
    0x00000000, 0x00000000, 0x00000002, 0x0000001c, 0xffff0400, 0x00000100,
    0x00000072, 0x0000005c, 0x00000003, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000001, 0x00000067, 0x00000002, 0x00000005,
    0x00000004, 0xffffffff, 0x00000000, 0x00000001, 0x0000000d, 0x53656874,
    0x6c706d61, 0x74007265, 0x65546568, 0x72757478, 0x694d0065, 0x736f7263,
    0x2074666f, 0x20295228, 0x4c534c48, 0x61685320, 0x20726564, 0x706d6f43,
    0x72656c69, 0x332e3920, 0x32392e30, 0x312e3030, 0x34383336, 0xababab00,
    0x4e475349, 0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000,
    0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000,
    0x00000000, 0x00000003, 0x00000001, 0x00000303, 0x00000065, 0x00000000,
    0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x505f5653, 0x5449534f,
    0x004e4f49, 0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f, 0x4e47534f,
    0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
    0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054
};
#elif defined(D3D11_USE_SHADER_MODEL_4_0_level_9_3)
static const DWORD D3D11_PixelShader_Textures[] = {
    0x43425844, 0x5876569a, 0x01b6c87e, 0x8447454f, 0xc7f3ef10, 0x00000001,
    0x00000330, 0x00000006, 0x00000038, 0x000000c0, 0x0000015c, 0x000001d8,
    0x00000288, 0x000002fc, 0x396e6f41, 0x00000080, 0x00000080, 0xffff0200,
    0x00000058, 0x00000028, 0x00280000, 0x00280000, 0x00280000, 0x00240001,
    0x00280000, 0x00000000, 0xffff0201, 0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x80000000, 0xb00f0001, 0x0200001f, 0x90000000, 0xa00f0800,
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000005, 0x800f0000,
    0x80e40000, 0xb0e40001, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff,
    0x52444853, 0x00000094, 0x00000040, 0x00000025, 0x0300005a, 0x00106000,
    0x00000000, 0x04001858, 0x00107000, 0x00000000, 0x00005555, 0x03001062,
    0x00101032, 0x00000001, 0x03001062, 0x001010f2, 0x00000002, 0x03000065,
    0x001020f2, 0x00000000, 0x02000068, 0x00000001, 0x09000045, 0x001000f2,
    0x00000000, 0x00101046, 0x00000001, 0x00107e46, 0x00000000, 0x00106000,
    0x00000000, 0x07000038, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000,
    0x00101e46, 0x00000002, 0x0100003e, 0x54415453, 0x00000074, 0x00000003,
    0x00000001, 0x00000000, 0x00000003, 0x00000001, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x46454452, 0x000000a8,
    0x00000000, 0x00000000, 0x00000002, 0x0000001c, 0xffff0400, 0x00000100,
    0x00000072, 0x0000005c, 0x00000003, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000001, 0x00000067, 0x00000002, 0x00000005,
    0x00000004, 0xffffffff, 0x00000000, 0x00000001, 0x0000000d, 0x53656874,
    0x6c706d61, 0x74007265, 0x65546568, 0x72757478, 0x694d0065, 0x736f7263,
    0x2074666f, 0x20295228, 0x4c534c48, 0x61685320, 0x20726564, 0x706d6f43,
    0x72656c69, 0x332e3920, 0x32392e30, 0x312e3030, 0x34383336, 0xababab00,
    0x4e475349, 0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000,
    0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000,
    0x00000000, 0x00000003, 0x00000001, 0x00000303, 0x00000065, 0x00000000,
    0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x505f5653, 0x5449534f,
    0x004e4f49, 0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f, 0x4e47534f,
    0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
    0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054
};
#else
#error "An appropriate 'textures' pixel shader is not defined"
#endif

/* The color-only-rendering pixel shader:

   --- D3D11_PixelShader_Colors.hlsl ---
   struct PixelShaderInput
   {
       float4 pos : SV_POSITION;
       float2 tex : TEXCOORD0;
       float4 color : COLOR0;
   };

   float4 main(PixelShaderInput input) : SV_TARGET
   {
       return input.color;
   }
*/
#if defined(D3D11_USE_SHADER_MODEL_4_0_level_9_1)
static const DWORD D3D11_PixelShader_Colors[] = {
    0x43425844, 0xd74c28fe, 0xa1eb8804, 0x269d512a, 0x7699723d, 0x00000001,
    0x00000240, 0x00000006, 0x00000038, 0x00000084, 0x000000c4, 0x00000140,
    0x00000198, 0x0000020c, 0x396e6f41, 0x00000044, 0x00000044, 0xffff0200,
    0x00000020, 0x00000024, 0x00240000, 0x00240000, 0x00240000, 0x00240000,
    0x00240000, 0xffff0200, 0x0200001f, 0x80000000, 0xb00f0001, 0x02000001,
    0x800f0800, 0xb0e40001, 0x0000ffff, 0x52444853, 0x00000038, 0x00000040,
    0x0000000e, 0x03001062, 0x001010f2, 0x00000002, 0x03000065, 0x001020f2,
    0x00000000, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000002,
    0x0100003e, 0x54415453, 0x00000074, 0x00000002, 0x00000000, 0x00000000,
    0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x46454452, 0x00000050, 0x00000000, 0x00000000,
    0x00000000, 0x0000001c, 0xffff0400, 0x00000100, 0x0000001c, 0x7263694d,
    0x666f736f, 0x52282074, 0x4c482029, 0x53204c53, 0x65646168, 0x6f432072,
    0x6c69706d, 0x39207265, 0x2e30332e, 0x30303239, 0x3336312e, 0xab003438,
    0x4e475349, 0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000,
    0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000,
    0x00000000, 0x00000003, 0x00000001, 0x00000003, 0x00000065, 0x00000000,
    0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x505f5653, 0x5449534f,
    0x004e4f49, 0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f, 0x4e47534f,
    0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
    0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054
};
#elif defined(D3D11_USE_SHADER_MODEL_4_0_level_9_3)
static const DWORD D3D11_PixelShader_Colors[] = {
    0x43425844, 0x93f6ccfc, 0x5f919270, 0x7a11aa4f, 0x9148e931, 0x00000001,
    0x00000240, 0x00000006, 0x00000038, 0x00000084, 0x000000c4, 0x00000140,
    0x00000198, 0x0000020c, 0x396e6f41, 0x00000044, 0x00000044, 0xffff0200,
    0x00000020, 0x00000024, 0x00240000, 0x00240000, 0x00240000, 0x00240000,
    0x00240000, 0xffff0201, 0x0200001f, 0x80000000, 0xb00f0001, 0x02000001,
    0x800f0800, 0xb0e40001, 0x0000ffff, 0x52444853, 0x00000038, 0x00000040,
    0x0000000e, 0x03001062, 0x001010f2, 0x00000002, 0x03000065, 0x001020f2,
    0x00000000, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000002,
    0x0100003e, 0x54415453, 0x00000074, 0x00000002, 0x00000000, 0x00000000,
    0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x46454452, 0x00000050, 0x00000000, 0x00000000,
    0x00000000, 0x0000001c, 0xffff0400, 0x00000100, 0x0000001c, 0x7263694d,
    0x666f736f, 0x52282074, 0x4c482029, 0x53204c53, 0x65646168, 0x6f432072,
    0x6c69706d, 0x39207265, 0x2e30332e, 0x30303239, 0x3336312e, 0xab003438,
    0x4e475349, 0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000,
    0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000,
    0x00000000, 0x00000003, 0x00000001, 0x00000003, 0x00000065, 0x00000000,
    0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x505f5653, 0x5449534f,
    0x004e4f49, 0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f, 0x4e47534f,
    0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
    0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x45475241, 0xabab0054
};
#else
#error "An appropriate 'colors' pixel shader is not defined."
#endif

/* The sole vertex shader:

   --- D3D11_VertexShader.hlsl ---
   #pragma pack_matrix( row_major )

   cbuffer VertexShaderConstants : register(b0)
   {
       matrix model;
       matrix projectionAndView;
   };

   struct VertexShaderInput
   {
       float3 pos : POSITION;
       float2 tex : TEXCOORD0;
       float4 color : COLOR0;
   };

   struct VertexShaderOutput
   {
       float4 pos : SV_POSITION;
       float2 tex : TEXCOORD0;
       float4 color : COLOR0;
   };

   VertexShaderOutput main(VertexShaderInput input)
   {
       VertexShaderOutput output;
       float4 pos = float4(input.pos, 1.0f);

       // Transform the vertex position into projected space.
       pos = mul(pos, model);
       pos = mul(pos, projectionAndView);
       output.pos = pos;

       // Pass through texture coordinates and color values without transformation
       output.tex = input.tex;
       output.color = input.color;

       return output;
   }
*/
#if defined(D3D11_USE_SHADER_MODEL_4_0_level_9_1)
static const DWORD D3D11_VertexShader[] = {
    0x43425844, 0x62dfae5f, 0x3e8bd8df, 0x9ec97127, 0x5044eefb, 0x00000001,
    0x00000598, 0x00000006, 0x00000038, 0x0000016c, 0x00000334, 0x000003b0,
    0x000004b4, 0x00000524, 0x396e6f41, 0x0000012c, 0x0000012c, 0xfffe0200,
    0x000000f8, 0x00000034, 0x00240001, 0x00300000, 0x00300000, 0x00240000,
    0x00300001, 0x00000000, 0x00010008, 0x00000000, 0x00000000, 0xfffe0200,
    0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x80010005, 0x900f0001,
    0x0200001f, 0x80020005, 0x900f0002, 0x03000005, 0x800f0000, 0x90550000,
    0xa0e40002, 0x04000004, 0x800f0000, 0x90000000, 0xa0e40001, 0x80e40000,
    0x04000004, 0x800f0000, 0x90aa0000, 0xa0e40003, 0x80e40000, 0x03000002,
    0x800f0000, 0x80e40000, 0xa0e40004, 0x03000005, 0x800f0001, 0x80550000,
    0xa0e40006, 0x04000004, 0x800f0001, 0x80000000, 0xa0e40005, 0x80e40001,
    0x04000004, 0x800f0001, 0x80aa0000, 0xa0e40007, 0x80e40001, 0x04000004,
    0x800f0000, 0x80ff0000, 0xa0e40008, 0x80e40001, 0x04000004, 0xc0030000,
    0x80ff0000, 0xa0e40000, 0x80e40000, 0x02000001, 0xc00c0000, 0x80e40000,
    0x02000001, 0xe0030000, 0x90e40001, 0x02000001, 0xe00f0001, 0x90e40002,
    0x0000ffff, 0x52444853, 0x000001c0, 0x00010040, 0x00000070, 0x04000059,
    0x00208e46, 0x00000000, 0x00000008, 0x0300005f, 0x00101072, 0x00000000,
    0x0300005f, 0x00101032, 0x00000001, 0x0300005f, 0x001010f2, 0x00000002,
    0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102032,
    0x00000001, 0x03000065, 0x001020f2, 0x00000002, 0x02000068, 0x00000002,
    0x08000038, 0x001000f2, 0x00000000, 0x00101556, 0x00000000, 0x00208e46,
    0x00000000, 0x00000001, 0x0a000032, 0x001000f2, 0x00000000, 0x00101006,
    0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00100e46, 0x00000000,
    0x0a000032, 0x001000f2, 0x00000000, 0x00101aa6, 0x00000000, 0x00208e46,
    0x00000000, 0x00000002, 0x00100e46, 0x00000000, 0x08000000, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x00208e46, 0x00000000, 0x00000003,
    0x08000038, 0x001000f2, 0x00000001, 0x00100556, 0x00000000, 0x00208e46,
    0x00000000, 0x00000005, 0x0a000032, 0x001000f2, 0x00000001, 0x00100006,
    0x00000000, 0x00208e46, 0x00000000, 0x00000004, 0x00100e46, 0x00000001,
    0x0a000032, 0x001000f2, 0x00000001, 0x00100aa6, 0x00000000, 0x00208e46,
    0x00000000, 0x00000006, 0x00100e46, 0x00000001, 0x0a000032, 0x001020f2,
    0x00000000, 0x00100ff6, 0x00000000, 0x00208e46, 0x00000000, 0x00000007,
    0x00100e46, 0x00000001, 0x05000036, 0x00102032, 0x00000001, 0x00101046,
    0x00000001, 0x05000036, 0x001020f2, 0x00000002, 0x00101e46, 0x00000002,
    0x0100003e, 0x54415453, 0x00000074, 0x0000000b, 0x00000002, 0x00000000,
    0x00000006, 0x00000003, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x46454452, 0x000000fc, 0x00000001, 0x00000054,
    0x00000001, 0x0000001c, 0xfffe0400, 0x00000100, 0x000000c6, 0x0000003c,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000001, 0x74726556, 0x68537865, 0x72656461, 0x736e6f43, 0x746e6174,
    0xabab0073, 0x0000003c, 0x00000002, 0x0000006c, 0x00000080, 0x00000000,
    0x00000000, 0x0000009c, 0x00000000, 0x00000040, 0x00000002, 0x000000a4,
    0x00000000, 0x000000b4, 0x00000040, 0x00000040, 0x00000002, 0x000000a4,
    0x00000000, 0x65646f6d, 0xabab006c, 0x00030002, 0x00040004, 0x00000000,
    0x00000000, 0x6a6f7270, 0x69746365, 0x6e416e6f, 0x65695664, 0x694d0077,
    0x736f7263, 0x2074666f, 0x20295228, 0x4c534c48, 0x61685320, 0x20726564,
    0x706d6f43, 0x72656c69, 0x332e3920, 0x32392e30, 0x312e3030, 0x34383336,
    0xababab00, 0x4e475349, 0x00000068, 0x00000003, 0x00000008, 0x00000050,
    0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000707, 0x00000059,
    0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000303, 0x00000062,
    0x00000000, 0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x49534f50,
    0x4e4f4954, 0x58455400, 0x524f4f43, 0x4f430044, 0x00524f4c, 0x4e47534f,
    0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000, 0x00000001,
    0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000,
    0x00000003, 0x00000001, 0x00000c03, 0x00000065, 0x00000000, 0x00000000,
    0x00000003, 0x00000002, 0x0000000f, 0x505f5653, 0x5449534f, 0x004e4f49,
    0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f
};
#elif defined(D3D11_USE_SHADER_MODEL_4_0_level_9_3)
static const DWORD D3D11_VertexShader[] = {
    0x43425844, 0x01a24e41, 0x696af551, 0x4b2a87d1, 0x82ea03f6, 0x00000001,
    0x00000598, 0x00000006, 0x00000038, 0x0000016c, 0x00000334, 0x000003b0,
    0x000004b4, 0x00000524, 0x396e6f41, 0x0000012c, 0x0000012c, 0xfffe0200,
    0x000000f8, 0x00000034, 0x00240001, 0x00300000, 0x00300000, 0x00240000,
    0x00300001, 0x00000000, 0x00010008, 0x00000000, 0x00000000, 0xfffe0201,
    0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x80010005, 0x900f0001,
    0x0200001f, 0x80020005, 0x900f0002, 0x03000005, 0x800f0000, 0x90550000,
    0xa0e40002, 0x04000004, 0x800f0000, 0x90000000, 0xa0e40001, 0x80e40000,
    0x04000004, 0x800f0000, 0x90aa0000, 0xa0e40003, 0x80e40000, 0x03000002,
    0x800f0000, 0x80e40000, 0xa0e40004, 0x03000005, 0x800f0001, 0x80550000,
    0xa0e40006, 0x04000004, 0x800f0001, 0x80000000, 0xa0e40005, 0x80e40001,
    0x04000004, 0x800f0001, 0x80aa0000, 0xa0e40007, 0x80e40001, 0x04000004,
    0x800f0000, 0x80ff0000, 0xa0e40008, 0x80e40001, 0x04000004, 0xc0030000,
    0x80ff0000, 0xa0e40000, 0x80e40000, 0x02000001, 0xc00c0000, 0x80e40000,
    0x02000001, 0xe0030000, 0x90e40001, 0x02000001, 0xe00f0001, 0x90e40002,
    0x0000ffff, 0x52444853, 0x000001c0, 0x00010040, 0x00000070, 0x04000059,
    0x00208e46, 0x00000000, 0x00000008, 0x0300005f, 0x00101072, 0x00000000,
    0x0300005f, 0x00101032, 0x00000001, 0x0300005f, 0x001010f2, 0x00000002,
    0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102032,
    0x00000001, 0x03000065, 0x001020f2, 0x00000002, 0x02000068, 0x00000002,
    0x08000038, 0x001000f2, 0x00000000, 0x00101556, 0x00000000, 0x00208e46,
    0x00000000, 0x00000001, 0x0a000032, 0x001000f2, 0x00000000, 0x00101006,
    0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00100e46, 0x00000000,
    0x0a000032, 0x001000f2, 0x00000000, 0x00101aa6, 0x00000000, 0x00208e46,
    0x00000000, 0x00000002, 0x00100e46, 0x00000000, 0x08000000, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x00208e46, 0x00000000, 0x00000003,
    0x08000038, 0x001000f2, 0x00000001, 0x00100556, 0x00000000, 0x00208e46,
    0x00000000, 0x00000005, 0x0a000032, 0x001000f2, 0x00000001, 0x00100006,
    0x00000000, 0x00208e46, 0x00000000, 0x00000004, 0x00100e46, 0x00000001,
    0x0a000032, 0x001000f2, 0x00000001, 0x00100aa6, 0x00000000, 0x00208e46,
    0x00000000, 0x00000006, 0x00100e46, 0x00000001, 0x0a000032, 0x001020f2,
    0x00000000, 0x00100ff6, 0x00000000, 0x00208e46, 0x00000000, 0x00000007,
    0x00100e46, 0x00000001, 0x05000036, 0x00102032, 0x00000001, 0x00101046,
    0x00000001, 0x05000036, 0x001020f2, 0x00000002, 0x00101e46, 0x00000002,
    0x0100003e, 0x54415453, 0x00000074, 0x0000000b, 0x00000002, 0x00000000,
    0x00000006, 0x00000003, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x46454452, 0x000000fc, 0x00000001, 0x00000054,
    0x00000001, 0x0000001c, 0xfffe0400, 0x00000100, 0x000000c6, 0x0000003c,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000001, 0x74726556, 0x68537865, 0x72656461, 0x736e6f43, 0x746e6174,
    0xabab0073, 0x0000003c, 0x00000002, 0x0000006c, 0x00000080, 0x00000000,
    0x00000000, 0x0000009c, 0x00000000, 0x00000040, 0x00000002, 0x000000a4,
    0x00000000, 0x000000b4, 0x00000040, 0x00000040, 0x00000002, 0x000000a4,
    0x00000000, 0x65646f6d, 0xabab006c, 0x00030002, 0x00040004, 0x00000000,
    0x00000000, 0x6a6f7270, 0x69746365, 0x6e416e6f, 0x65695664, 0x694d0077,
    0x736f7263, 0x2074666f, 0x20295228, 0x4c534c48, 0x61685320, 0x20726564,
    0x706d6f43, 0x72656c69, 0x332e3920, 0x32392e30, 0x312e3030, 0x34383336,
    0xababab00, 0x4e475349, 0x00000068, 0x00000003, 0x00000008, 0x00000050,
    0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000707, 0x00000059,
    0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000303, 0x00000062,
    0x00000000, 0x00000000, 0x00000003, 0x00000002, 0x00000f0f, 0x49534f50,
    0x4e4f4954, 0x58455400, 0x524f4f43, 0x4f430044, 0x00524f4c, 0x4e47534f,
    0x0000006c, 0x00000003, 0x00000008, 0x00000050, 0x00000000, 0x00000001,
    0x00000003, 0x00000000, 0x0000000f, 0x0000005c, 0x00000000, 0x00000000,
    0x00000003, 0x00000001, 0x00000c03, 0x00000065, 0x00000000, 0x00000000,
    0x00000003, 0x00000002, 0x0000000f, 0x505f5653, 0x5449534f, 0x004e4f49,
    0x43584554, 0x44524f4f, 0x4c4f4300, 0xab00524f
};
#else
#error "An appropriate vertex shader is not defined."
#endif

/* Direct3D matrix math functions */

static Float4X4 MatrixIdentity()
{
    Float4X4 m;
    SDL_zero(m);
    m._11 = 1.0f;
    m._22 = 1.0f;
    m._33 = 1.0f;
    m._44 = 1.0f;
    return m;
}

static Float4X4 MatrixMultiply(Float4X4 M1, Float4X4 M2)
{
    Float4X4 m;
    SDL_zero(m);
    m._11 = M1._11 * M2._11 + M1._12 * M2._21 + M1._13 * M2._31 + M1._14 * M2._41;
    m._12 = M1._11 * M2._12 + M1._12 * M2._22 + M1._13 * M2._32 + M1._14 * M2._42;
    m._13 = M1._11 * M2._13 + M1._12 * M2._23 + M1._13 * M2._33 + M1._14 * M2._43;
    m._14 = M1._11 * M2._14 + M1._12 * M2._24 + M1._13 * M2._34 + M1._14 * M2._44;
    m._21 = M1._21 * M2._11 + M1._22 * M2._21 + M1._23 * M2._31 + M1._24 * M2._41;
    m._22 = M1._21 * M2._12 + M1._22 * M2._22 + M1._23 * M2._32 + M1._24 * M2._42;
    m._23 = M1._21 * M2._13 + M1._22 * M2._23 + M1._23 * M2._33 + M1._24 * M2._43;
    m._24 = M1._21 * M2._14 + M1._22 * M2._24 + M1._23 * M2._34 + M1._24 * M2._44;
    m._31 = M1._31 * M2._11 + M1._32 * M2._21 + M1._33 * M2._31 + M1._34 * M2._41;
    m._32 = M1._31 * M2._12 + M1._32 * M2._22 + M1._33 * M2._32 + M1._34 * M2._42;
    m._33 = M1._31 * M2._13 + M1._32 * M2._23 + M1._33 * M2._33 + M1._34 * M2._43;
    m._34 = M1._31 * M2._14 + M1._32 * M2._24 + M1._33 * M2._34 + M1._34 * M2._44;
    m._41 = M1._41 * M2._11 + M1._42 * M2._21 + M1._43 * M2._31 + M1._44 * M2._41;
    m._42 = M1._41 * M2._12 + M1._42 * M2._22 + M1._43 * M2._32 + M1._44 * M2._42;
    m._43 = M1._41 * M2._13 + M1._42 * M2._23 + M1._43 * M2._33 + M1._44 * M2._43;
    m._44 = M1._41 * M2._14 + M1._42 * M2._24 + M1._43 * M2._34 + M1._44 * M2._44;
    return m;
}

static Float4X4 MatrixScaling(float x, float y, float z)
{
    Float4X4 m;
    SDL_zero(m);
    m._11 = x;
    m._22 = y;
    m._33 = z;
    m._44 = 1.0f;
    return m;
}

static Float4X4 MatrixTranslation(float x, float y, float z)
{
    Float4X4 m;
    SDL_zero(m);
    m._11 = 1.0f;
    m._22 = 1.0f;
    m._33 = 1.0f;
    m._44 = 1.0f;
    m._41 = x;
    m._42 = y;
    m._43 = z;
    return m;
}

static Float4X4 MatrixRotationX(float r)
{
    float sinR = SDL_sinf(r);
    float cosR = SDL_cosf(r);
    Float4X4 m;
    SDL_zero(m);
    m._11 = 1.0f;
    m._22 = cosR;
    m._23 = sinR;
    m._32 = -sinR;
    m._33 = cosR;
    m._44 = 1.0f;
    return m;
}

static Float4X4 MatrixRotationY(float r)
{
    float sinR = SDL_sinf(r);
    float cosR = SDL_cosf(r);
    Float4X4 m;
    SDL_zero(m);
    m._11 = cosR;
    m._13 = -sinR;
    m._22 = 1.0f;
    m._31 = sinR;
    m._33 = cosR;
    m._44 = 1.0f;
    return m;
}

static Float4X4 MatrixRotationZ(float r)
{
    float sinR = SDL_sinf(r);
    float cosR = SDL_cosf(r);
    Float4X4 m;
    SDL_zero(m);
    m._11 = cosR;
    m._12 = sinR;
    m._21 = -sinR;
    m._22 = cosR;
    m._33 = 1.0f;
    m._44 = 1.0f;
    return m;
}


/* Direct3D 11.1 renderer implementation */
static SDL_Renderer *D3D11_CreateRenderer(SDL_Window * window, Uint32 flags);
static void D3D11_WindowEvent(SDL_Renderer * renderer,
                            const SDL_WindowEvent *event);
static int D3D11_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static int D3D11_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                             const SDL_Rect * rect, const void *srcPixels,
                             int srcPitch);
static int D3D11_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                             const SDL_Rect * rect, void **pixels, int *pitch);
static void D3D11_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static int D3D11_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture);
static int D3D11_UpdateViewport(SDL_Renderer * renderer);
static int D3D11_UpdateClipRect(SDL_Renderer * renderer);
static int D3D11_RenderClear(SDL_Renderer * renderer);
static int D3D11_RenderDrawPoints(SDL_Renderer * renderer,
                                  const SDL_FPoint * points, int count);
static int D3D11_RenderDrawLines(SDL_Renderer * renderer,
                                 const SDL_FPoint * points, int count);
static int D3D11_RenderFillRects(SDL_Renderer * renderer,
                                 const SDL_FRect * rects, int count);
static int D3D11_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                            const SDL_Rect * srcrect, const SDL_FRect * dstrect);
static int D3D11_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                              const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                              const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip);
static int D3D11_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                                  Uint32 format, void * pixels, int pitch);
static void D3D11_RenderPresent(SDL_Renderer * renderer);
static void D3D11_DestroyTexture(SDL_Renderer * renderer,
                                 SDL_Texture * texture);
static void D3D11_DestroyRenderer(SDL_Renderer * renderer);

/* Direct3D 11.1 Internal Functions */
static HRESULT D3D11_CreateDeviceResources(SDL_Renderer * renderer);
static HRESULT D3D11_CreateWindowSizeDependentResources(SDL_Renderer * renderer);
static HRESULT D3D11_UpdateForWindowSizeChange(SDL_Renderer * renderer);
static HRESULT D3D11_HandleDeviceLost(SDL_Renderer * renderer);
static void D3D11_ReleaseMainRenderTargetView(SDL_Renderer * renderer);

SDL_RenderDriver D3D11_RenderDriver = {
    D3D11_CreateRenderer,
    {
        "direct3d11",
        (
            SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC |
            SDL_RENDERER_TARGETTEXTURE
        ),                          /* flags.  see SDL_RendererFlags */
        2,                          /* num_texture_formats */
        {                           /* texture_formats */
            SDL_PIXELFORMAT_RGB888,
            SDL_PIXELFORMAT_ARGB8888
        },
        0,                          /* max_texture_width: will be filled in later */
        0                           /* max_texture_height: will be filled in later */
    }
};


static Uint32
DXGIFormatToSDLPixelFormat(DXGI_FORMAT dxgiFormat) {
    switch (dxgiFormat) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return SDL_PIXELFORMAT_ARGB8888;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return SDL_PIXELFORMAT_RGB888;
        default:
            return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static DXGI_FORMAT
SDLPixelFormatToDXGIFormat(Uint32 sdlFormat)
{
    switch (sdlFormat) {
        case SDL_PIXELFORMAT_ARGB8888:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case SDL_PIXELFORMAT_RGB888:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

SDL_Renderer *
D3D11_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    SDL_Renderer *renderer;
    D3D11_RenderData *data;

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (D3D11_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        SDL_OutOfMemory();
        return NULL;
    }

    renderer->WindowEvent = D3D11_WindowEvent;
    renderer->CreateTexture = D3D11_CreateTexture;
    renderer->UpdateTexture = D3D11_UpdateTexture;
    renderer->LockTexture = D3D11_LockTexture;
    renderer->UnlockTexture = D3D11_UnlockTexture;
    renderer->SetRenderTarget = D3D11_SetRenderTarget;
    renderer->UpdateViewport = D3D11_UpdateViewport;
    renderer->UpdateClipRect = D3D11_UpdateClipRect;
    renderer->RenderClear = D3D11_RenderClear;
    renderer->RenderDrawPoints = D3D11_RenderDrawPoints;
    renderer->RenderDrawLines = D3D11_RenderDrawLines;
    renderer->RenderFillRects = D3D11_RenderFillRects;
    renderer->RenderCopy = D3D11_RenderCopy;
    renderer->RenderCopyEx = D3D11_RenderCopyEx;
    renderer->RenderReadPixels = D3D11_RenderReadPixels;
    renderer->RenderPresent = D3D11_RenderPresent;
    renderer->DestroyTexture = D3D11_DestroyTexture;
    renderer->DestroyRenderer = D3D11_DestroyRenderer;
    renderer->info = D3D11_RenderDriver.info;
    renderer->info.flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    renderer->driverdata = data;

    if ((flags & SDL_RENDERER_PRESENTVSYNC)) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    /* HACK: make sure the SDL_Renderer references the SDL_Window data now, in
     * order to give init functions access to the underlying window handle:
     */
    renderer->window = window;

    /* Initialize Direct3D resources */
    if (FAILED(D3D11_CreateDeviceResources(renderer))) {
        D3D11_DestroyRenderer(renderer);
        return NULL;
    }
    if (FAILED(D3D11_CreateWindowSizeDependentResources(renderer))) {
        D3D11_DestroyRenderer(renderer);
        return NULL;
    }

    return renderer;
}

static void
D3D11_DestroyRenderer(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;

    if (data) {
        SAFE_RELEASE(data->d3dDevice);
        SAFE_RELEASE(data->d3dContext);
        SAFE_RELEASE(data->swapChain);
        SAFE_RELEASE(data->mainRenderTargetView);
        SAFE_RELEASE(data->currentOffscreenRenderTargetView);
        SAFE_RELEASE(data->inputLayout);
        SAFE_RELEASE(data->vertexBuffer);
        SAFE_RELEASE(data->vertexShader);
        SAFE_RELEASE(data->texturePixelShader);
        SAFE_RELEASE(data->colorPixelShader);
        SAFE_RELEASE(data->blendModeBlend);
        SAFE_RELEASE(data->blendModeAdd);
        SAFE_RELEASE(data->blendModeMod);
        SAFE_RELEASE(data->nearestPixelSampler);
        SAFE_RELEASE(data->linearSampler);
        SAFE_RELEASE(data->mainRasterizer);
        SAFE_RELEASE(data->clippedRasterizer);
        SAFE_RELEASE(data->vertexShaderConstants);

        if (data->hD3D11Mod) {
            SDL_UnloadObject(data->hD3D11Mod);
        }
        SDL_free(data);
    }
    SDL_free(renderer);
}

static HRESULT
D3D11_CreateBlendMode(SDL_Renderer * renderer,
                      BOOL enableBlending,
                      D3D11_BLEND srcBlend,
                      D3D11_BLEND destBlend,
                      D3D11_BLEND srcBlendAlpha,
                      D3D11_BLEND destBlendAlpha,
                      ID3D11BlendState ** blendStateOutput)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    HRESULT result = S_OK;

    D3D11_BLEND_DESC blendDesc;
    SDL_zero(blendDesc);
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = enableBlending;
    blendDesc.RenderTarget[0].SrcBlend = srcBlend;
    blendDesc.RenderTarget[0].DestBlend = destBlend;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = srcBlendAlpha;
    blendDesc.RenderTarget[0].DestBlendAlpha = destBlendAlpha;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    result = ID3D11Device_CreateBlendState(data->d3dDevice, &blendDesc, blendStateOutput);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateBlendState", result);
        return result;
    }

    return S_OK;
}

/* Create resources that depend on the device. */
static HRESULT
D3D11_CreateDeviceResources(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    PFN_D3D11_CREATE_DEVICE D3D11CreateDeviceFunc;
    ID3D11Device *d3dDevice = NULL;
    ID3D11DeviceContext *d3dContext = NULL;
    HRESULT result = S_OK;

#ifdef __WINRT__
    D3D11CreateDeviceFunc = D3D11CreateDevice;
#else
    data->hD3D11Mod = SDL_LoadObject("d3d11.dll");
    if (!data->hD3D11Mod) {
        result = E_FAIL;
        goto done;
    }

    D3D11CreateDeviceFunc = (PFN_D3D11_CREATE_DEVICE)SDL_LoadFunction(data->hD3D11Mod, "D3D11CreateDevice");
    if (!D3D11CreateDeviceFunc) {
        result = E_FAIL;
        goto done;
    }
#endif /* __WINRT__ */

    /* This flag adds support for surfaces with a different color channel ordering
     * than the API default. It is required for compatibility with Direct2D.
     */
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    /* Make sure Direct3D's debugging feature gets used, if the app requests it. */
    const char *hint = SDL_GetHint(SDL_HINT_RENDER_DIRECT3D11_DEBUG);
    if (hint && SDL_atoi(hint) > 0) {
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    /* This array defines the set of DirectX hardware feature levels this app will support.
     * Note the ordering should be preserved.
     * Don't forget to declare your application's minimum required feature level in its
     * description.  All applications are assumed to support 9.1 unless otherwise stated.
     */
    D3D_FEATURE_LEVEL featureLevels[] = 
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };

    /* Create the Direct3D 11 API device object and a corresponding context. */
    result = D3D11CreateDeviceFunc(
        NULL, /* Specify NULL to use the default adapter */
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        creationFlags, /* Set set debug and Direct2D compatibility flags. */
        featureLevels, /* List of feature levels this app can support. */
        SDL_arraysize(featureLevels),
        D3D11_SDK_VERSION, /* Always set this to D3D11_SDK_VERSION for Windows Store apps. */
        &d3dDevice, /* Returns the Direct3D device created. */
        &data->featureLevel, /* Returns feature level of device created. */
        &d3dContext /* Returns the device immediate context. */
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", D3D11CreateDevice", result);
        goto done;
    }

    result = ID3D11Device_QueryInterface(d3dDevice, &IID_ID3D11Device1, &data->d3dDevice);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device to ID3D11Device1", result);
        goto done;
    }

    result = ID3D11DeviceContext_QueryInterface(d3dContext, &IID_ID3D11DeviceContext1, &data->d3dContext);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11DeviceContext to ID3D11DeviceContext1", result);
        goto done;
    }

    /* Make note of the maximum texture size
     * Max texture sizes are documented on MSDN, at:
     * http://msdn.microsoft.com/en-us/library/windows/apps/ff476876.aspx
     */
    switch (data->featureLevel) {
        case D3D_FEATURE_LEVEL_11_1:
        case D3D_FEATURE_LEVEL_11_0:
            renderer->info.max_texture_width = renderer->info.max_texture_height = 16384;
            break;

        case D3D_FEATURE_LEVEL_10_1:
        case D3D_FEATURE_LEVEL_10_0:
            renderer->info.max_texture_width = renderer->info.max_texture_height = 8192;
            break;

        case D3D_FEATURE_LEVEL_9_3:
            renderer->info.max_texture_width = renderer->info.max_texture_height = 4096;
            break;

        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_1:
            renderer->info.max_texture_width = renderer->info.max_texture_height = 2048;
            break;

        default:
            SDL_SetError(__FUNCTION__ ", Unexpected feature level: %d", data->featureLevel);
            result = E_FAIL;
            goto done;
    }

    /* Load in SDL's one and only vertex shader: */
    result = ID3D11Device_CreateVertexShader(data->d3dDevice,
        D3D11_VertexShader,
        sizeof(D3D11_VertexShader),
        NULL,
        &data->vertexShader
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateVertexShader", result);
        goto done;
    }

    /* Create an input layout for SDL's vertex shader: */
    const D3D11_INPUT_ELEMENT_DESC vertexDesc[] = 
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    result = ID3D11Device_CreateInputLayout(data->d3dDevice,
        vertexDesc,
        ARRAYSIZE(vertexDesc),
        D3D11_VertexShader,
        sizeof(D3D11_VertexShader),
        &data->inputLayout
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateInputLayout", result);
        goto done;
    }

    /* Load in SDL's pixel shaders */
    result = ID3D11Device_CreatePixelShader(data->d3dDevice,
        D3D11_PixelShader_Textures,
        sizeof(D3D11_PixelShader_Textures),
        NULL,
        &data->texturePixelShader
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreatePixelShader ['textures' shader]", result);
        goto done;
    }

    result = ID3D11Device_CreatePixelShader(data->d3dDevice,
        D3D11_PixelShader_Colors,
        sizeof(D3D11_PixelShader_Colors),
        NULL,
        &data->colorPixelShader
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreatePixelShader ['color' shader]", result);
        goto done;
    }

    /* Setup space to hold vertex shader constants: */
    D3D11_BUFFER_DESC constantBufferDesc;
    SDL_zero(constantBufferDesc);
    constantBufferDesc.ByteWidth = sizeof(VertexShaderConstants);
    constantBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = ID3D11Device_CreateBuffer(data->d3dDevice,
		&constantBufferDesc,
		NULL,
        &data->vertexShaderConstants
		);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateBuffer [vertex shader constants]", result);
        goto done;
    }

    /* Create samplers to use when drawing textures: */
    D3D11_SAMPLER_DESC samplerDesc;
    SDL_zero(samplerDesc);
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    result = ID3D11Device_CreateSamplerState(data->d3dDevice,
        &samplerDesc,
        &data->nearestPixelSampler
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateSamplerState [nearest-pixel filter]", result);
        goto done;
    }

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    result = ID3D11Device_CreateSamplerState(data->d3dDevice,
        &samplerDesc,
        &data->linearSampler
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateSamplerState [linear filter]", result);
        goto done;
    }

    /* Setup Direct3D rasterizer states */
    D3D11_RASTERIZER_DESC rasterDesc;
    SDL_zero(rasterDesc);
	rasterDesc.AntialiasedLineEnable = FALSE;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthBias = 0;
	rasterDesc.DepthBiasClamp = 0.0f;
	rasterDesc.DepthClipEnable = TRUE;
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.ScissorEnable = FALSE;
	rasterDesc.SlopeScaledDepthBias = 0.0f;
    result = ID3D11Device_CreateRasterizerState(data->d3dDevice, &rasterDesc, &data->mainRasterizer);
	if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateRasterizerState [main rasterizer]", result);
        goto done;
    }

    rasterDesc.ScissorEnable = TRUE;
    result = ID3D11Device_CreateRasterizerState(data->d3dDevice, &rasterDesc, &data->clippedRasterizer);
	if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateRasterizerState [clipped rasterizer]", result);
        goto done;
    }

    /* Create blending states: */
    result = D3D11_CreateBlendMode(
        renderer,
        TRUE,
        D3D11_BLEND_SRC_ALPHA,          /* srcBlend */
        D3D11_BLEND_INV_SRC_ALPHA,      /* destBlend */
        D3D11_BLEND_ONE,                /* srcBlendAlpha */
        D3D11_BLEND_INV_SRC_ALPHA,      /* destBlendAlpha */
        &data->blendModeBlend);
    if (FAILED(result)) {
        /* D3D11_CreateBlendMode will set the SDL error, if it fails */
        goto done;
    }

    result = D3D11_CreateBlendMode(
        renderer,
        TRUE,
        D3D11_BLEND_SRC_ALPHA,          /* srcBlend */
        D3D11_BLEND_ONE,                /* destBlend */
        D3D11_BLEND_ZERO,               /* srcBlendAlpha */
        D3D11_BLEND_ONE,                /* destBlendAlpha */
        &data->blendModeAdd);
    if (FAILED(result)) {
        /* D3D11_CreateBlendMode will set the SDL error, if it fails */
        goto done;
    }

    result = D3D11_CreateBlendMode(
        renderer,
        TRUE,
        D3D11_BLEND_ZERO,               /* srcBlend */
        D3D11_BLEND_SRC_COLOR,          /* destBlend */
        D3D11_BLEND_ZERO,               /* srcBlendAlpha */
        D3D11_BLEND_ONE,                /* destBlendAlpha */
        &data->blendModeMod);
    if (FAILED(result)) {
        /* D3D11_CreateBlendMode will set the SDL error, if it fails */
        goto done;
    }

    /* Setup render state that doesn't change through the program */
    ID3D11DeviceContext_IASetInputLayout(data->d3dContext, data->inputLayout);
    ID3D11DeviceContext_VSSetShader(data->d3dContext, data->vertexShader, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(data->d3dContext, 0, 1, &data->vertexShaderConstants);

done:
    SAFE_RELEASE(d3dDevice);
    SAFE_RELEASE(d3dContext);
    return result;
}

#ifdef __WINRT__

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
/* TODO, WinRT, XAML: get the ISwapChainBackgroundPanelNative from something other than a global var */
extern ISwapChainBackgroundPanelNative * WINRT_GlobalSwapChainBackgroundPanelNative;
#endif

static IUnknown *
D3D11_GetCoreWindowFromSDLRenderer(SDL_Renderer * renderer)
{
    SDL_Window * sdlWindow = renderer->window;
    if ( ! renderer->window ) {
        return NULL;
    }

    SDL_SysWMinfo sdlWindowInfo;
    SDL_VERSION(&sdlWindowInfo.version);
    if ( ! SDL_GetWindowWMInfo(sdlWindow, &sdlWindowInfo) ) {
        return NULL;
    }

    if (sdlWindowInfo.subsystem != SDL_SYSWM_WINRT) {
        return NULL;
    }

    if (!sdlWindowInfo.info.winrt.window) {
        return NULL;
    }

    ABI::Windows::UI::Core::ICoreWindow *coreWindow = NULL;
    if (FAILED(sdlWindowInfo.info.winrt.window->QueryInterface(&coreWindow))) {
        return NULL;
    }

    IUnknown *coreWindowAsIUnknown = NULL;
    coreWindow->QueryInterface(&coreWindowAsIUnknown);
    coreWindow->Release();

    return coreWindowAsIUnknown;
}

static DXGI_MODE_ROTATION
D3D11_GetCurrentRotation()
{
    switch (DisplayProperties::CurrentOrientation)
    {
#if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
        /* Windows Phone rotations */
    case DisplayOrientations::Landscape:
        return DXGI_MODE_ROTATION_ROTATE90;
    case DisplayOrientations::Portrait:
        return DXGI_MODE_ROTATION_IDENTITY;
    case DisplayOrientations::LandscapeFlipped:
        return DXGI_MODE_ROTATION_ROTATE270;
    case DisplayOrientations::PortraitFlipped:
        return DXGI_MODE_ROTATION_ROTATE180;
#else
        /* Non-Windows-Phone rotations (ex: Windows 8, Windows RT) */
    case DisplayOrientations::Landscape:
        return DXGI_MODE_ROTATION_IDENTITY;
    case DisplayOrientations::Portrait:
        return DXGI_MODE_ROTATION_ROTATE270;
    case DisplayOrientations::LandscapeFlipped:
        return DXGI_MODE_ROTATION_ROTATE180;
    case DisplayOrientations::PortraitFlipped:
        return DXGI_MODE_ROTATION_ROTATE90;
#endif /* WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP */

    default:
        return DXGI_MODE_ROTATION_UNSPECIFIED;
    }
}

#else

static DXGI_MODE_ROTATION
D3D11_GetCurrentRotation()
{
    /* FIXME */
    return DXGI_MODE_ROTATION_IDENTITY;
}

#endif /* __WINRT__ */

static BOOL
D3D11_IsDisplayRotated90Degrees(DXGI_MODE_ROTATION rotation)
{
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
        case DXGI_MODE_ROTATION_ROTATE270:
            return TRUE;
        default:
            return FALSE;
    }
}

static int
D3D11_GetViewportAlignedD3DRect(SDL_Renderer * renderer, const SDL_Rect * sdlRect, D3D11_RECT * outRect)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    switch (data->rotation) {
        case DXGI_MODE_ROTATION_IDENTITY:
            outRect->left = sdlRect->x;
            outRect->right = sdlRect->x + sdlRect->w;
            outRect->top = sdlRect->y;
            outRect->bottom = sdlRect->y + sdlRect->h;
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            outRect->left = sdlRect->y;
            outRect->right = sdlRect->y + sdlRect->h;
            outRect->top = renderer->viewport.w - sdlRect->x - sdlRect->w;
            outRect->bottom = renderer->viewport.w - sdlRect->x;
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            outRect->left = renderer->viewport.w - sdlRect->x - sdlRect->w;
            outRect->right = renderer->viewport.w - sdlRect->x;
            outRect->top = renderer->viewport.h - sdlRect->y - sdlRect->h;
            outRect->bottom = renderer->viewport.h - sdlRect->y;
            break;
        case DXGI_MODE_ROTATION_ROTATE90:
            outRect->left = renderer->viewport.h - sdlRect->y - sdlRect->h;
            outRect->right = renderer->viewport.h - sdlRect->y;
            outRect->top = sdlRect->x;
            outRect->bottom = sdlRect->x + sdlRect->h;
            break;
        default:
            return SDL_SetError("The physical display is in an unknown or unsupported rotation");
    }
    return 0;
}

static HRESULT
D3D11_CreateSwapChain(SDL_Renderer * renderer, int w, int h)
{
    D3D11_RenderData *data = (D3D11_RenderData *)renderer->driverdata;
#ifdef __WINRT__
    IUnknown *coreWindow = D3D11_GetCoreWindowFromSDLRenderer(renderer);
    const BOOL usingXAML = (coreWindow == NULL);
#else
    IUnknown *coreWindow = NULL;
    const BOOL usingXAML = FALSE;
#endif
    IDXGIDevice1 *dxgiDevice = NULL;
    IDXGIAdapter *dxgiAdapter = NULL;
    IDXGIFactory2 *dxgiFactory = NULL;
    HRESULT result = S_OK;

    /* Create a swap chain using the same adapter as the existing Direct3D device. */
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    SDL_zero(swapChainDesc);
    swapChainDesc.Width = w;
    swapChainDesc.Height = h;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* This is the most common swap chain format. */
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1; /* Don't use multi-sampling. */
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2; /* Use double-buffering to minimize latency. */
#if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH; /* On phone, only stretch and aspect-ratio stretch scaling are allowed. */
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; /* On phone, no swap effects are supported. */
#else
    if (usingXAML) {
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    } else {
        swapChainDesc.Scaling = DXGI_SCALING_NONE;
    }
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; /* All Windows Store apps must use this SwapEffect. */
#endif
    swapChainDesc.Flags = 0;

    result = ID3D11Device_QueryInterface(data->d3dDevice, &IID_IDXGIDevice1, &dxgiDevice);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device to IDXGIDevice1", result);
        goto done;
    }

    result = IDXGIDevice1_GetAdapter(dxgiDevice, &dxgiAdapter);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIDevice1::GetAdapter", result);
        goto done;
    }

    result = IDXGIAdapter_GetParent(dxgiAdapter, &IID_IDXGIFactory2, &dxgiFactory);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIAdapter::GetParent", result);
        goto done;
    }

    if (coreWindow) {
        result = IDXGIFactory2_CreateSwapChainForCoreWindow(dxgiFactory,
            (IUnknown *)data->d3dDevice,
            coreWindow,
            &swapChainDesc,
            NULL, /* Allow on all displays. */
            &data->swapChain
            );
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIFactory2::CreateSwapChainForCoreWindow", result);
            goto done;
        }
    } else if (usingXAML) {
        result = IDXGIFactory2_CreateSwapChainForComposition(dxgiFactory,
            (IUnknown *)data->d3dDevice,
            &swapChainDesc,
            NULL,
            &data->swapChain);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIFactory2::CreateSwapChainForComposition", result);
            goto done;
        }

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
        result = WINRT_GlobalSwapChainBackgroundPanelNative->SetSwapChain(data->swapChain);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", ISwapChainBackgroundPanelNative::SetSwapChain", result);
            return result;
        }
#else
        SDL_SetError(__FUNCTION__ ", XAML support is not yet available for Windows Phone");
        result = E_FAIL;
        goto done;
#endif
    } else {
        SDL_SysWMinfo windowinfo;
        SDL_VERSION(&windowinfo.version);
        SDL_GetWindowWMInfo(renderer->window, &windowinfo);

        result = IDXGIFactory2_CreateSwapChainForHwnd(dxgiFactory,
            (IUnknown *)data->d3dDevice,
            windowinfo.info.win.window,
            &swapChainDesc,
            NULL,
            NULL, /* Allow on all displays. */
            &data->swapChain
            );
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIFactory2::CreateSwapChainForHwnd", result);
            goto done;
        }
    }
    data->swapEffect = swapChainDesc.SwapEffect;

    /* Ensure that DXGI does not queue more than one frame at a time. This both reduces latency and
     * ensures that the application will only render after each VSync, minimizing power consumption.
     */
    result = IDXGIDevice1_SetMaximumFrameLatency(dxgiDevice, 1);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIDevice1::SetMaximumFrameLatency", result);
        goto done;
    }

done:
    SAFE_RELEASE(coreWindow);
    SAFE_RELEASE(dxgiDevice);
    SAFE_RELEASE(dxgiAdapter);
    SAFE_RELEASE(dxgiFactory);
    return result;
}


/* Initialize all resources that change when the window's size changes. */
static HRESULT
D3D11_CreateWindowSizeDependentResources(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *)renderer->driverdata;
    ID3D11Texture2D *backBuffer = NULL;
    HRESULT result = S_OK;
    int w, h;

    /* Release the previous render target view */
    D3D11_ReleaseMainRenderTargetView(renderer);

    /* The width and height of the swap chain must be based on the window's
     * landscape-oriented width and height. If the window is in a portrait
     * rotation, the dimensions must be reversed.
     */
    SDL_GetWindowSize(renderer->window, &w, &h);
    data->rotation = D3D11_GetCurrentRotation();

#if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
    const BOOL swapDimensions = FALSE;
#else
    const BOOL swapDimensions = D3D11_IsDisplayRotated90Degrees(data->rotation);
#endif
    if (swapDimensions) {
        int tmp = w;
        w = h;
        h = tmp;
    }

    if (data->swapChain) {
        /* If the swap chain already exists, resize it. */
        result = IDXGISwapChain_ResizeBuffers(data->swapChain,
            0,
            w, h,
            DXGI_FORMAT_UNKNOWN,
            0
            );
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGISwapChain::ResizeBuffers", result);
            goto done;
        }
    } else {
        result = D3D11_CreateSwapChain(renderer, w, h);
        if (FAILED(result)) {
            goto done;
        }
    }
    
#if WINAPI_FAMILY != WINAPI_FAMILY_PHONE_APP
    /* Set the proper rotation for the swap chain, and generate the
     * 3D matrix transformation for rendering to the rotated swap chain.
     *
     * To note, the call for this, IDXGISwapChain1::SetRotation, is not necessary
     * on Windows Phone, nor is it supported there.  It's only needed in Windows 8/RT.
     */
    if (data->swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
    {
        result = IDXGISwapChain1_SetRotation(data->swapChain, data->rotation);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGISwapChain1::SetRotation", result);
            goto done;
        }
    }
#endif

    result = IDXGISwapChain_GetBuffer(data->swapChain,
        0,
        &IID_ID3D11Texture2D,
        &backBuffer
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGISwapChain::GetBuffer [back-buffer]", result);
        goto done;
    }

    /* Create a render target view of the swap chain back buffer. */
    result = ID3D11Device_CreateRenderTargetView(data->d3dDevice,
        (ID3D11Resource *)backBuffer,
        NULL,
        &data->mainRenderTargetView
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device::CreateRenderTargetView", result);
        goto done;
    }

    if (D3D11_UpdateViewport(renderer) != 0) {
        /* D3D11_UpdateViewport will set the SDL error if it fails. */
        result = E_FAIL;
        goto done;
    }

done:
    SAFE_RELEASE(backBuffer);
    return result;
}

/* This method is called when the window's size changes. */
static HRESULT
D3D11_UpdateForWindowSizeChange(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *)renderer->driverdata;
    /* FIXME: Do we need to release render targets like we do in D3D9? */
    return D3D11_CreateWindowSizeDependentResources(renderer);
}

HRESULT
D3D11_HandleDeviceLost(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    HRESULT result = S_OK;

    /* FIXME: Need to release all resources - all texures are invalid! */

    result = D3D11_CreateDeviceResources(renderer);
    if (FAILED(result)) {
        /* D3D11_CreateDeviceResources will set the SDL error */
        return result;
    }

    result = D3D11_UpdateForWindowSizeChange(renderer);
    if (FAILED(result)) {
        /* D3D11_UpdateForWindowSizeChange will set the SDL error */
        return result;
    }

    return S_OK;
}

static void
D3D11_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{
    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        D3D11_UpdateForWindowSizeChange(renderer);
    }
}

static D3D11_FILTER
GetScaleQuality(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
    if (!hint || *hint == '0' || SDL_strcasecmp(hint, "nearest") == 0) {
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    } else /* if (*hint == '1' || SDL_strcasecmp(hint, "linear") == 0) */ {
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

static int
D3D11_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData;
    HRESULT result;
    DXGI_FORMAT textureFormat = SDLPixelFormatToDXGIFormat(texture->format);
    if (textureFormat == SDL_PIXELFORMAT_UNKNOWN) {
        return SDL_SetError("%s, An unsupported SDL pixel format (0x%x) was specified",
            __FUNCTION__, texture->format);
    }

    textureData = (D3D11_TextureData*) SDL_calloc(1, sizeof(*textureData));
    if (!textureData) {
        SDL_OutOfMemory();
        return -1;
    }
    textureData->scaleMode = GetScaleQuality();

    texture->driverdata = textureData;

    D3D11_TEXTURE2D_DESC textureDesc;
    SDL_zero(textureDesc);
    textureDesc.Width = texture->w;
    textureDesc.Height = texture->h;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = textureFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.MiscFlags = 0;

    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        textureDesc.Usage = D3D11_USAGE_DYNAMIC;
        textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else {
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.CPUAccessFlags = 0;
    }

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    } else {
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    }

    result = ID3D11Device_CreateTexture2D(rendererData->d3dDevice,
        &textureDesc,
        NULL,
        &textureData->mainTexture
        );
    if (FAILED(result)) {
        D3D11_DestroyTexture(renderer, texture);
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateTexture2D", result);
        return -1;
    }

    if (texture->access & SDL_TEXTUREACCESS_TARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
        renderTargetViewDesc.Format = textureDesc.Format;
        renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        renderTargetViewDesc.Texture2D.MipSlice = 0;

        result = ID3D11Device_CreateRenderTargetView(rendererData->d3dDevice,
            (ID3D11Resource *)textureData->mainTexture,
            &renderTargetViewDesc,
            &textureData->mainTextureRenderTargetView);
        if (FAILED(result)) {
            D3D11_DestroyTexture(renderer, texture);
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateRenderTargetView", result);
            return -1;
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;
    resourceViewDesc.Format = textureDesc.Format;
    resourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    resourceViewDesc.Texture2D.MostDetailedMip = 0;
    resourceViewDesc.Texture2D.MipLevels = textureDesc.MipLevels;
    result = ID3D11Device_CreateShaderResourceView(rendererData->d3dDevice,
        (ID3D11Resource *)textureData->mainTexture,
        &resourceViewDesc,
        &textureData->mainTextureResourceView
        );
    if (FAILED(result)) {
        D3D11_DestroyTexture(renderer, texture);
        WIN_SetErrorFromHRESULT(__FUNCTION__ "ID3D11Device1::CreateShaderResourceView", result);
        return -1;
    }

    return 0;
}

static void
D3D11_DestroyTexture(SDL_Renderer * renderer,
                     SDL_Texture * texture)
{
    D3D11_TextureData *data = (D3D11_TextureData *)texture->driverdata;

    if (!data) {
        return;
    }

    SAFE_RELEASE(data->mainTexture);
    SAFE_RELEASE(data->mainTextureResourceView);
    SAFE_RELEASE(data->mainTextureRenderTargetView);
    SAFE_RELEASE(data->stagingTexture);
    SDL_free(data);
    texture->driverdata = NULL;
}

static int
D3D11_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                    const SDL_Rect * rect, const void * srcPixels,
                    int srcPitch)
{
    /* Lock the texture, retrieving a buffer to write pixel data to: */
    void * destPixels = NULL;
    int destPitch = 0;
    if (D3D11_LockTexture(renderer, texture, rect, &destPixels, &destPitch) != 0) {
        /* An error is already set.  Attach some info to it, then return to the caller. */
        char errorMessage[1024];
        SDL_snprintf(errorMessage, sizeof(errorMessage), __FUNCTION__ ", Lock Texture Failed: %s", SDL_GetError());
        return SDL_SetError(errorMessage);
    }

    /* Copy pixel data to the locked texture's memory: */
    for (int y = 0; y < rect->h; ++y) {
        SDL_memcpy(
            ((Uint8 *)destPixels) + (destPitch * y),
            ((Uint8 *)srcPixels) + (srcPitch * y),
            srcPitch
            );
    }

    /* Commit the texture's memory back to Direct3D: */
    D3D11_UnlockTexture(renderer, texture);

    return 0;
}

static int
D3D11_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                  const SDL_Rect * rect, void **pixels, int *pitch)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;
    HRESULT result = S_OK;

    if (textureData->stagingTexture) {
        return SDL_SetError("texture is already locked");
    }
    
    /* Create a 'staging' texture, which will be used to write to a portion
     * of the main texture.  This is necessary, as Direct3D 11.1 does not
     * have the ability to write a CPU-bound pixel buffer to a rectangular
     * subrect of a texture.  Direct3D 11.1 can, however, write a pixel
     * buffer to an entire texture, hence the use of a staging texture.
     *
     * TODO, WinRT: consider avoiding the use of a staging texture in D3D11_LockTexture if/when the entire texture is being updated
     */
    D3D11_TEXTURE2D_DESC stagingTextureDesc;
    ID3D11Texture2D_GetDesc(textureData->mainTexture, &stagingTextureDesc);
    stagingTextureDesc.Width = rect->w;
    stagingTextureDesc.Height = rect->h;
    stagingTextureDesc.BindFlags = 0;
    stagingTextureDesc.MiscFlags = 0;
    stagingTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    stagingTextureDesc.Usage = D3D11_USAGE_STAGING;
    result = ID3D11Device_CreateTexture2D(rendererData->d3dDevice,
        &stagingTextureDesc,
        NULL,
        &textureData->stagingTexture);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateTexture2D [create staging texture]", result);
        return -1;
    }

    /* Get a write-only pointer to data in the staging texture: */
    D3D11_MAPPED_SUBRESOURCE textureMemory;
    result = ID3D11DeviceContext_Map(rendererData->d3dContext,
        (ID3D11Resource *)textureData->stagingTexture,
        0,
        D3D11_MAP_WRITE,
        0,
        &textureMemory
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11DeviceContext1::Map [map staging texture]", result);
        SAFE_RELEASE(textureData->stagingTexture);
        return -1;
    }

    /* Make note of where the staging texture will be written to 
     * (on a call to SDL_UnlockTexture):
     */
    textureData->lockedTexturePositionX = rect->x;
    textureData->lockedTexturePositionY = rect->y;

    /* Make sure the caller has information on the texture's pixel buffer,
     * then return:
     */
    *pixels = textureMemory.pData;
    *pitch = textureMemory.RowPitch;
    return 0;
}

static void
D3D11_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;

    /* Commit the pixel buffer's changes back to the staging texture: */
    ID3D11DeviceContext_Unmap(rendererData->d3dContext,
        (ID3D11Resource *)textureData->stagingTexture,
        0);

    /* Copy the staging texture's contents back to the main texture: */
    ID3D11DeviceContext_CopySubresourceRegion(rendererData->d3dContext,
        (ID3D11Resource *)textureData->mainTexture,
        0,
        textureData->lockedTexturePositionX,
        textureData->lockedTexturePositionY,
        0,
        (ID3D11Resource *)textureData->stagingTexture,
        0,
        NULL);

    SAFE_RELEASE(textureData->stagingTexture);
}

static int
D3D11_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;

    if (texture == NULL) {
        rendererData->currentOffscreenRenderTargetView = NULL;
        return 0;
    }

    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;

    if (!textureData->mainTextureRenderTargetView) {
        return SDL_SetError("specified texture is not a render target");
    }

    rendererData->currentOffscreenRenderTargetView = textureData->mainTextureRenderTargetView;

    return 0;
}

static void
D3D11_SetModelMatrix(SDL_Renderer *renderer, const Float4X4 *matrix)
{
    D3D11_RenderData *data = (D3D11_RenderData *)renderer->driverdata;

    if (matrix) {
        data->vertexShaderConstantsData.model = *matrix;
    } else {
        data->vertexShaderConstantsData.model = MatrixIdentity();
    }

    ID3D11DeviceContext_UpdateSubresource(data->d3dContext,
        (ID3D11Resource *)data->vertexShaderConstants,
        0,
        NULL,
        &data->vertexShaderConstantsData,
        0,
        0
        );
}

static int
D3D11_UpdateViewport(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;

    if (renderer->viewport.w == 0 || renderer->viewport.h == 0) {
        /* If the viewport is empty, assume that it is because
         * SDL_CreateRenderer is calling it, and will call it again later
         * with a non-empty viewport.
         */
        return 0;
    }

    /* Make sure the SDL viewport gets rotated to that of the physical display's rotation.
     * Keep in mind here that the Y-axis will be been inverted (from Direct3D's
     * default coordinate system) so rotations will be done in the opposite
     * direction of the DXGI_MODE_ROTATION enumeration.
     */
    Float4X4 projection;
    switch (data->rotation)
    {
        case DXGI_MODE_ROTATION_IDENTITY:
            projection = MatrixIdentity();
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            projection = MatrixRotationZ(SDL_static_cast(float, M_PI * 0.5f));
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            projection = MatrixRotationZ(SDL_static_cast(float, M_PI));
            break;
        case DXGI_MODE_ROTATION_ROTATE90:
            projection = MatrixRotationZ(SDL_static_cast(float, -M_PI * 0.5f));
            break;
        default:
            return SDL_SetError("An unknown DisplayOrientation is being used");
    }

    /* Update the view matrix */
    Float4X4 view;
    view.m[0][0] = 2.0f / renderer->viewport.w;
    view.m[0][1] = 0.0f;
    view.m[0][2] = 0.0f;
    view.m[0][3] = 0.0f;
    view.m[1][0] = 0.0f;
    view.m[1][1] = -2.0f / renderer->viewport.h;
    view.m[1][2] = 0.0f;
    view.m[1][3] = 0.0f;
    view.m[2][0] = 0.0f;
    view.m[2][1] = 0.0f;
    view.m[2][2] = 1.0f;
    view.m[2][3] = 0.0f;
    view.m[3][0] = -1.0f;
    view.m[3][1] = 1.0f;
    view.m[3][2] = 0.0f;
    view.m[3][3] = 1.0f;

    /* Combine the projection + view matrix together now, as both only get
     * set here (as of this writing, on Dec 26, 2013).  When done, store it
     * for eventual transfer to the GPU.
     */
    data->vertexShaderConstantsData.projectionAndView = MatrixMultiply(
            view,
            projection);

    /* Reset the model matrix */
    D3D11_SetModelMatrix(renderer, NULL);

    /* Update the Direct3D viewport, which seems to be aligned to the
     * swap buffer's coordinate space, which is always in either
     * a landscape mode, for all Windows 8/RT devices, or a portrait mode,
     * for Windows Phone devices.
     */
    SDL_FRect orientationAlignedViewport;
    const BOOL swapDimensions = D3D11_IsDisplayRotated90Degrees(data->rotation);
    if (swapDimensions) {
        orientationAlignedViewport.x = (float) renderer->viewport.y;
        orientationAlignedViewport.y = (float) renderer->viewport.x;
        orientationAlignedViewport.w = (float) renderer->viewport.h;
        orientationAlignedViewport.h = (float) renderer->viewport.w;
    } else {
        orientationAlignedViewport.x = (float) renderer->viewport.x;
        orientationAlignedViewport.y = (float) renderer->viewport.y;
        orientationAlignedViewport.w = (float) renderer->viewport.w;
        orientationAlignedViewport.h = (float) renderer->viewport.h;
    }
    /* TODO, WinRT: get custom viewports working with non-Landscape modes (Portrait, PortraitFlipped, and LandscapeFlipped) */

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = orientationAlignedViewport.x;
    viewport.TopLeftY = orientationAlignedViewport.y;
    viewport.Width = orientationAlignedViewport.w;
    viewport.Height = orientationAlignedViewport.h;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(data->d3dContext, 1, &viewport);

    return 0;
}

static int
D3D11_UpdateClipRect(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    const SDL_Rect *rect = &renderer->clip_rect;

    if (SDL_RectEmpty(rect)) {
        ID3D11DeviceContext_RSSetScissorRects(data->d3dContext, 0, NULL);
    } else {
        D3D11_RECT scissorRect;
        if (D3D11_GetViewportAlignedD3DRect(renderer, rect, &scissorRect) != 0) {
            /* D3D11_GetViewportAlignedD3DRect will have set the SDL error */
            return -1;
        }
        ID3D11DeviceContext_RSSetScissorRects(data->d3dContext, 1, &scissorRect);
    }

    return 0;
}

static void
D3D11_ReleaseMainRenderTargetView(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *)renderer->driverdata;
    ID3D11DeviceContext_OMSetRenderTargets(data->d3dContext, 0, NULL, NULL);
    SAFE_RELEASE(data->mainRenderTargetView);
}

static ID3D11RenderTargetView *
D3D11_GetCurrentRenderTargetView(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    if (data->currentOffscreenRenderTargetView) {
        return data->currentOffscreenRenderTargetView;
    } else {
        return data->mainRenderTargetView;
    }
}

static int
D3D11_RenderClear(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    const float colorRGBA[] = {
        (renderer->r / 255.0f),
        (renderer->g / 255.0f),
        (renderer->b / 255.0f),
        (renderer->a / 255.0f)
    };
    ID3D11DeviceContext_ClearRenderTargetView(data->d3dContext,
        D3D11_GetCurrentRenderTargetView(renderer),
        colorRGBA
        );
    return 0;
}

static int
D3D11_UpdateVertexBuffer(SDL_Renderer *renderer,
                         const void * vertexData, size_t dataSizeInBytes)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_BUFFER_DESC vertexBufferDesc;
    HRESULT result = S_OK;

    if (rendererData->vertexBuffer) {
        ID3D11Buffer_GetDesc(rendererData->vertexBuffer, &vertexBufferDesc);
    } else {
        SDL_zero(vertexBufferDesc);
    }

    if (rendererData->vertexBuffer && vertexBufferDesc.ByteWidth >= dataSizeInBytes) {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        result = ID3D11DeviceContext_Map(rendererData->d3dContext,
            (ID3D11Resource *)rendererData->vertexBuffer,
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mappedResource
            );
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11DeviceContext1::Map [vertex buffer]", result);
            return -1;
        }
        SDL_memcpy(mappedResource.pData, vertexData, dataSizeInBytes);
        ID3D11DeviceContext_Unmap(rendererData->d3dContext, (ID3D11Resource *)rendererData->vertexBuffer, 0);
    } else {
        SAFE_RELEASE(rendererData->vertexBuffer);

        vertexBufferDesc.ByteWidth = dataSizeInBytes;
        vertexBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_SUBRESOURCE_DATA vertexBufferData;
        SDL_zero(vertexBufferData);
        vertexBufferData.pSysMem = vertexData;
        vertexBufferData.SysMemPitch = 0;
        vertexBufferData.SysMemSlicePitch = 0;

        result = ID3D11Device_CreateBuffer(rendererData->d3dDevice,
            &vertexBufferDesc,
            &vertexBufferData,
            &rendererData->vertexBuffer
            );
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateBuffer [vertex buffer]", result);
            return -1;
        }

        UINT stride = sizeof(VertexPositionColor);
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(rendererData->d3dContext,
            0,
            1,
            &rendererData->vertexBuffer,
            &stride,
            &offset
            );
    }

    return 0;
}

static void
D3D11_RenderStartDrawOp(SDL_Renderer * renderer)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *)renderer->driverdata;
    ID3D11RenderTargetView *renderTargetView = D3D11_GetCurrentRenderTargetView(renderer);
    if (renderTargetView != rendererData->currentRenderTargetView) {
        ID3D11DeviceContext_OMSetRenderTargets(rendererData->d3dContext,
            1,
            &renderTargetView,
            NULL
            );
        rendererData->currentRenderTargetView = renderTargetView;
    }

    ID3D11RasterizerState *rasterizerState;
    if (SDL_RectEmpty(&renderer->clip_rect)) {
        rasterizerState = rendererData->mainRasterizer;
    } else {
        rasterizerState = rendererData->clippedRasterizer;
    }
    if (rasterizerState != rendererData->currentRasterizerState) {
        ID3D11DeviceContext_RSSetState(rendererData->d3dContext, rasterizerState);
        rendererData->currentRasterizerState = rasterizerState;
    }
}

static void
D3D11_RenderSetBlendMode(SDL_Renderer * renderer, SDL_BlendMode blendMode)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *)renderer->driverdata;
    ID3D11BlendState *blendState;
    switch (blendMode) {
    case SDL_BLENDMODE_BLEND:
        blendState = rendererData->blendModeBlend;
        break;
    case SDL_BLENDMODE_ADD:
        blendState = rendererData->blendModeAdd;
        break;
    case SDL_BLENDMODE_MOD:
        blendState = rendererData->blendModeMod;
        break;
    case SDL_BLENDMODE_NONE:
        blendState = NULL;
        break;
    }
    if (blendState != rendererData->currentBlendState) {
        ID3D11DeviceContext_OMSetBlendState(rendererData->d3dContext, blendState, 0, 0xFFFFFFFF);
        rendererData->currentBlendState = blendState;
    }
}

static void
D3D11_SetPixelShader(SDL_Renderer * renderer,
                     ID3D11PixelShader * shader,
                     ID3D11ShaderResourceView * shaderResource,
                     ID3D11SamplerState * sampler)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    if (shader != rendererData->currentShader) {
        ID3D11DeviceContext_PSSetShader(rendererData->d3dContext, shader, NULL, 0);
        rendererData->currentShader = shader;
    }
    if (shaderResource != rendererData->currentShaderResource) {
        ID3D11DeviceContext_PSSetShaderResources(rendererData->d3dContext, 0, 1, &shaderResource);
        rendererData->currentShaderResource = shaderResource;
    }
    if (sampler != rendererData->currentSampler) {
        ID3D11DeviceContext_PSSetSamplers(rendererData->d3dContext, 0, 1, &sampler);
        rendererData->currentSampler = sampler;
    }
}

static void
D3D11_RenderFinishDrawOp(SDL_Renderer * renderer,
                         D3D11_PRIMITIVE_TOPOLOGY primitiveTopology,
                         UINT vertexCount)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;

    ID3D11DeviceContext_IASetPrimitiveTopology(rendererData->d3dContext, primitiveTopology);
    ID3D11DeviceContext_Draw(rendererData->d3dContext, vertexCount, 0);
}

static int
D3D11_RenderDrawPoints(SDL_Renderer * renderer,
                       const SDL_FPoint * points, int count)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    float r, g, b, a;

    r = (float)(renderer->r / 255.0f);
    g = (float)(renderer->g / 255.0f);
    b = (float)(renderer->b / 255.0f);
    a = (float)(renderer->a / 255.0f);

    VertexPositionColor * vertices = SDL_stack_alloc(VertexPositionColor, count);
    for (int i = 0; i < min(count, 128); ++i) {
        const VertexPositionColor v = { { points[i].x, points[i].y, 0.0f }, { 0.0f, 0.0f }, { r, g, b, a } };
        vertices[i] = v;
    }

    D3D11_RenderStartDrawOp(renderer);
    D3D11_RenderSetBlendMode(renderer, renderer->blendMode);
    if (D3D11_UpdateVertexBuffer(renderer, vertices, (unsigned int)count * sizeof(VertexPositionColor)) != 0) {
        SDL_stack_free(vertices);
        return -1;
    }

    D3D11_SetPixelShader(
        renderer,
        rendererData->colorPixelShader,
        NULL,
        NULL);

    D3D11_RenderFinishDrawOp(renderer, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, count);
    SDL_stack_free(vertices);
    return 0;
}

static int
D3D11_RenderDrawLines(SDL_Renderer * renderer,
                      const SDL_FPoint * points, int count)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    float r, g, b, a;

    r = (float)(renderer->r / 255.0f);
    g = (float)(renderer->g / 255.0f);
    b = (float)(renderer->b / 255.0f);
    a = (float)(renderer->a / 255.0f);

    VertexPositionColor * vertices = SDL_stack_alloc(VertexPositionColor, count);
    for (int i = 0; i < count; ++i) {
        const VertexPositionColor v = { { points[i].x, points[i].y, 0.0f }, { 0.0f, 0.0f }, { r, g, b, a } };
        vertices[i] = v;
    }

    D3D11_RenderStartDrawOp(renderer);
    D3D11_RenderSetBlendMode(renderer, renderer->blendMode);
    if (D3D11_UpdateVertexBuffer(renderer, vertices, (unsigned int)count * sizeof(VertexPositionColor)) != 0) {
        SDL_stack_free(vertices);
        return -1;
    }

    D3D11_SetPixelShader(
        renderer,
        rendererData->colorPixelShader,
        NULL,
        NULL);

    D3D11_RenderFinishDrawOp(renderer, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, count);
    SDL_stack_free(vertices);
    return 0;
}

static int
D3D11_RenderFillRects(SDL_Renderer * renderer,
                      const SDL_FRect * rects, int count)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    float r, g, b, a;

    r = (float)(renderer->r / 255.0f);
    g = (float)(renderer->g / 255.0f);
    b = (float)(renderer->b / 255.0f);
    a = (float)(renderer->a / 255.0f);

    for (int i = 0; i < count; ++i) {
        D3D11_RenderStartDrawOp(renderer);
        D3D11_RenderSetBlendMode(renderer, renderer->blendMode);

        VertexPositionColor vertices[] = {
            { { rects[i].x, rects[i].y, 0.0f },                             { 0.0f, 0.0f}, {r, g, b, a} },
            { { rects[i].x, rects[i].y + rects[i].h, 0.0f },                { 0.0f, 0.0f }, { r, g, b, a } },
            { { rects[i].x + rects[i].w, rects[i].y, 0.0f },                { 0.0f, 0.0f }, { r, g, b, a } },
            { { rects[i].x + rects[i].w, rects[i].y + rects[i].h, 0.0f },   { 0.0f, 0.0f }, { r, g, b, a } },
        };
        if (D3D11_UpdateVertexBuffer(renderer, vertices, sizeof(vertices)) != 0) {
            return -1;
        }

        D3D11_SetPixelShader(
            renderer,
            rendererData->colorPixelShader,
            NULL,
            NULL);

        D3D11_RenderFinishDrawOp(renderer, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, SDL_arraysize(vertices));
    }

    return 0;
}

static ID3D11SamplerState *
D3D11_RenderGetSampler(SDL_Renderer * renderer, SDL_Texture * texture)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;

    switch (textureData->scaleMode) {
    case D3D11_FILTER_MIN_MAG_MIP_POINT:
        return rendererData->nearestPixelSampler;
    case D3D11_FILTER_MIN_MAG_MIP_LINEAR:
        return rendererData->linearSampler;
    default:
        return NULL;
    }
}

static int
D3D11_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                 const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;

    D3D11_RenderStartDrawOp(renderer);
    D3D11_RenderSetBlendMode(renderer, texture->blendMode);

    float minu = (float) srcrect->x / texture->w;
    float maxu = (float) (srcrect->x + srcrect->w) / texture->w;
    float minv = (float) srcrect->y / texture->h;
    float maxv = (float) (srcrect->y + srcrect->h) / texture->h;

    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    if (texture->modMode & SDL_TEXTUREMODULATE_COLOR) {
        r = (float)(texture->r / 255.0f);
        g = (float)(texture->g / 255.0f);
        b = (float)(texture->b / 255.0f);
    }
    if (texture->modMode & SDL_TEXTUREMODULATE_ALPHA) {
        a = (float)(texture->a / 255.0f);
    }

    VertexPositionColor vertices[] = {
        { { dstrect->x, dstrect->y, 0.0f },                             { minu, minv }, { r, g, b, a } },
        { { dstrect->x, dstrect->y + dstrect->h, 0.0f },                { minu, maxv }, { r, g, b, a } },
        { { dstrect->x + dstrect->w, dstrect->y, 0.0f },                { maxu, minv }, { r, g, b, a } },
        { { dstrect->x + dstrect->w, dstrect->y + dstrect->h, 0.0f },   { maxu, maxv }, { r, g, b, a } },
    };
    if (D3D11_UpdateVertexBuffer(renderer, vertices, sizeof(vertices)) != 0) {
        return -1;
    }

    ID3D11SamplerState *textureSampler = D3D11_RenderGetSampler(renderer, texture);
    D3D11_SetPixelShader(
        renderer,
        rendererData->texturePixelShader,
        textureData->mainTextureResourceView,
        textureSampler);

    D3D11_RenderFinishDrawOp(renderer, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, sizeof(vertices) / sizeof(VertexPositionColor));

    return 0;
}

static int
D3D11_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                   const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                   const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip)
{
    D3D11_RenderData *rendererData = (D3D11_RenderData *) renderer->driverdata;
    D3D11_TextureData *textureData = (D3D11_TextureData *) texture->driverdata;

    D3D11_RenderStartDrawOp(renderer);
    D3D11_RenderSetBlendMode(renderer, texture->blendMode);

    float minu = (float) srcrect->x / texture->w;
    float maxu = (float) (srcrect->x + srcrect->w) / texture->w;
    float minv = (float) srcrect->y / texture->h;
    float maxv = (float) (srcrect->y + srcrect->h) / texture->h;

    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    if (texture->modMode & SDL_TEXTUREMODULATE_COLOR) {
        r = (float)(texture->r / 255.0f);
        g = (float)(texture->g / 255.0f);
        b = (float)(texture->b / 255.0f);
    }
    if (texture->modMode & SDL_TEXTUREMODULATE_ALPHA) {
        a = (float)(texture->a / 255.0f);
    }

    if (flip & SDL_FLIP_HORIZONTAL) {
        float tmp = maxu;
        maxu = minu;
        minu = tmp;
    }
    if (flip & SDL_FLIP_VERTICAL) {
        float tmp = maxv;
        maxv = minv;
        minv = tmp;
    }

    Float4X4 oldModelMatrix = rendererData->vertexShaderConstantsData.model;
    Float4X4 newModelMatrix = MatrixMultiply(
            MatrixRotationZ((float)(M_PI * (float) angle / 180.0f)),
            MatrixTranslation(dstrect->x + center->x, dstrect->y + center->y, 0)
            );
    D3D11_SetModelMatrix(renderer, &newModelMatrix);

    const float minx = -center->x;
    const float maxx = dstrect->w - center->x;
    const float miny = -center->y;
    const float maxy = dstrect->h - center->y;

    VertexPositionColor vertices[] = {
        {{minx, miny, 0.0f}, {minu, minv}, {r, g, b, a}},
        {{minx, maxy, 0.0f}, {minu, maxv}, {r, g, b, a}},
        {{maxx, miny, 0.0f}, {maxu, minv}, {r, g, b, a}},
        {{maxx, maxy, 0.0f}, {maxu, maxv}, {r, g, b, a}},
    };
    if (D3D11_UpdateVertexBuffer(renderer, vertices, sizeof(vertices)) != 0) {
        return -1;
    }

    ID3D11SamplerState *textureSampler = D3D11_RenderGetSampler(renderer, texture);
    D3D11_SetPixelShader(
        renderer,
        rendererData->texturePixelShader,
        textureData->mainTextureResourceView,
        textureSampler);

    D3D11_RenderFinishDrawOp(renderer, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, sizeof(vertices) / sizeof(VertexPositionColor));

    D3D11_SetModelMatrix(renderer, &oldModelMatrix);

    return 0;
}

static int
D3D11_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                       Uint32 format, void * pixels, int pitch)
{
    D3D11_RenderData * data = (D3D11_RenderData *) renderer->driverdata;
    ID3D11Texture2D *backBuffer = NULL;
    ID3D11Texture2D *stagingTexture = NULL;
    HRESULT result;
    int status = -1;

    /* Retrieve a pointer to the back buffer: */
    result = IDXGISwapChain_GetBuffer(data->swapChain,
        0,
        &IID_ID3D11Texture2D,
        &backBuffer
        );
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGISwapChain1::GetBuffer [get back buffer]", result);
        goto done;
    }

    /* Create a staging texture to copy the screen's data to: */
    D3D11_TEXTURE2D_DESC stagingTextureDesc;
    ID3D11Texture2D_GetDesc(backBuffer, &stagingTextureDesc);
    stagingTextureDesc.Width = rect->w;
    stagingTextureDesc.Height = rect->h;
    stagingTextureDesc.BindFlags = 0;
    stagingTextureDesc.MiscFlags = 0;
    stagingTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingTextureDesc.Usage = D3D11_USAGE_STAGING;
    result = ID3D11Device_CreateTexture2D(data->d3dDevice,
        &stagingTextureDesc,
        NULL,
        &stagingTexture);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11Device1::CreateTexture2D [create staging texture]", result);
        goto done;
    }

    /* Copy the desired portion of the back buffer to the staging texture: */
    D3D11_RECT srcRect;
    if (D3D11_GetViewportAlignedD3DRect(renderer, rect, &srcRect) != 0) {
        /* D3D11_GetViewportAlignedD3DRect will have set the SDL error */
        goto done;
    }

    D3D11_BOX srcBox;
    srcBox.left = srcRect.left;
    srcBox.right = srcRect.right;
    srcBox.top = srcRect.top;
    srcBox.bottom = srcRect.bottom;
    srcBox.front = 0;
    srcBox.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(data->d3dContext,
        (ID3D11Resource *)stagingTexture,
        0,
        0, 0, 0,
        (ID3D11Resource *)backBuffer,
        0,
        &srcBox);

    /* Map the staging texture's data to CPU-accessible memory: */
    D3D11_MAPPED_SUBRESOURCE textureMemory;
    result = ID3D11DeviceContext_Map(data->d3dContext,
        (ID3D11Resource *)stagingTexture,
        0,
        D3D11_MAP_READ,
        0,
        &textureMemory);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", ID3D11DeviceContext1::Map [map staging texture]", result);
        goto done;
    }

    /* Copy the data into the desired buffer, converting pixels to the
     * desired format at the same time:
     */
    if (SDL_ConvertPixels(
        rect->w, rect->h,
        DXGIFormatToSDLPixelFormat(stagingTextureDesc.Format),
        textureMemory.pData,
        textureMemory.RowPitch,
        format,
        pixels,
        pitch) != 0)
    {
        /* When SDL_ConvertPixels fails, it'll have already set the format.
         * Get the error message, and attach some extra data to it.
         */
        char errorMessage[1024];
        SDL_snprintf(errorMessage, sizeof(errorMessage), __FUNCTION__ ", Convert Pixels failed: %s", SDL_GetError());
        SDL_SetError(errorMessage);
        goto done;
    }

    /* Unmap the texture: */
    ID3D11DeviceContext_Unmap(data->d3dContext,
        (ID3D11Resource *)stagingTexture,
        0);

    status = 0;

done:
    SAFE_RELEASE(backBuffer);
    SAFE_RELEASE(stagingTexture);
    return status;
}

static void
D3D11_RenderPresent(SDL_Renderer * renderer)
{
    D3D11_RenderData *data = (D3D11_RenderData *) renderer->driverdata;
    UINT syncInterval;
    UINT presentFlags;

    if (renderer->info.flags & SDL_RENDERER_PRESENTVSYNC) {
        syncInterval = 1;
        presentFlags = 0;
    } else {
        syncInterval = 0;
        presentFlags = DXGI_PRESENT_DO_NOT_WAIT;
    }

#if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
    HRESULT result = IDXGISwapChain_Present(data->swapChain, syncInterval, presentFlags);
#else
    /* The application may optionally specify "dirty" or "scroll"
     * rects to improve efficiency in certain scenarios.
     * This option is not available on Windows Phone 8, to note.
     */
    DXGI_PRESENT_PARAMETERS parameters;
    SDL_zero(parameters);
    HRESULT result = IDXGISwapChain1_Present1(data->swapChain, syncInterval, presentFlags, &parameters);
#endif

    /* Discard the contents of the render target.
     * This is a valid operation only when the existing contents will be entirely
     * overwritten. If dirty or scroll rects are used, this call should be removed.
     */
    ID3D11DeviceContext1_DiscardView(data->d3dContext, (ID3D11View*)data->mainRenderTargetView);

    /* When the present flips, it unbinds the current view, so bind it again on the next draw call */
    data->currentRenderTargetView = NULL;

    if (FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING) {
        /* If the device was removed either by a disconnect or a driver upgrade, we 
         * must recreate all device resources.
         *
         * TODO, WinRT: consider throwing an exception if D3D11_RenderPresent fails, especially if there is a way to salvage debug info from users' machines
         */
        if (result == DXGI_ERROR_DEVICE_REMOVED) {
            D3D11_HandleDeviceLost(renderer);
        } else {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGISwapChain::Present", result);
        }
    }
}

#endif /* SDL_VIDEO_RENDER_D3D11 && !SDL_RENDER_DISABLED */

/* vi: set ts=4 sw=4 expandtab: */