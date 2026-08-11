#include "GpuWorldRenderer.h"

#include "GpuRenderDevice.h"

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	// Size of the shading / blending lookup window of x_BYTE_F6EE0_tablesx that
	// the rasteriser can address with its 16 bit (shade << 8 | texel) index.
	constexpr uint32_t ShadeTableWidth = 256;
	constexpr uint32_t ShadeTableHeight = 256;
	constexpr uint32_t ShadeTableBytes = ShadeTableWidth * ShadeTableHeight;

	// x_BYTE_F6EE0_tablesx holds the 64 shade ramps first and the 256x256
	// destination blend table from byte 16384 on.
	constexpr uint32_t BlendTableOffset = 16384;

	// The terrain atlas is always 256 bytes wide; tiles are placed inside it.
	constexpr uint32_t AtlasWidth = 256;

	// Above this size the atlas is only re-uploaded when the engine says it
	// changed, below it the per frame upload is cheap enough to always do.
	constexpr uint32_t AtlasAlwaysUploadLimit = 512u * 1024u;

	// The sprite atlas is repacked every frame with a simple shelf packer;
	// each entry keeps a one texel replicated border so interpolation error at
	// the quad edges can never sample a neighbouring entry.
	constexpr int SpriteAtlasWidth = 1024;
	constexpr int SpriteAtlasHeight = 2048;

	// Bit 24 of the packed vertex field marks "texel comes from the sprite
	// atlas, uv is absolute"; bits 0..7 then hold the sprite pixel mode
	// (dword0x01_rotIdx) instead of the terrain shading mode.
	constexpr uint32_t SpriteSourceFlag = 1u << 24;

	// Bit 25 marks the sky quad, which derives its texel from the pixel
	// position and the sky constants instead of from any uv.
	constexpr uint32_t SkySourceFlag = 1u << 25;

	constexpr const char* WorldVertexShaderSource = R"(
cbuffer WorldSettings : register(b0)
{
    float2 invViewportSize;
    float2 padding;
    int4 skyOrigin;   // beginX, beginY, sinRoll, cosRoll (16.16)
    int4 skyParams;   // textureSize, viewportX, viewportY, unused
};

struct VertexInput
{
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    float  shade    : TEXCOORD1;
    uint   texBase  : TEXCOORD2;
    uint   packed   : TEXCOORD3;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    noperspective float2 uv : TEXCOORD0;
    noperspective float shade : TEXCOORD1;
    nointerpolation uint texBase : TEXCOORD2;
    nointerpolation uint packed : TEXCOORD3;
};

VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.position = float4(
        input.position.x * invViewportSize.x - 1.0f,
        1.0f - input.position.y * invViewportSize.y,
        0.5f,
        1.0f);
    output.uv = input.uv;
    output.shade = input.shade;
    output.texBase = input.texBase;
    output.packed = input.packed;
    return output;
}
)";

	// Reproduces the pixel formulas of DrawTriangleInProjectionSpace_B6253 and
	// DrawSprite_41BD3.  Compiled twice: the ps_4_0 baseline writes SV_TARGET
	// and approximates destination blends against the background layer; with
	// USE_ROV=1 (ps_5_0) the index target is a rasterizer ordered view, so
	// destination reads see the exact painter-order pixel underneath and the
	// blend modes become bit-exact.
	constexpr const char* WorldPixelShaderSource = R"(
#ifndef USE_ROV
#define USE_ROV 0
#endif

Texture2D<float> AtlasTexture : register(t0);
Texture2D<float> ShadeTable : register(t1);
Texture2D<float> BlendTable : register(t2);
Texture2D<float> BackgroundTexture : register(t3);
Texture2D<float> SpriteAtlas : register(t4);
Texture2D<float> SkyTexture : register(t5);

cbuffer WorldSettings : register(b0)
{
    float2 invViewportSize;
    float2 padding;
    int4 skyOrigin;   // beginX, beginY, sinRoll, cosRoll (16.16)
    int4 skyParams;   // textureSize, viewportX, viewportY, unused
};

#if USE_ROV
RasterizerOrderedTexture2D<unorm float> IndexTarget : register(u0);
#define REJECT_PIXEL return
#else
#define REJECT_PIXEL discard
#endif

struct VertexOutput
{
    float4 position : SV_POSITION;
    noperspective float2 uv : TEXCOORD0;
    noperspective float shade : TEXCOORD1;
    nointerpolation uint texBase : TEXCOORD2;
    nointerpolation uint packed : TEXCOORD3;
};

uint LoadByte(Texture2D<float> source, int x, int y)
{
    return (uint)(source.Load(int3(x, y, 0)) * 255.0f + 0.5f);
}

