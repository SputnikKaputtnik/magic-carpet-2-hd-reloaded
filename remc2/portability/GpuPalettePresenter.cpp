#include "GpuPalettePresenter.h"

#include "GpuRenderDevice.h"
#include "GpuWorldRenderer.h"

#include <SDL2/SDL.h>

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr const char* SettingsBlock = R"(
cbuffer FrameSettings : register(b0)
{
    float2 uvScale;
    float2 sourceSize;
    int4 worldViewport;
    uint worldValid;
    uint3 settingsPadding;
    float warpMix;
    float warpAlpha;
    float2 warpPadding;
};
)";

	constexpr const char* VertexShaderBody = R"(
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    output.uv = uv * uvScale;
    return output;
}
)";

	// Resolves the palette and, where the GPU rasterised the world, composes the
	// CPU overlay on top of it.  Index 0 inside the world viewport means "the CPU
	// did not draw here", which is the same convention the engine already uses
	// for keyed sprite blits.
	//
	// During the exit warp the world pixels take a detour: the resolved world is
	// mixed with the trail texture the passes below maintain.  CPU pixels stay
	// out of the mix, so overlays drawn into the world area keep their edges -
	// the same order the software warp has, which blends before the HUD is
	// drawn.
	constexpr const char* PixelShaderBody = R"(
Texture2D<float> IndexTexture : register(t0);
Texture2D<float4> PaletteTexture : register(t1);
Texture2D<float> WorldIndexTexture : register(t2);
Texture2D<float4> TrailTexture : register(t3);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VertexOutput input) : SV_TARGET
{
    int2 texel = int2(input.uv * sourceSize);
    uint paletteIndex = (uint)(IndexTexture.Load(int3(texel, 0)) * 255.0f + 0.5f);

    if (worldValid != 0u && paletteIndex == 0u &&
        texel.x >= worldViewport.x && texel.x < worldViewport.z &&
        texel.y >= worldViewport.y && texel.y < worldViewport.w)
    {
        uint worldIndex = (uint)(WorldIndexTexture.Load(int3(texel, 0)) * 255.0f + 0.5f);
        float4 world = PaletteTexture.Load(int3((int)min(255u, worldIndex), 0, 0));
        if (warpMix > 0.0f)
        {
            world = lerp(world, TrailTexture.Load(int3(texel, 0)), warpMix);
        }
        return world;
    }

    return PaletteTexture.Load(int3((int)min(255u, paletteIndex), 0, 0));
}
)";

	// The exit warp, step one: the world indices resolved to RGB, 1:1 at game
	// resolution.  The trail lives behind the palette on purpose - in RGB there
	// is real fractional blending, which index space cannot offer (a blend
	// result must be an index the palette has, and dithering around that
	// limitation reads as grain).
	constexpr const char* WarpResolvePixelShaderBody = R"(
Texture2D<float> WorldIndexTexture : register(t0);
Texture2D<float4> PaletteTexture : register(t1);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VertexOutput input) : SV_TARGET
{
    int2 texel = int2(input.position.xy);
    uint worldIndex = (uint)(WorldIndexTexture.Load(int3(texel, 0)) * 255.0f + 0.5f);
    return PaletteTexture.Load(int3((int)min(255u, worldIndex), 0, 0));
}
)";

	// Step two: the trail buffer moves towards the current world by the blend
	// factor computed from elapsed time, an exponential decay whose constant is
	// a duration - the trail keeps its length whatever frame rate the renderer
	// reaches.  This is the accumulator the index-space attempt could only
	// approximate by dithering.
	constexpr const char* WarpAccumulatePixelShaderBody = R"(