#if USE_ROV
void main(VertexOutput input)
#else
float4 main(VertexOutput input) : SV_TARGET
#endif
{
    uint mode = input.packed & 0xFFu;
    uint constantShade = (input.packed >> 8) & 0xFFu;
    int textureSize = (int)((input.packed >> 16) & 0xFFu);
    int2 screenTexel = int2(input.position.xy);

    uint outputIndex;

    if (input.packed & 0x2000000u)
    {
        // Sky layer (DrawSky_40950).  The software loop accumulates the
        // texture coordinate from per column BYTE2 deltas, so column w lands
        // on x0 + BYTE2((w-1) * cosRoll) rather than on the exact affine
        // value - that truncation is part of the picture and is reproduced
        // here.  Column 0 keeps the unreduced origin, exactly like the loop.
        int size = skyParams.x;
        int column = (int)input.position.x - skyParams.y;
        int row = (int)input.position.y - skyParams.z;

        int originX = skyOrigin.x - skyOrigin.z * row;   // beginX - sinRoll * h
        int originY = skyOrigin.y + skyOrigin.w * row;   // beginY + cosRoll * h

        int texelX;
        int texelY;
        if (size == 256)
        {
            texelX = (originX >> 16) & 255;
            texelY = (originY >> 16) & 255;
        }
        else
        {
            texelX = (int)(((uint)originX) >> 16);
            texelY = (int)(((uint)originY) >> 16);
        }

        if (column > 0)
        {
            texelX = (texelX + (((column - 1) * skyOrigin.w >> 16) & 255)) % size;
            texelY = (texelY + (((column - 1) * skyOrigin.z >> 16) & 255)) % size;
        }

        uint skyIndex = ((uint)(texelX + size * texelY)) % (uint)(size * size);
        outputIndex = LoadByte(
            SkyTexture, (int)(skyIndex % (uint)size), (int)(skyIndex / (uint)size));
    }
    else if (input.packed & 0x1000000u)
    {
        // World sprite (DrawSprite_41BD3): uv addresses the sprite atlas
        // directly, index 0 is transparent, the mode byte is the sprite pixel
        // mode dword0x01_rotIdx.
        int texelU = (int)floor(input.uv.x);
        int texelV = (int)floor(input.uv.y);
        uint texel = LoadByte(SpriteAtlas, texelU, texelV);
        if (texel == 0u)
        {
            REJECT_PIXEL;
        }
        if (mode == 0u)
        {
            outputIndex = texel;             // plain copy
        }
        else if (mode == 1u)
        {
            // tablesx[shade << 8 | texel] with the constant shade level
            outputIndex = LoadByte(ShadeTable, (int)texel, (int)constantShade);
        }
        else if (mode == 4u)
        {
            // tablesx[16384 + (colour << 8 | texel)] - player colour remap
            outputIndex = LoadByte(BlendTable, (int)texel, (int)constantShade);
        }
        else if (mode == 5u)
        {
            // tablesx[16384 + (texel << 8 | colour)]
            outputIndex = LoadByte(BlendTable, (int)constantShade, (int)texel);
        }
#if USE_ROV
        else
        {
            // Destination reading sprite modes (2, 3, 6, 7, 8), exact through
            // the rasterizer ordered view.
            uint destination = (uint)(IndexTarget[screenTexel] * 255.0f + 0.5f);
            if (mode == 2u)
            {
                // tablesx[16384 + (texel << 8 | dest)]
                outputIndex = LoadByte(BlendTable, (int)destination, (int)texel);
            }
            else if (mode == 3u)
            {
                // tablesx[16384 + (dest << 8 | texel)]
                outputIndex = LoadByte(BlendTable, (int)texel, (int)destination);
            }
            else if (mode == 6u)
            {
                // blend then shade: tablesx[S | tablesx[16384 + (t << 8 | d)]]
                uint blended = LoadByte(BlendTable, (int)destination, (int)texel);
                outputIndex = LoadByte(ShadeTable, (int)blended, (int)constantShade);
            }
            else if (mode == 7u)
            {
                // transposed blend then shade
                uint blended = LoadByte(BlendTable, (int)texel, (int)destination);
                outputIndex = LoadByte(ShadeTable, (int)blended, (int)constantShade);
            }
            else
            {
                // mode 8: shade the destination, texel only gates coverage
                outputIndex = LoadByte(ShadeTable, (int)destination, (int)constantShade);
            }
        }
#else
        else
        {
            REJECT_PIXEL;                    // dest reading modes need the ROV
        }
#endif
    }
    else if (mode == 0u || mode == 14u || mode == 15u)
    {
        outputIndex = constantShade;
    }
    else if (mode == 1u || mode == 4u || mode == 16u || mode == 17u)
    {
        outputIndex = (uint)(((int)floor(input.shade)) & 0xFF);
    }
    else
    {
        // Keep the sample inside its own tile.  The tiles sit side by side in
        // a 256 byte wide atlas, so a texel that overshoots the tile edge by
        // one lands in the neighbouring tile and paints a single pixel in a
        // completely unrelated colour - sand orange in the middle of water.
        // The software DDA never overshoots; the GPU interpolation does, most
        // often near the horizon where the tiles are only a few pixels wide.
        int texelU = clamp((int)floor(input.uv.x), 0, textureSize - 1);
        int texelV = clamp((int)floor(input.uv.y), 0, textureSize - 1);
        // linear = base + ((V & 0xFF) << 8) + (U & 0xFF), exactly as the
        // software rasteriser addresses the 256 byte wide atlas.  It does NOT
        // reject texels past the tile: a V of exactly textureSize at a polygon
        // edge simply reads the next atlas row.  Discarding those pixels
        // instead punched one pixel holes along the tile edges - the texture
        // seams reported in game.
        uint localOffset = ((uint)texelV << 8) | (uint)texelU;
        uint linearOffset = input.texBase + localOffset;
        uint texel = LoadByte(AtlasTexture, (int)(linearOffset & 255u), (int)(linearOffset >> 8));

        if ((mode == 3u || mode == 6u) && texel == 0u)
        {
            REJECT_PIXEL;
        }

        if (mode == 2u || mode == 3u)
        {
            outputIndex = texel;
        }
        else
        {
            uint shadeLevel = (mode == 7u || mode == 11u)
                ? constantShade
                : (uint)(((int)floor(input.shade)) & 0xFF);
            outputIndex = LoadByte(ShadeTable, (int)texel, (int)shadeLevel);

            // Mode 0x1A blends texels below index 12 with the pixel underneath,
            // which is how the engine draws translucent water and shadows.
            // With the rasterizer ordered view the destination is the exact
            // painter-order pixel; the baseline shader falls back to the
            // background layer of this frame (see README limitations).
            if (mode == 26u && texel < 12u)
            {
#if USE_ROV
                uint destination = (uint)(IndexTarget[screenTexel] * 255.0f + 0.5f);
#else
                uint destination = LoadByte(
                    BackgroundTexture, screenTexel.x, screenTexel.y);
#endif
                outputIndex = LoadByte(
                    BlendTable, (int)outputIndex, (int)destination);
            }
        }
    }

#if USE_ROV
    IndexTarget[screenTexel] = (float)outputIndex / 255.0f;
#else
    float encoded = (float)outputIndex / 255.0f;
    return float4(encoded, encoded, encoded, 1.0f);
#endif
}
)";

	bool CompileWorldShader(
		const char* source,
		const char* target,
		ComPtr<ID3DBlob>& byteCode,
		std::string& error,
		const D3D_SHADER_MACRO* defines = nullptr)
	{
		ComPtr<ID3DBlob> diagnostics;
		const HRESULT result = D3DCompile(
			source,
			std::strlen(source),
			nullptr,
			defines,
			nullptr,
			"main",
			target,
			D3DCOMPILE_ENABLE_STRICTNESS,
			0,
			&byteCode,
			&diagnostics);

		if (FAILED(result))
		{
			if (diagnostics)
			{
				error.assign(
					static_cast<const char*>(diagnostics->GetBufferPointer()),
					diagnostics->GetBufferSize());
			}
			else
			{
				error = GpuHResultMessage("D3DCompile", result);
			}
			return false;
		}
		return true;
	}
}

struct GpuWorldRenderer::Impl
{
	struct alignas(16) WorldSettings
	{
		float invViewportSize[2] = { 1.0f, 1.0f };
		float padding[2] = { 0.0f, 0.0f };
		int32_t skyOrigin[4] = { 0, 0, 0, 0 };
		int32_t skyParams[4] = { 256, 0, 0, 0 };
	};

	struct Vertex
	{
		float position[2];
		float uv[2];
		float shade;
		uint32_t textureBase;
		uint32_t packed;
	};

	ComPtr<ID3D11VertexShader> vertexShader;
	ComPtr<ID3D11PixelShader> pixelShader;
	// ps_5_0 variant writing through a rasterizer ordered view; only present
	// when the hardware supports ROVs and typed R8_UNORM UAV load/store.
	ComPtr<ID3D11PixelShader> pixelShaderOrdered;
	ComPtr<ID3D11InputLayout> inputLayout;
	ComPtr<ID3D11Buffer> settingsBuffer;
	ComPtr<ID3D11Buffer> vertexBuffer;
	ComPtr<ID3D11RasterizerState> rasterizerState;
	ComPtr<ID3D11BlendState> opaqueBlendState;
	ComPtr<ID3D11DepthStencilState> depthDisabledState;

	ComPtr<ID3D11Texture2D> indexTarget;
	ComPtr<ID3D11RenderTargetView> indexTargetView;
	ComPtr<ID3D11ShaderResourceView> indexTargetSrv;
	ComPtr<ID3D11UnorderedAccessView> indexTargetUav;
	ComPtr<ID3D11Texture2D> backgroundTexture;

	ComPtr<ID3D11Texture2D> atlasTexture;
	ComPtr<ID3D11ShaderResourceView> atlasView;
	ComPtr<ID3D11Texture2D> shadeTexture;
	ComPtr<ID3D11ShaderResourceView> shadeView;
	ComPtr<ID3D11Texture2D> blendTexture;
	ComPtr<ID3D11ShaderResourceView> blendView;
	ComPtr<ID3D11ShaderResourceView> backgroundView;

	ComPtr<ID3D11Texture2D> spriteAtlasTexture;
	ComPtr<ID3D11ShaderResourceView> spriteAtlasView;

	ComPtr<ID3D11Texture2D> skyTexture;
	ComPtr<ID3D11ShaderResourceView> skyView;
	int skyTextureSize = 0;
	GpuSkyParams pendingSky;
	bool skySubmitted = false;

	std::vector<Vertex> vertices;
	size_t vertexBufferCapacity = 0;

	// Per frame sprite atlas state (shelf packer, see PackSprite).
	struct SpriteAtlasEntry
	{
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
	};
	std::vector<uint8_t> spriteAtlasPixels;
	std::unordered_map<const uint8_t*, SpriteAtlasEntry> spriteCache;
	int spriteShelfX = 0;
	int spriteShelfY = 0;
	int spriteShelfHeight = 0;
	int spriteAtlasUsedHeight = 0;
	bool spriteAtlasDirty = false;

	int targetWidth = 0;
	int targetHeight = 0;
	uint32_t atlasHeight = 0;
	bool atlasDirty = true;

	bool ready = false;
	bool geometryEnabled = false;
	bool spritesEnabled = false;
	bool skyEnabled = false;
	bool capturing = false;
	// Hardware can do ordered destination reads / the config wants them.
	bool orderedViewsSupported = false;
	bool exactBlendEnabled = true;

	int viewportX = 0;
	int viewportY = 0;
	int viewportWidth = 0;
	int viewportHeight = 0;

	int triangleCullMode = 0;
	uint32_t culledTriangles = 0;
	uint32_t submittedTriangles = 0;
	uint32_t rejectedTriangles = 0;
	uint32_t submittedSprites = 0;
	uint32_t rejectedSprites = 0;
	uint32_t modeCounts[256] = {};

	CompositeInfo composite;
	std::string lastError;
	std::string orderedViewsDiagnostic;

	bool UseOrderedViews() const
	{
		return orderedViewsSupported && exactBlendEnabled && pixelShaderOrdered;
	}

	bool EnsureTargets(int width, int height)
	{
		if (width == targetWidth && height == targetHeight && indexTarget)
		{
			return true;
		}

		indexTargetUav.Reset();
		indexTargetSrv.Reset();
		indexTargetView.Reset();
		indexTarget.Reset();
		backgroundView.Reset();
		backgroundTexture.Reset();

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			lastError = "GPU world renderer has no device";
			return false;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(width);
		description.Height = static_cast<UINT>(height);
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (orderedViewsSupported)
		{
			description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
		}

		HRESULT result = device->CreateTexture2D(&description, nullptr, &indexTarget);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(world index target)", result);
			return false;
		}

		result = device->CreateRenderTargetView(indexTarget.Get(), nullptr, &indexTargetView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateRenderTargetView(world)", result);
			return false;
		}

		result = device->CreateShaderResourceView(indexTarget.Get(), nullptr, &indexTargetSrv);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(world)", result);
			return false;
		}

		if (orderedViewsSupported)
		{
			result = device->CreateUnorderedAccessView(
				indexTarget.Get(), nullptr, &indexTargetUav);
			if (FAILED(result))
			{
				lastError = GpuHResultMessage("CreateUnorderedAccessView(world)", result);
				return false;
			}
		}

		D3D11_TEXTURE2D_DESC backgroundDescription = description;
		backgroundDescription.Usage = D3D11_USAGE_DYNAMIC;
		backgroundDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		backgroundDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		result = device->CreateTexture2D(&backgroundDescription, nullptr, &backgroundTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(world background)", result);
			return false;
		}

		result = device->CreateShaderResourceView(
			backgroundTexture.Get(), nullptr, &backgroundView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(world background)", result);
			return false;
		}

		targetWidth = width;
		targetHeight = height;
		return true;
	}

	bool EnsureAtlas(uint32_t atlasBytes)
	{
		const uint32_t requiredHeight = std::max<uint32_t>(1u, atlasBytes / AtlasWidth);
		if (requiredHeight == atlasHeight && atlasTexture)
		{
			return true;
		}

		atlasView.Reset();
		atlasTexture.Reset();

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = AtlasWidth;
		description.Height = requiredHeight;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DYNAMIC;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT result = device->CreateTexture2D(&description, nullptr, &atlasTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(terrain atlas)", result);
			return false;
		}

		result = device->CreateShaderResourceView(atlasTexture.Get(), nullptr, &atlasView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(terrain atlas)", result);
			return false;
		}

		atlasHeight = requiredHeight;
		atlasDirty = true;
		return true;
	}

	bool UploadTexture2D(
		ID3D11Texture2D* texture,
		const uint8_t* source,
		uint32_t width,
		uint32_t height,
		uint32_t sourcePitch)
	{
		ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();
		if (!context || !texture || !source)
		{
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT result = context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("Map(world texture)", result);
			return false;
		}

		for (uint32_t y = 0; y < height; ++y)
		{
			std::memcpy(
				static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
				source + static_cast<size_t>(y) * sourcePitch,
				width);
		}

		context->Unmap(texture, 0);
		return true;
	}

	bool EnsureVertexBuffer(size_t vertexCount)
	{
		if (vertexCount <= vertexBufferCapacity && vertexBuffer)
		{
			return true;
		}

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			return false;
		}

		size_t capacity = std::max<size_t>(vertexBufferCapacity * 2, 8192);
		while (capacity < vertexCount)
		{
			capacity *= 2;
		}

		vertexBuffer.Reset();

		D3D11_BUFFER_DESC description{};
		description.ByteWidth = static_cast<UINT>(capacity * sizeof(Vertex));
		description.Usage = D3D11_USAGE_DYNAMIC;
		description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		const HRESULT result = device->CreateBuffer(&description, nullptr, &vertexBuffer);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateBuffer(world vertices)", result);
			return false;
		}

		vertexBufferCapacity = capacity;
		return true;
	}

	void ResetSpriteAtlas()
	{
		spriteCache.clear();
		spriteShelfX = 0;
		spriteShelfY = 0;
		spriteShelfHeight = 0;
		spriteAtlasUsedHeight = 0;
		spriteAtlasDirty = false;
	}

	// Places a sprite bitmap in the per frame atlas.  The same bitmap pointer
	// is reused within a frame (a sprite drawn many times costs one entry).
	// Each entry gets a one texel border replicated from its edges, so a
	// half-ulp of interpolation error at a quad edge still reads the texel the
	// CPU blit would have read.  Returns false when the atlas is full.
	bool PackSprite(const uint8_t* pixels, int width, int height, int& outX, int& outY)
	{
		const auto found = spriteCache.find(pixels);
		if (found != spriteCache.end() &&
			found->second.width == width && found->second.height == height)
		{
			outX = found->second.x;
			outY = found->second.y;
			return true;
		}

		const int paddedWidth = width + 2;
		const int paddedHeight = height + 2;
		if (paddedWidth > SpriteAtlasWidth)
		{
			return false;
		}
		if (spriteShelfX + paddedWidth > SpriteAtlasWidth)
		{
			spriteShelfY += spriteShelfHeight;
			spriteShelfX = 0;
			spriteShelfHeight = 0;
		}
		if (spriteShelfY + paddedHeight > SpriteAtlasHeight)
		{
			return false;
		}

		if (spriteAtlasPixels.empty())
		{
			spriteAtlasPixels.resize(
				static_cast<size_t>(SpriteAtlasWidth) * SpriteAtlasHeight);
		}

		for (int row = -1; row <= height; ++row)
		{
			const int sourceRow = std::clamp(row, 0, height - 1);
			const uint8_t* source = pixels + static_cast<size_t>(sourceRow) * width;
			uint8_t* destination = spriteAtlasPixels.data() +
				static_cast<size_t>(spriteShelfY + 1 + row) * SpriteAtlasWidth +
				spriteShelfX + 1;
			std::memcpy(destination, source, static_cast<size_t>(width));
			destination[-1] = source[0];
			destination[width] = source[width - 1];
		}

		outX = spriteShelfX + 1;
		outY = spriteShelfY + 1;
		spriteCache.emplace(pixels, SpriteAtlasEntry{ outX, outY, width, height });
		spriteShelfX += paddedWidth;
		spriteShelfHeight = std::max(spriteShelfHeight, paddedHeight);
		spriteAtlasUsedHeight = std::max(spriteAtlasUsedHeight, spriteShelfY + paddedHeight);
		spriteAtlasDirty = true;
		return true;
	}

	// The sky bitmap changes on level load / day cycle; it is small enough
	// (64 KB, 1 MB with big textures) to simply refresh it per frame.
	bool EnsureSkyTexture(int size)
	{
		if (skyTexture && skyTextureSize == size)
		{
			return true;
		}

		skyView.Reset();
		skyTexture.Reset();

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device || size <= 0)
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(size);
		description.Height = static_cast<UINT>(size);
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DYNAMIC;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT result = device->CreateTexture2D(&description, nullptr, &skyTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(sky)", result);
			return false;
		}
		result = device->CreateShaderResourceView(skyTexture.Get(), nullptr, &skyView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(sky)", result);
			return false;
		}

		skyTextureSize = size;
		return true;
	}

	bool EnsureSpriteAtlasTexture()
	{
		if (spriteAtlasTexture)
		{
			return true;
		}

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = SpriteAtlasWidth;
		description.Height = SpriteAtlasHeight;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DYNAMIC;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT result = device->CreateTexture2D(&description, nullptr, &spriteAtlasTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(sprite atlas)", result);
			return false;
		}
		result = device->CreateShaderResourceView(
			spriteAtlasTexture.Get(), nullptr, &spriteAtlasView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(sprite atlas)", result);
			return false;
		}
		return true;
	}
};