Texture2D<float4> TrailTexture : register(t0);
Texture2D<float4> WorldTexture : register(t1);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(VertexOutput input) : SV_TARGET
{
    int2 texel = int2(input.position.xy);
    float4 trail = TrailTexture.Load(int3(texel, 0));
    float4 world = WorldTexture.Load(int3(texel, 0));
    return lerp(trail, world, warpAlpha);
}
)";

	bool CompileShader(
		const char* source,
		const char* target,
		ComPtr<ID3DBlob>& byteCode,
		std::string& error)
	{
		ComPtr<ID3DBlob> diagnostics;
		const HRESULT result = D3DCompile(
			source,
			std::strlen(source),
			nullptr,
			nullptr,
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

struct GpuPalettePresenter::Impl
{
	struct alignas(16) FrameSettings
	{
		float uvScale[2] = { 1.0f, 1.0f };
		float sourceSize[2] = { 1.0f, 1.0f };
		int32_t worldViewport[4] = { 0, 0, 0, 0 };
		uint32_t worldValid = 0;
		uint32_t padding[3] = { 0, 0, 0 };
		float warpMix = 0.0f;
		float warpAlpha = 0.0f;
		float warpPadding[2] = { 0.0f, 0.0f };
	};

	ComPtr<ID3D11VertexShader> vertexShader;
	ComPtr<ID3D11PixelShader> pixelShader;
	ComPtr<ID3D11Buffer> frameSettings;
	ComPtr<ID3D11SamplerState> pointSampler;
	ComPtr<ID3D11Texture2D> indexTexture;
	ComPtr<ID3D11ShaderResourceView> indexTextureView;
	ComPtr<ID3D11Texture2D> paletteTexture;
	ComPtr<ID3D11ShaderResourceView> paletteTextureView;
	int indexTextureWidth = 0;
	int indexTextureHeight = 0;
	bool ready = false;
	std::string lastError;

	// The exit warp trail, in RGB behind the palette resolve.  worldColour is
	// the resolved world of the current frame; the trail pair ping-pongs
	// because a texture cannot be sampled and rendered at once.
	struct WarpTarget
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> view;
		ComPtr<ID3D11ShaderResourceView> srv;
	};
	WarpTarget warpWorldColour;
	WarpTarget warpTrail[2];
	int warpTrailIndex = 0;
	int warpTargetWidth = 0;
	int warpTargetHeight = 0;
	bool warpWasActive = false;
	std::chrono::steady_clock::time_point warpLastFrame{};
	ComPtr<ID3D11PixelShader> warpResolveShader;
	ComPtr<ID3D11PixelShader> warpAccumulateShader;

	bool EnsureWarpTargets(int width, int height)
	{
		if (width == warpTargetWidth && height == warpTargetHeight &&
			warpWorldColour.texture)
		{
			return true;
		}

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC description{};
		description.Width = static_cast<UINT>(width);
		description.Height = static_cast<UINT>(height);
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		for (WarpTarget* target : { &warpWorldColour, &warpTrail[0], &warpTrail[1] })
		{
			target->srv.Reset();
			target->view.Reset();
			target->texture.Reset();
			if (FAILED(device->CreateTexture2D(&description, nullptr, &target->texture)) ||
				FAILED(device->CreateRenderTargetView(
					target->texture.Get(), nullptr, &target->view)) ||
				FAILED(device->CreateShaderResourceView(
					target->texture.Get(), nullptr, &target->srv)))
			{
				lastError = "CreateTexture2D(warp trail)";
				return false;
			}
		}

		warpTargetWidth = width;
		warpTargetHeight = height;
		warpWasActive = false;
		return true;
	}

	bool EnsureIndexTexture(int width, int height)
	{
		if (width == indexTextureWidth && height == indexTextureHeight && indexTexture)
		{
			return true;
		}

		indexTextureView.Reset();
		indexTexture.Reset();

		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			lastError = "GPU palette presenter has no device";
			return false;
		}

		D3D11_TEXTURE2D_DESC textureDescription{};
		textureDescription.Width = static_cast<UINT>(width);
		textureDescription.Height = static_cast<UINT>(height);
		textureDescription.MipLevels = 1;
		textureDescription.ArraySize = 1;
		textureDescription.Format = DXGI_FORMAT_R8_UNORM;
		textureDescription.SampleDesc.Count = 1;
		textureDescription.Usage = D3D11_USAGE_DYNAMIC;
		textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT result = device->CreateTexture2D(&textureDescription, nullptr, &indexTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(index)", result);
			return false;
		}

		result = device->CreateShaderResourceView(indexTexture.Get(), nullptr, &indexTextureView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(index)", result);
			return false;
		}

		indexTextureWidth = width;
		indexTextureHeight = height;
		return true;
	}

	bool CreatePaletteTexture()
	{
		ID3D11Device* device = GpuRenderDevice::Get().GetDevice();
		if (!device)
		{
			lastError = "GPU palette presenter has no device";
			return false;
		}

		D3D11_TEXTURE2D_DESC textureDescription{};
		textureDescription.Width = 256;
		textureDescription.Height = 1;
		textureDescription.MipLevels = 1;
		textureDescription.ArraySize = 1;
		textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDescription.SampleDesc.Count = 1;
		textureDescription.Usage = D3D11_USAGE_DYNAMIC;
		textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		HRESULT result = device->CreateTexture2D(&textureDescription, nullptr, &paletteTexture);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateTexture2D(palette)", result);
			return false;
		}

		result = device->CreateShaderResourceView(paletteTexture.Get(), nullptr, &paletteTextureView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateShaderResourceView(palette)", result);
			return false;
		}
		return true;
	}

	bool UploadFrame(const uint8_t* pixels, int pitch, int width, int height)
	{
		ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT result = context->Map(indexTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("Map(index)", result);
			return false;
		}

		for (int y = 0; y < height; ++y)
		{
			std::memcpy(
				static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
				pixels + static_cast<size_t>(y) * pitch,
				static_cast<size_t>(width));
		}
		context->Unmap(indexTexture.Get(), 0);
		return true;
	}

	bool UploadPalette(const SDL_Color* palette)
	{
		ID3D11DeviceContext* context = GpuRenderDevice::Get().GetContext();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT result = context->Map(paletteTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("Map(palette)", result);
			return false;
		}

		auto* destination = static_cast<uint8_t*>(mapped.pData);
		for (int index = 0; index < 256; ++index)
		{
			destination[index * 4 + 0] = palette[index].r;
			destination[index * 4 + 1] = palette[index].g;
			destination[index * 4 + 2] = palette[index].b;
			destination[index * 4 + 3] = 255;
		}
		context->Unmap(paletteTexture.Get(), 0);
		return true;
	}
};