GpuWorldRenderer::GpuWorldRenderer() : m_impl(std::make_unique<Impl>())
{
}

GpuWorldRenderer::~GpuWorldRenderer()
{
	Shutdown();
}

GpuWorldRenderer& GpuWorldRenderer::Get()
{
	static GpuWorldRenderer instance;
	return instance;
}

bool GpuWorldRenderer::Initialize()
{
	m_impl = std::make_unique<Impl>();

	GpuRenderDevice& gpuDevice = GpuRenderDevice::Get();
	if (!gpuDevice.IsReady())
	{
		m_impl->lastError = "GPU world renderer requires an initialised render device";
		return false;
	}

	ID3D11Device* device = gpuDevice.GetDevice();

	ComPtr<ID3DBlob> vertexByteCode;
	if (!CompileWorldShader(WorldVertexShaderSource, "vs_4_0", vertexByteCode, m_impl->lastError))
	{
		return false;
	}
	HRESULT result = device->CreateVertexShader(
		vertexByteCode->GetBufferPointer(),
		vertexByteCode->GetBufferSize(),
		nullptr,
		&m_impl->vertexShader);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateVertexShader(world)", result);
		return false;
	}

	ComPtr<ID3DBlob> pixelByteCode;
	if (!CompileWorldShader(WorldPixelShaderSource, "ps_4_0", pixelByteCode, m_impl->lastError))
	{
		return false;
	}
	result = device->CreatePixelShader(
		pixelByteCode->GetBufferPointer(),
		pixelByteCode->GetBufferSize(),
		nullptr,
		&m_impl->pixelShader);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreatePixelShader(world)", result);
		return false;
	}

	// Optional exact destination blending: needs feature level 11.0 (ps_5_0),
	// rasterizer ordered views and typed R8_UNORM UAV load/store.  Failure
	// here is not an error - the baseline shader stays fully functional.
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
	{
		m_impl->orderedViewsDiagnostic = "feature level below 11.0";
	}
	else
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 formatSupport2{};
		formatSupport2.InFormat = DXGI_FORMAT_R8_UNORM;
		constexpr UINT requiredUavSupport =
			D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
		if (FAILED(device->CheckFeatureSupport(
				D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2))) ||
			!options2.ROVsSupported)
		{
			m_impl->orderedViewsDiagnostic = "rasterizer ordered views not supported";
		}
		else if (FAILED(device->CheckFeatureSupport(
				D3D11_FEATURE_FORMAT_SUPPORT2, &formatSupport2, sizeof(formatSupport2))) ||
			(formatSupport2.OutFormatSupport2 & requiredUavSupport) != requiredUavSupport)
		{
			m_impl->orderedViewsDiagnostic = "no typed R8_UNORM UAV load/store";
		}
		else
		{
			const D3D_SHADER_MACRO rovDefines[] = {
				{ "USE_ROV", "1" },
				{ nullptr, nullptr }
			};
			ComPtr<ID3DBlob> orderedByteCode;
			std::string orderedError;
			if (!CompileWorldShader(
					WorldPixelShaderSource, "ps_5_0", orderedByteCode, orderedError, rovDefines))
			{
				m_impl->orderedViewsDiagnostic = "ROV shader compile failed: " + orderedError;
			}
			else if (FAILED(result = device->CreatePixelShader(
					orderedByteCode->GetBufferPointer(),
					orderedByteCode->GetBufferSize(),
					nullptr,
					&m_impl->pixelShaderOrdered)))
			{
				m_impl->orderedViewsDiagnostic =
					GpuHResultMessage("CreatePixelShader(world ROV)", result);
			}
			else
			{
				m_impl->orderedViewsSupported = true;
			}
		}
	}

	const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	result = device->CreateInputLayout(
		layout,
		static_cast<UINT>(std::size(layout)),
		vertexByteCode->GetBufferPointer(),
		vertexByteCode->GetBufferSize(),
		&m_impl->inputLayout);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateInputLayout(world)", result);
		return false;
	}

	D3D11_BUFFER_DESC settingsDescription{};
	settingsDescription.ByteWidth = sizeof(Impl::WorldSettings);
	settingsDescription.Usage = D3D11_USAGE_DYNAMIC;
	settingsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	settingsDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	result = device->CreateBuffer(&settingsDescription, nullptr, &m_impl->settingsBuffer);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateBuffer(world settings)", result);
		return false;
	}

	D3D11_RASTERIZER_DESC rasterizerDescription{};
	rasterizerDescription.FillMode = D3D11_FILL_SOLID;
	rasterizerDescription.CullMode = D3D11_CULL_NONE;
	rasterizerDescription.DepthClipEnable = TRUE;
	result = device->CreateRasterizerState(&rasterizerDescription, &m_impl->rasterizerState);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateRasterizerState(world)", result);
		return false;
	}

	D3D11_BLEND_DESC blendDescription{};
	blendDescription.RenderTarget[0].BlendEnable = FALSE;
	blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	result = device->CreateBlendState(&blendDescription, &m_impl->opaqueBlendState);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateBlendState(world)", result);
		return false;
	}

	D3D11_DEPTH_STENCIL_DESC depthDescription{};
	depthDescription.DepthEnable = FALSE;
	depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
	result = device->CreateDepthStencilState(&depthDescription, &m_impl->depthDisabledState);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateDepthStencilState(world)", result);
		return false;
	}

	D3D11_TEXTURE2D_DESC shadeDescription{};
	shadeDescription.Width = ShadeTableWidth;
	shadeDescription.Height = ShadeTableHeight;
	shadeDescription.MipLevels = 1;
	shadeDescription.ArraySize = 1;
	shadeDescription.Format = DXGI_FORMAT_R8_UNORM;
	shadeDescription.SampleDesc.Count = 1;
	shadeDescription.Usage = D3D11_USAGE_DYNAMIC;
	shadeDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	shadeDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	result = device->CreateTexture2D(&shadeDescription, nullptr, &m_impl->shadeTexture);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateTexture2D(shade table)", result);
		return false;
	}
	result = device->CreateShaderResourceView(m_impl->shadeTexture.Get(), nullptr, &m_impl->shadeView);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateShaderResourceView(shade table)", result);
		return false;
	}

	result = device->CreateTexture2D(&shadeDescription, nullptr, &m_impl->blendTexture);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateTexture2D(blend table)", result);
		return false;
	}
	result = device->CreateShaderResourceView(m_impl->blendTexture.Get(), nullptr, &m_impl->blendView);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateShaderResourceView(blend table)", result);
		return false;
	}

	m_impl->ready = true;
	return true;
}

void GpuWorldRenderer::Shutdown()
{
	if (m_impl)
	{
		m_impl = std::make_unique<Impl>();
	}
}

bool GpuWorldRenderer::IsReady() const
{
	return m_impl && m_impl->ready;
}

const std::string& GpuWorldRenderer::GetLastError() const
{
	static const std::string noError;
	return m_impl ? m_impl->lastError : noError;
}

void GpuWorldRenderer::SetGeometryEnabled(bool enabled)
{
	if (m_impl)
	{
		m_impl->geometryEnabled = enabled;
	}
}

bool GpuWorldRenderer::IsGeometryEnabled() const
{
	return m_impl && m_impl->ready && m_impl->geometryEnabled;
}

void GpuWorldRenderer::SetSpritesEnabled(bool enabled)
{
	if (m_impl)
	{
		m_impl->spritesEnabled = enabled;
	}
}

bool GpuWorldRenderer::AreSpritesEnabled() const
{
	return m_impl && m_impl->ready && m_impl->spritesEnabled;
}

void GpuWorldRenderer::SetExactBlendEnabled(bool enabled)
{
	if (m_impl)
	{
		m_impl->exactBlendEnabled = enabled;
	}
}

void GpuWorldRenderer::SetTriangleCullMode(int mode)
{
	if (m_impl)
	{
		m_impl->triangleCullMode = mode;
	}
}

void GpuWorldRenderer::SetSkyEnabled(bool enabled)
{
	if (m_impl)
	{
		m_impl->skyEnabled = enabled;
	}
}

bool GpuWorldRenderer::IsSkyEnabled() const
{
	// Without ordered views the mode 0x1A approximation reads the CPU drawn
	// background layer, which the GPU sky would no longer fill.
	return m_impl && m_impl->ready && m_impl->geometryEnabled &&
		m_impl->skyEnabled && m_impl->UseOrderedViews();
}