GpuPalettePresenter::GpuPalettePresenter() : m_impl(std::make_unique<Impl>())
{
}

GpuPalettePresenter::~GpuPalettePresenter()
{
	Shutdown();
}

bool GpuPalettePresenter::Initialize(SDL_Window* window)
{
	Shutdown();
	m_impl = std::make_unique<Impl>();

	GpuRenderDevice& gpuDevice = GpuRenderDevice::Get();
	if (!gpuDevice.IsReady() && !gpuDevice.Initialize(window))
	{
		m_impl->lastError = gpuDevice.GetLastError();
		return false;
	}

	ID3D11Device* device = gpuDevice.GetDevice();

	const std::string vertexSource = std::string(SettingsBlock) + VertexShaderBody;
	const std::string pixelSource = std::string(SettingsBlock) + PixelShaderBody;

	ComPtr<ID3DBlob> vertexShaderByteCode;
	if (!CompileShader(vertexSource.c_str(), "vs_4_0", vertexShaderByteCode, m_impl->lastError))
	{
		return false;
	}
	HRESULT result = device->CreateVertexShader(
		vertexShaderByteCode->GetBufferPointer(),
		vertexShaderByteCode->GetBufferSize(),
		nullptr,
		&m_impl->vertexShader);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateVertexShader", result);
		return false;
	}

	ComPtr<ID3DBlob> pixelShaderByteCode;
	if (!CompileShader(pixelSource.c_str(), "ps_4_0", pixelShaderByteCode, m_impl->lastError))
	{
		return false;
	}
	result = device->CreatePixelShader(
		pixelShaderByteCode->GetBufferPointer(),
		pixelShaderByteCode->GetBufferSize(),
		nullptr,
		&m_impl->pixelShader);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreatePixelShader", result);
		return false;
	}

	// The exit warp passes.  Failure is not fatal - the presenter still works,
	// the warp just stays on the engine's software path, and the availability
	// flag on the world renderer is what tells the engine which one runs.
	{
		const std::string resolveSource = std::string(SettingsBlock) + WarpResolvePixelShaderBody;
		const std::string accumulateSource =
			std::string(SettingsBlock) + WarpAccumulatePixelShaderBody;
		ComPtr<ID3DBlob> resolveByteCode;
		ComPtr<ID3DBlob> accumulateByteCode;
		std::string warpError;
		if (CompileShader(resolveSource.c_str(), "ps_4_0", resolveByteCode, warpError) &&
			CompileShader(accumulateSource.c_str(), "ps_4_0", accumulateByteCode, warpError) &&
			SUCCEEDED(device->CreatePixelShader(
				resolveByteCode->GetBufferPointer(),
				resolveByteCode->GetBufferSize(),
				nullptr,
				&m_impl->warpResolveShader)) &&
			SUCCEEDED(device->CreatePixelShader(
				accumulateByteCode->GetBufferPointer(),
				accumulateByteCode->GetBufferSize(),
				nullptr,
				&m_impl->warpAccumulateShader)))
		{
			GpuWorldRenderer::Get().SetWarpTrailAvailable(true);
		}
		else
		{
			m_impl->warpResolveShader.Reset();
			m_impl->warpAccumulateShader.Reset();
			GpuWorldRenderer::Get().SetWarpTrailAvailable(false);
		}
	}

	D3D11_BUFFER_DESC constantBufferDescription{};
	constantBufferDescription.ByteWidth = sizeof(Impl::FrameSettings);
	constantBufferDescription.Usage = D3D11_USAGE_DYNAMIC;
	constantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	constantBufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	result = device->CreateBuffer(&constantBufferDescription, nullptr, &m_impl->frameSettings);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateBuffer(frame settings)", result);
		return false;
	}

	D3D11_SAMPLER_DESC samplerDescription{};
	samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
	result = device->CreateSamplerState(&samplerDescription, &m_impl->pointSampler);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("CreateSamplerState", result);
		return false;
	}

	if (!m_impl->CreatePaletteTexture())
	{
		return false;
	}

	m_impl->ready = true;
	return true;
}