bool GpuWorldRenderer::SubmitSky(const GpuSkyParams& params)
{
	if (!m_impl || !m_impl->capturing || !IsSkyEnabled())
	{
		return false;
	}

	Impl& impl = *m_impl;
	if (!params.pixels || params.textureSize <= 0 || !impl.EnsureSkyTexture(params.textureSize))
	{
		return false;
	}

	// A viewport filling quad, emitted before any terrain so the painter
	// order puts it underneath everything else.
	constexpr uint32_t packed = SkySourceFlag;
	const float width = static_cast<float>(impl.viewportWidth);
	const float height = static_cast<float>(impl.viewportHeight);
	const Impl::Vertex corners[4] = {
		{ { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.0f, 0, packed },
		{ { width, 0.0f }, { 0.0f, 0.0f }, 0.0f, 0, packed },
		{ { width, height }, { 0.0f, 0.0f }, 0.0f, 0, packed },
		{ { 0.0f, height }, { 0.0f, 0.0f }, 0.0f, 0, packed },
	};
	impl.vertices.push_back(corners[0]);
	impl.vertices.push_back(corners[1]);
	impl.vertices.push_back(corners[2]);
	impl.vertices.push_back(corners[0]);
	impl.vertices.push_back(corners[2]);
	impl.vertices.push_back(corners[3]);

	impl.pendingSky = params;
	impl.skySubmitted = true;
	return true;
}

bool GpuWorldRenderer::SupportsDestinationReads() const
{
	return m_impl && m_impl->ready && m_impl->UseOrderedViews();
}

std::string GpuWorldRenderer::GetOrderedViewsDiagnostic() const
{
	return m_impl ? m_impl->orderedViewsDiagnostic : std::string();
}

bool GpuWorldRenderer::IsCapturing() const
{
	return m_impl && m_impl->capturing;
}

bool GpuWorldRenderer::SupportsMode(uint8_t mode)
{
	switch (mode)
	{
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 0x0B:
	case 0x0E:
	case 0x0F:
	case 0x10:
	case 0x11:
		return true;
	// 0x1A is textured Gouraud shading that additionally blends dark results
	// with the destination pixel.  The blend needs a framebuffer read the index
	// pass cannot do, so it is rendered as plain mode 5 for now (documented
	// approximation - a hole would be far more visible than a missing blend).
	case 0x1A:
		return true;
	default:
		return false;
	}
}

void GpuWorldRenderer::BeginWorld(
	uint8_t* screenBuffer,
	int screenWidth,
	int screenHeight,
	int viewportX,
	int viewportY,
	int viewportWidth,
	int viewportHeight)
{
	if (!IsGeometryEnabled() || !screenBuffer)
	{
		return;
	}
	if (screenWidth <= 0 || screenHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
	{
		return;
	}

	Impl& impl = *m_impl;
	impl.capturing = false;
	impl.vertices.clear();

	// DrawWorld_411A0 renders several viewports per frame (mirrored views,
	// split screen).  The first pass of a frame owns the whole target, later
	// passes only refresh their own rectangle and extend the composed region.
	const bool firstPassOfFrame = !impl.composite.valid;
	if (firstPassOfFrame)
	{
		impl.submittedTriangles = 0;
		impl.rejectedTriangles = 0;
		impl.culledTriangles = 0;
		impl.submittedSprites = 0;
		impl.rejectedSprites = 0;
		std::memset(impl.modeCounts, 0, sizeof(impl.modeCounts));
		impl.ResetSpriteAtlas();
	}
	impl.skySubmitted = false;

	if (!impl.EnsureTargets(screenWidth, screenHeight))
	{
		return;
	}

	// The CPU has just produced sky / background for the viewport.  That layer
	// has to stay below the GPU geometry, so it becomes the initial content of
	// the index target.
	if (!impl.UploadTexture2D(
		impl.backgroundTexture.Get(),
		screenBuffer,
		static_cast<uint32_t>(screenWidth),
		static_cast<uint32_t>(screenHeight),
		static_cast<uint32_t>(screenWidth)))
	{
		return;
	}

	const int clampedWidth = std::min(viewportWidth, screenWidth - viewportX);
	const int clampedHeight = std::min(viewportHeight, screenHeight - viewportY);
	if (clampedWidth <= 0 || clampedHeight <= 0)
	{
		return;
	}

	ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();
	if (firstPassOfFrame)
	{
		context->CopyResource(impl.indexTarget.Get(), impl.backgroundTexture.Get());
	}
	else
	{
		D3D11_BOX box{};
		box.left = static_cast<UINT>(viewportX);
		box.top = static_cast<UINT>(viewportY);
		box.front = 0;
		box.right = static_cast<UINT>(viewportX + clampedWidth);
		box.bottom = static_cast<UINT>(viewportY + clampedHeight);
		box.back = 1;
		context->CopySubresourceRegion(
			impl.indexTarget.Get(),
			0,
			static_cast<UINT>(viewportX),
			static_cast<UINT>(viewportY),
			0,
			impl.backgroundTexture.Get(),
			0,
			&box);
	}

	// Everything the CPU still writes into the viewport from now on (HUD, spell
	// icons, name plates) is an overlay; index 0 marks "untouched".
	for (int row = 0; row < clampedHeight; ++row)
	{
		std::memset(
			screenBuffer + static_cast<size_t>(viewportY + row) * screenWidth + viewportX,
			0,
			static_cast<size_t>(clampedWidth));
	}

	impl.viewportX = viewportX;
	impl.viewportY = viewportY;
	impl.viewportWidth = clampedWidth;
	impl.viewportHeight = clampedHeight;
	impl.capturing = true;
}

void GpuWorldRenderer::SubmitTriangle(
	const GpuWorldVertex& vertex1,
	const GpuWorldVertex& vertex2,
	const GpuWorldVertex& vertex3,
	uint32_t textureOffset,
	uint8_t mode,
	uint8_t constantShade,
	uint8_t textureSize)
{
	if (!m_impl || !m_impl->capturing)
	{
		return;
	}

	Impl& impl = *m_impl;
	if (!SupportsMode(mode))
	{
		++impl.rejectedTriangles;
		return;
	}

	if (impl.triangleCullMode != 0)
	{
		// Signed area of the projected triangle; the sign is its winding.
		const int64_t crossProduct =
			static_cast<int64_t>(vertex2.X - vertex1.X) * (vertex3.Y - vertex1.Y) -
			static_cast<int64_t>(vertex3.X - vertex1.X) * (vertex2.Y - vertex1.Y);
		if ((impl.triangleCullMode == 1 && crossProduct > 0) ||
			(impl.triangleCullMode == 2 && crossProduct < 0))
		{
			++impl.culledTriangles;
			return;
		}
	}

	const uint32_t packed =
		static_cast<uint32_t>(mode) |
		(static_cast<uint32_t>(constantShade) << 8) |
		(static_cast<uint32_t>(textureSize) << 16);

	// The software rasteriser evaluates U/V/brightness at the integer pixel
	// coordinate, the GPU at the pixel centre (x + 0.5).  Shifting the
	// triangle by half a pixel makes the centre land on the value the CPU
	// would have used, which also lines the coverage rule up with the CPU
	// span (x .. xEnd-1).  Without it neighbouring polygons round differently
	// along a shared edge and leave single pixel texture seams.
	// Sprites and the sky quad already work in pixel centre space.
	constexpr float pixelCentre = 0.5f;

	// U/V travel to the GPU as floats, whose 24 bit mantissa loses fractional
	// precision once the integer part grows.  The shader only ever uses
	// (texel & 0xFF), so subtracting the same multiple of 256 texels
	// (0x1000000 in 16.16) from all three corners leaves the result untouched
	// while keeping the values small enough to interpolate exactly.
	const int32_t uBase = vertex1.U & ~0xFFFFFF;
	const int32_t vBase = vertex1.V & ~0xFFFFFF;

	// Note for future attempts: the software rasteriser starts each scanline
	// with the attribute value of the UNROUNDED span edge and then steps in
	// whole pixels, so its sample sits frac(xEdge) * dA/dx ahead of the exact
	// value - a per scanline offset no vertex data can reproduce.  Adding its
	// nominal expected value (half a horizontal step) once per triangle was
	// measured and made both test frames worse (level 8: 0.78 % -> 0.87 %
	// strong pixels, level 1: 16 -> 18), so the fractions are evidently not
	// uniformly distributed.  Only emulating the DDA per scanline would help.

	const GpuWorldVertex* source[3] = { &vertex1, &vertex2, &vertex3 };
	for (const GpuWorldVertex* vertex : source)
	{
		Impl::Vertex converted{};
		converted.position[0] = static_cast<float>(vertex->X) + pixelCentre;
		converted.position[1] = static_cast<float>(vertex->Y) + pixelCentre;
		converted.uv[0] = static_cast<float>(vertex->U - uBase) / 65536.0f;
		converted.uv[1] = static_cast<float>(vertex->V - vBase) / 65536.0f;
		converted.shade = static_cast<float>(vertex->Brightness) / 65536.0f;
		converted.textureBase = textureOffset;
		converted.packed = packed;
		impl.vertices.push_back(converted);
	}

	++impl.submittedTriangles;
	++impl.modeCounts[mode];
}

bool GpuWorldRenderer::SubmitSprite(const GpuSpriteQuad& quad)
{
	if (!m_impl || !m_impl->capturing || !m_impl->spritesEnabled)
	{
		return false;
	}

	Impl& impl = *m_impl;
	if (!quad.pixels || quad.width <= 0 || quad.height <= 0)
	{
		++impl.rejectedSprites;
		return false;
	}

	int atlasX = 0;
	int atlasY = 0;
	if (!impl.PackSprite(quad.pixels, quad.width, quad.height, atlasX, atlasY))
	{
		++impl.rejectedSprites;
		return false;
	}

	const uint32_t packed =
		SpriteSourceFlag |
		static_cast<uint32_t>(quad.mode) |
		(static_cast<uint32_t>(quad.constant) << 8);

	const float u0 = static_cast<float>(atlasX) + quad.u0;
	const float u1 = static_cast<float>(atlasX) + quad.u1;
	const float v0 = static_cast<float>(atlasY) + quad.v0;
	const float v1 = static_cast<float>(atlasY) + quad.v1;

	// Corner order P00, P10, P11, P01: u runs along P00->P10, v along P00->P01.
	const Impl::Vertex corners[4] = {
		{ { quad.cornerX[0], quad.cornerY[0] }, { u0, v0 }, 0.0f, 0, packed },
		{ { quad.cornerX[1], quad.cornerY[1] }, { u1, v0 }, 0.0f, 0, packed },
		{ { quad.cornerX[2], quad.cornerY[2] }, { u1, v1 }, 0.0f, 0, packed },
		{ { quad.cornerX[3], quad.cornerY[3] }, { u0, v1 }, 0.0f, 0, packed },
	};
	impl.vertices.push_back(corners[0]);
	impl.vertices.push_back(corners[1]);
	impl.vertices.push_back(corners[2]);
	impl.vertices.push_back(corners[0]);
	impl.vertices.push_back(corners[2]);
	impl.vertices.push_back(corners[3]);

	++impl.submittedSprites;
	return true;
}

void GpuWorldRenderer::NoteSpriteRejected()
{
	if (m_impl)
	{
		++m_impl->rejectedSprites;
	}
}

void GpuWorldRenderer::EndWorld(
	const uint8_t* shadeTable,
	const uint8_t* textureAtlas,
	uint32_t textureAtlasBytes)
{
	if (!m_impl || !m_impl->capturing)
	{
		return;
	}

	Impl& impl = *m_impl;
	impl.capturing = false;

	if (impl.composite.valid)
	{
		const int left = std::min(impl.composite.viewportX, impl.viewportX);
		const int top = std::min(impl.composite.viewportY, impl.viewportY);
		const int right = std::max(
			impl.composite.viewportX + impl.composite.viewportWidth,
			impl.viewportX + impl.viewportWidth);
		const int bottom = std::max(
			impl.composite.viewportY + impl.composite.viewportHeight,
			impl.viewportY + impl.viewportHeight);
		impl.composite.viewportX = left;
		impl.composite.viewportY = top;
		impl.composite.viewportWidth = right - left;
		impl.composite.viewportHeight = bottom - top;
	}
	else
	{
		impl.composite.viewportX = impl.viewportX;
		impl.composite.viewportY = impl.viewportY;
		impl.composite.viewportWidth = impl.viewportWidth;
		impl.composite.viewportHeight = impl.viewportHeight;
	}
	impl.composite.valid = true;
	impl.composite.targetWidth = impl.targetWidth;
	impl.composite.targetHeight = impl.targetHeight;

	if (impl.vertices.empty())
	{
		return;
	}

	if (!impl.EnsureAtlas(textureAtlasBytes) || !impl.EnsureVertexBuffer(impl.vertices.size()))
	{
		return;
	}

	ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();

	if (shadeTable)
	{
		impl.UploadTexture2D(
			impl.shadeTexture.Get(), shadeTable, ShadeTableWidth, ShadeTableHeight, ShadeTableWidth);
		impl.UploadTexture2D(
			impl.blendTexture.Get(), shadeTable + BlendTableOffset,
			ShadeTableWidth, ShadeTableHeight, ShadeTableWidth);
	}

	if (textureAtlas &&
		(impl.atlasDirty || textureAtlasBytes <= AtlasAlwaysUploadLimit))
	{
		if (impl.UploadTexture2D(
			impl.atlasTexture.Get(), textureAtlas, AtlasWidth, impl.atlasHeight, AtlasWidth))
		{
			impl.atlasDirty = false;
		}
	}

	if (impl.spriteAtlasDirty && impl.spriteAtlasUsedHeight > 0 &&
		impl.EnsureSpriteAtlasTexture())
	{
		// WRITE_DISCARD invalidates the whole texture, but only rows the packer
		// filled are ever sampled, so uploading the used shelf region suffices.
		if (impl.UploadTexture2D(
			impl.spriteAtlasTexture.Get(),
			impl.spriteAtlasPixels.data(),
			SpriteAtlasWidth,
			static_cast<uint32_t>(impl.spriteAtlasUsedHeight),
			SpriteAtlasWidth))
		{
			impl.spriteAtlasDirty = false;
		}
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	HRESULT result = context->Map(impl.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(result))
	{
		impl.lastError = GpuHResultMessage("Map(world vertices)", result);
		return;
	}
	std::memcpy(mapped.pData, impl.vertices.data(), impl.vertices.size() * sizeof(Impl::Vertex));
	context->Unmap(impl.vertexBuffer.Get(), 0);

	if (impl.skySubmitted && impl.skyTexture)
	{
		impl.UploadTexture2D(
			impl.skyTexture.Get(),
			impl.pendingSky.pixels,
			static_cast<uint32_t>(impl.pendingSky.textureSize),
			static_cast<uint32_t>(impl.pendingSky.textureSize),
			static_cast<uint32_t>(impl.pendingSky.textureSize));
	}

	Impl::WorldSettings settings;
	settings.invViewportSize[0] = 2.0f / static_cast<float>(impl.viewportWidth);
	settings.invViewportSize[1] = 2.0f / static_cast<float>(impl.viewportHeight);
	settings.skyOrigin[0] = impl.pendingSky.beginX;
	settings.skyOrigin[1] = impl.pendingSky.beginY;
	settings.skyOrigin[2] = impl.pendingSky.sinRoll;
	settings.skyOrigin[3] = impl.pendingSky.cosRoll;
	settings.skyParams[0] = impl.pendingSky.textureSize;
	settings.skyParams[1] = impl.viewportX;
	settings.skyParams[2] = impl.viewportY;
	result = context->Map(impl.settingsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(result))
	{
		impl.lastError = GpuHResultMessage("Map(world settings)", result);
		return;
	}
	std::memcpy(mapped.pData, &settings, sizeof(settings));
	context->Unmap(impl.settingsBuffer.Get(), 0);

	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = static_cast<float>(impl.viewportX);
	viewport.TopLeftY = static_cast<float>(impl.viewportY);
	viewport.Width = static_cast<float>(impl.viewportWidth);
	viewport.Height = static_cast<float>(impl.viewportHeight);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	ID3D11ShaderResourceView* nullViews[6] = {
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 6, nullViews);

	constexpr float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const UINT stride = sizeof(Impl::Vertex);
	const UINT offset = 0;

	const bool orderedViews = impl.UseOrderedViews() && impl.indexTargetUav;
	if (orderedViews)
	{
		// The index target is written through the rasterizer ordered view, so
		// destination reading blend modes see the exact painter-order pixel.
		context->OMSetRenderTargetsAndUnorderedAccessViews(
			0, nullptr, nullptr, 0, 1, impl.indexTargetUav.GetAddressOf(), nullptr);
	}
	else
	{
		context->OMSetRenderTargets(1, impl.indexTargetView.GetAddressOf(), nullptr);
	}
	context->OMSetBlendState(impl.opaqueBlendState.Get(), blendFactor, 0xFFFFFFFF);
	context->OMSetDepthStencilState(impl.depthDisabledState.Get(), 0);
	context->RSSetState(impl.rasterizerState.Get());
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(impl.inputLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetVertexBuffers(0, 1, impl.vertexBuffer.GetAddressOf(), &stride, &offset);
	context->VSSetShader(impl.vertexShader.Get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, impl.settingsBuffer.GetAddressOf());
	context->PSSetConstantBuffers(0, 1, impl.settingsBuffer.GetAddressOf());
	context->PSSetShader(
		orderedViews ? impl.pixelShaderOrdered.Get() : impl.pixelShader.Get(), nullptr, 0);

	ID3D11ShaderResourceView* resources[6] = {
		impl.atlasView.Get(),
		impl.shadeView.Get(),
		impl.blendView.Get(),
		impl.backgroundView.Get(),
		impl.spriteAtlasView.Get(),
		impl.skyView.Get()
	};
	context->PSSetShaderResources(0, 6, resources);

	context->Draw(static_cast<UINT>(impl.vertices.size()), 0);

	if (orderedViews)
	{
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->OMSetRenderTargetsAndUnorderedAccessViews(
			0, nullptr, nullptr, 0, 1, &nullUav, nullptr);
	}
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShaderResources(0, 5, nullViews);
}

void GpuWorldRenderer::InvalidateTextureAtlas()
{
	if (m_impl)
	{
		m_impl->atlasDirty = true;
	}
}

GpuWorldRenderer::CompositeInfo GpuWorldRenderer::GetCompositeInfo() const
{
	return m_impl ? m_impl->composite : CompositeInfo{};
}

void GpuWorldRenderer::ConsumeComposite()
{
	if (m_impl)
	{
		m_impl->composite.valid = false;
	}
}

bool GpuWorldRenderer::ReadbackIndexTarget(uint8_t* destination, int destinationPitch)
{
	if (!m_impl || !m_impl->indexTarget || !destination)
	{
		return false;
	}

	Impl& impl = *m_impl;
	ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
	ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();
	if (!device || !context)
	{
		return false;
	}

	D3D11_TEXTURE2D_DESC description{};
	impl.indexTarget->GetDesc(&description);
	description.Usage = D3D11_USAGE_STAGING;
	description.BindFlags = 0;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ComPtr<ID3D11Texture2D> staging;
	HRESULT result = device->CreateTexture2D(&description, nullptr, &staging);
	if (FAILED(result))
	{
		impl.lastError = GpuHResultMessage("CreateTexture2D(world readback)", result);
		return false;
	}

	context->CopyResource(staging.Get(), impl.indexTarget.Get());

	D3D11_MAPPED_SUBRESOURCE mapped{};
	result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(result))
	{
		impl.lastError = GpuHResultMessage("Map(world readback)", result);
		return false;
	}

	for (UINT y = 0; y < description.Height; ++y)
	{
		std::memcpy(
			destination + static_cast<size_t>(y) * destinationPitch,
			static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
			description.Width);
	}

	context->Unmap(staging.Get(), 0);
	return true;
}

uint32_t GpuWorldRenderer::GetSubmittedTriangleCount() const
{
	return m_impl ? m_impl->submittedTriangles : 0;
}

uint32_t GpuWorldRenderer::GetRejectedTriangleCount() const
{
	return m_impl ? m_impl->rejectedTriangles : 0;
}

uint32_t GpuWorldRenderer::GetSubmittedSpriteCount() const
{
	return m_impl ? m_impl->submittedSprites : 0;
}

uint32_t GpuWorldRenderer::GetRejectedSpriteCount() const
{
	return m_impl ? m_impl->rejectedSprites : 0;
}

std::string GpuWorldRenderer::GetModeStatistics() const
{
	std::string statistics;
	if (!m_impl)
	{
		return statistics;
	}
	if (m_impl->culledTriangles != 0)
	{
		statistics += "culled=" + std::to_string(m_impl->culledTriangles);
	}
	for (int mode = 0; mode < 256; ++mode)
	{
		if (m_impl->modeCounts[mode] == 0)
		{
			continue;
		}
		if (!statistics.empty())
		{
			statistics += ", ";
		}
		statistics += "mode " + std::to_string(mode) + "=" +
			std::to_string(m_impl->modeCounts[mode]);
	}
	return statistics;
}

ID3D11ShaderResourceView* GpuWorldRenderer::GetIndexTargetView() const
{
	return m_impl ? m_impl->indexTargetSrv.Get() : nullptr;
}

#else

struct GpuWorldRenderer::Impl
{
	std::string lastError = "The GPU world renderer is only available on Windows";
	bool geometryEnabled = false;
};

GpuWorldRenderer::GpuWorldRenderer() : m_impl(std::make_unique<Impl>())
{
}

GpuWorldRenderer::~GpuWorldRenderer() = default;

GpuWorldRenderer& GpuWorldRenderer::Get()
{
	static GpuWorldRenderer instance;
	return instance;
}

bool GpuWorldRenderer::Initialize()
{
	return false;
}

void GpuWorldRenderer::Shutdown()
{
}

bool GpuWorldRenderer::IsReady() const
{
	return false;
}

const std::string& GpuWorldRenderer::GetLastError() const
{
	return m_impl->lastError;
}

void GpuWorldRenderer::SetGeometryEnabled(bool enabled)
{
	m_impl->geometryEnabled = enabled;
}

bool GpuWorldRenderer::IsGeometryEnabled() const
{
	return false;
}

void GpuWorldRenderer::SetSpritesEnabled(bool)
{
}

bool GpuWorldRenderer::AreSpritesEnabled() const
{
	return false;
}

void GpuWorldRenderer::SetExactBlendEnabled(bool)
{
}

void GpuWorldRenderer::SetTriangleCullMode(int)
{
}

void GpuWorldRenderer::SetSkyEnabled(bool)
{
}

bool GpuWorldRenderer::IsSkyEnabled() const
{
	return false;
}

bool GpuWorldRenderer::SubmitSky(const GpuSkyParams&)
{
	return false;
}

bool GpuWorldRenderer::SupportsDestinationReads() const
{
	return false;
}

std::string GpuWorldRenderer::GetOrderedViewsDiagnostic() const
{
	return std::string();
}

bool GpuWorldRenderer::IsCapturing() const
{
	return false;
}

bool GpuWorldRenderer::SupportsMode(uint8_t)
{
	return false;
}

void GpuWorldRenderer::BeginWorld(uint8_t*, int, int, int, int, int, int)
{
}

void GpuWorldRenderer::SubmitTriangle(
	const GpuWorldVertex&, const GpuWorldVertex&, const GpuWorldVertex&,
	uint32_t, uint8_t, uint8_t, uint8_t)
{
}

bool GpuWorldRenderer::SubmitSprite(const GpuSpriteQuad&)
{
	return false;
}

void GpuWorldRenderer::NoteSpriteRejected()
{
}

void GpuWorldRenderer::EndWorld(const uint8_t*, const uint8_t*, uint32_t)
{
}

void GpuWorldRenderer::InvalidateTextureAtlas()
{
}

GpuWorldRenderer::CompositeInfo GpuWorldRenderer::GetCompositeInfo() const
{
	return CompositeInfo{};
}

void GpuWorldRenderer::ConsumeComposite()
{
}

bool GpuWorldRenderer::ReadbackIndexTarget(uint8_t*, int)
{
	return false;
}

uint32_t GpuWorldRenderer::GetSubmittedTriangleCount() const
{
	return 0;
}

uint32_t GpuWorldRenderer::GetRejectedTriangleCount() const
{
	return 0;
}

uint32_t GpuWorldRenderer::GetSubmittedSpriteCount() const
{
	return 0;
}

uint32_t GpuWorldRenderer::GetRejectedSpriteCount() const
{
	return 0;
}

std::string GpuWorldRenderer::GetModeStatistics() const
{
	return std::string();
}

#endif