bool GpuPalettePresenter::Present(
	const uint8_t* pixels,
	int pitch,
	int textureWidth,
	int textureHeight,
	int sourceWidth,
	int sourceHeight,
	const SDL_Color* palette,
	bool maintainAspectRatio)
{
	if (!m_impl || !m_impl->ready || !pixels || !palette)
	{
		return false;
	}

	GpuRenderDevice& gpuDevice = GpuRenderDevice::Get();
	if (!gpuDevice.EnsureOutputSize())
	{
		m_impl->lastError = gpuDevice.GetLastError();
		return false;
	}

	const int outputWidth = gpuDevice.GetOutputWidth();
	const int outputHeight = gpuDevice.GetOutputHeight();
	if (outputWidth <= 0 || outputHeight <= 0)
	{
		return true;
	}
	if (!m_impl->EnsureIndexTexture(textureWidth, textureHeight))
	{
		return false;
	}
	if (!m_impl->UploadFrame(pixels, pitch, textureWidth, textureHeight) ||
		!m_impl->UploadPalette(palette))
	{
		return false;
	}

	GpuWorldRenderer& worldRenderer = GpuWorldRenderer::Get();
	const GpuWorldRenderer::CompositeInfo world = worldRenderer.GetCompositeInfo();
	const bool worldUsable =
		world.valid &&
		world.targetWidth == textureWidth &&
		world.targetHeight == textureHeight &&
		worldRenderer.GetIndexTargetView() != nullptr;

	Impl::FrameSettings settings;
	settings.uvScale[0] = static_cast<float>(sourceWidth) / static_cast<float>(textureWidth);
	settings.uvScale[1] = static_cast<float>(sourceHeight) / static_cast<float>(textureHeight);
	settings.sourceSize[0] = static_cast<float>(textureWidth);
	settings.sourceSize[1] = static_cast<float>(textureHeight);
	if (worldUsable)
	{
		settings.worldViewport[0] = world.viewportX;
		settings.worldViewport[1] = world.viewportY;
		settings.worldViewport[2] = world.viewportX + world.viewportWidth;
		settings.worldViewport[3] = world.viewportY + world.viewportHeight;
		settings.worldValid = 1;
	}

	// The exit warp.  The engine has requested it during the world pass; here,
	// behind the palette resolve, the trail is a real fractional blend in RGB -
	// index space cannot mix by a fraction, which is why the effect lives in
	// the presenter rather than in the world renderer.
	const bool warpNow = worldUsable &&
		worldRenderer.IsWarpBlurEnabled() &&
		m_impl->warpResolveShader && m_impl->warpAccumulateShader &&
		m_impl->EnsureWarpTargets(textureWidth, textureHeight);
	if (warpNow)
	{
		const auto now = std::chrono::steady_clock::now();
		float frameSeconds = 1.0f / 60.0f;
		if (m_impl->warpWasActive)
		{
			frameSeconds = std::chrono::duration<float>(now - m_impl->warpLastFrame).count();
			// A pause or a load can put minutes between two warp frames;
			// clamping keeps that from wiping the trail in one step.
			frameSeconds = std::min(std::max(frameSeconds, 0.0f), 0.1f);
		}
		m_impl->warpLastFrame = now;

		const float decay = std::max(worldRenderer.GetWarpDecaySeconds(), 0.001f);
		settings.warpAlpha = 1.0f - std::exp(-frameSeconds / decay);
		settings.warpMix =
			std::min(std::max(worldRenderer.GetWarpStrength(), 0.0f), 1.0f);
	}

	ID3D11DeviceContext* context = gpuDevice.GetContext();

	D3D11_MAPPED_SUBRESOURCE mapped{};
	HRESULT result = context->Map(
		m_impl->frameSettings.Get(),
		0,
		D3D11_MAP_WRITE_DISCARD,
		0,
		&mapped);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("Map(frame settings)", result);
		return false;
	}
	std::memcpy(mapped.pData, &settings, sizeof(settings));
	context->Unmap(m_impl->frameSettings.Get(), 0);

	if (warpNow)
	{
		// Both passes run 1:1 at game resolution, restricted to the world
		// rectangle; the fullscreen triangle maps onto whatever viewport is
		// set, and SV_POSITION then is the texel coordinate.
		D3D11_VIEWPORT worldRect{};
		worldRect.TopLeftX = static_cast<float>(world.viewportX);
		worldRect.TopLeftY = static_cast<float>(world.viewportY);
		worldRect.Width = static_cast<float>(world.viewportWidth);
		worldRect.Height = static_cast<float>(world.viewportHeight);
		worldRect.MaxDepth = 1.0f;

		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);
		context->RSSetViewports(1, &worldRect);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(m_impl->vertexShader.Get(), nullptr, 0);
		context->VSSetConstantBuffers(0, 1, m_impl->frameSettings.GetAddressOf());
		context->PSSetConstantBuffers(0, 1, m_impl->frameSettings.GetAddressOf());

		// The world of this frame, resolved to RGB.
		ID3D11ShaderResourceView* resolveResources[2] = {
			worldRenderer.GetIndexTargetView(),
			m_impl->paletteTextureView.Get()
		};
		context->OMSetRenderTargets(1, m_impl->warpWorldColour.view.GetAddressOf(), nullptr);
		context->PSSetShader(m_impl->warpResolveShader.Get(), nullptr, 0);
		context->PSSetShaderResources(0, 2, resolveResources);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		Impl::WarpTarget& trail = m_impl->warpTrail[m_impl->warpTrailIndex];
		Impl::WarpTarget& nextTrail = m_impl->warpTrail[m_impl->warpTrailIndex ^ 1];
		if (!m_impl->warpWasActive)
		{
			// First frame of a warp: the trail starts as the world, so the
			// effect fades in instead of mixing in stale buffer content.
			context->CopyResource(trail.texture.Get(), m_impl->warpWorldColour.texture.Get());
			m_impl->warpWasActive = true;
		}

		// The trail moves towards the world by the elapsed-time fraction.
		ID3D11ShaderResourceView* accumulateResources[2] = {
			trail.srv.Get(),
			m_impl->warpWorldColour.srv.Get()
		};
		context->OMSetRenderTargets(1, nextTrail.view.GetAddressOf(), nullptr);
		context->PSSetShader(m_impl->warpAccumulateShader.Get(), nullptr, 0);
		context->PSSetShaderResources(0, 2, accumulateResources);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		ID3D11ShaderResourceView* nullWarpResources[2] = { nullptr, nullptr };
		context->PSSetShaderResources(0, 2, nullWarpResources);
		m_impl->warpTrailIndex ^= 1;
	}
	else
	{
		m_impl->warpWasActive = false;
	}

	D3D11_VIEWPORT viewport{};
	if (maintainAspectRatio)
	{
		const float widthRatio = static_cast<float>(outputWidth) / sourceWidth;
		const float heightRatio = static_cast<float>(outputHeight) / sourceHeight;
		const float scale = std::min(widthRatio, heightRatio);
		viewport.Width = sourceWidth * scale;
		viewport.Height = sourceHeight * scale;
		viewport.TopLeftX = (outputWidth - viewport.Width) * 0.5f;
		viewport.TopLeftY = (outputHeight - viewport.Height) * 0.5f;
	}
	else
	{
		viewport.Width = static_cast<float>(outputWidth);
		viewport.Height = static_cast<float>(outputHeight);
	}
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	constexpr float clearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ID3D11RenderTargetView* backBufferView = gpuDevice.GetBackBufferView();
	context->OMSetRenderTargets(1, &backBufferView, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(nullptr);
	context->ClearRenderTargetView(backBufferView, clearColour);
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(m_impl->vertexShader.Get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, m_impl->frameSettings.GetAddressOf());
	context->PSSetShader(m_impl->pixelShader.Get(), nullptr, 0);
	context->PSSetConstantBuffers(0, 1, m_impl->frameSettings.GetAddressOf());
	ID3D11ShaderResourceView* resources[] = {
		m_impl->indexTextureView.Get(),
		m_impl->paletteTextureView.Get(),
		worldUsable ? worldRenderer.GetIndexTargetView() : nullptr,
		// The freshly accumulated trail; the index flipped after the passes.
		warpNow ? m_impl->warpTrail[m_impl->warpTrailIndex].srv.Get() : nullptr
	};
	context->PSSetShaderResources(0, 4, resources);
	context->PSSetSamplers(0, 1, m_impl->pointSampler.GetAddressOf());
	context->Draw(3, 0);

	ID3D11ShaderResourceView* nullResources[4] = { nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 4, nullResources);

	worldRenderer.ConsumeComposite();

	result = gpuDevice.GetSwapChain()->Present(0, 0);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("IDXGISwapChain::Present", result);
		return false;
	}
	return true;
}

void GpuPalettePresenter::Shutdown()
{
	if (!m_impl)
	{
		return;
	}
	m_impl.reset();
}

bool GpuPalettePresenter::IsReady() const
{
	return m_impl && m_impl->ready;
}

const std::string& GpuPalettePresenter::GetLastError() const
{
	static const std::string noError;
	return m_impl ? m_impl->lastError : noError;
}

#else

struct GpuPalettePresenter::Impl
{
	std::string lastError = "The D3D11 palette presenter is only available on Windows";
};

GpuPalettePresenter::GpuPalettePresenter() : m_impl(std::make_unique<Impl>())
{
}

GpuPalettePresenter::~GpuPalettePresenter() = default;

bool GpuPalettePresenter::Initialize(SDL_Window*)
{
	return false;
}

bool GpuPalettePresenter::Present(
	const uint8_t*, int, int, int, int, int, const SDL_Color*, bool)
{
	return false;
}

void GpuPalettePresenter::Shutdown()
{
}

bool GpuPalettePresenter::IsReady() const
{
	return false;
}

const std::string& GpuPalettePresenter::GetLastError() const
{
	return m_impl->lastError;
}

#endif
