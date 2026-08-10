#include "GpuRenderDevice.h"

#include <SDL2/SDL.h>

#include <iomanip>
#include <sstream>

std::string GpuHResultMessage(const char* operation, long result)
{
	std::ostringstream message;
	message << operation << " failed (HRESULT 0x"
		<< std::hex << std::uppercase << static_cast<unsigned long>(result) << ")";
	return message.str();
}

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <SDL2/SDL_syswm.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

using Microsoft::WRL::ComPtr;

struct GpuRenderDevice::Impl
{
	HWND windowHandle = nullptr;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D11RenderTargetView> backBufferView;
	int outputWidth = 0;
	int outputHeight = 0;
	bool ready = false;
	std::string lastError;

	bool CreateBackBufferView()
	{
		ComPtr<ID3D11Texture2D> backBuffer;
		HRESULT result = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("IDXGISwapChain::GetBuffer", result);
			return false;
		}

		result = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferView);
		if (FAILED(result))
		{
			lastError = GpuHResultMessage("CreateRenderTargetView", result);
			return false;
		}
		return true;
	}
};

GpuRenderDevice::GpuRenderDevice() : m_impl(std::make_unique<Impl>())
{
}

GpuRenderDevice::~GpuRenderDevice()
{
	Shutdown();
}

GpuRenderDevice& GpuRenderDevice::Get()
{
	static GpuRenderDevice instance;
	return instance;
}

bool GpuRenderDevice::Initialize(SDL_Window* window)
{
	Shutdown();
	m_impl = std::make_unique<Impl>();

	SDL_SysWMinfo windowInfo{};
	SDL_VERSION(&windowInfo.version);
	if (!SDL_GetWindowWMInfo(window, &windowInfo) || windowInfo.subsystem != SDL_SYSWM_WINDOWS)
	{
		m_impl->lastError = std::string("SDL_GetWindowWMInfo failed: ") + SDL_GetError();
		return false;
	}
	m_impl->windowHandle = windowInfo.info.win.window;

	RECT clientRect{};
	GetClientRect(m_impl->windowHandle, &clientRect);
	const UINT initialWidth = static_cast<UINT>(std::max(1L, clientRect.right - clientRect.left));
	const UINT initialHeight = static_cast<UINT>(std::max(1L, clientRect.bottom - clientRect.top));

	DXGI_SWAP_CHAIN_DESC swapChainDescription{};
	swapChainDescription.BufferCount = 2;
	swapChainDescription.BufferDesc.Width = initialWidth;
	swapChainDescription.BufferDesc.Height = initialHeight;
	swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDescription.OutputWindow = m_impl->windowHandle;
	swapChainDescription.SampleDesc.Count = 1;
	swapChainDescription.Windowed = TRUE;
	swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	// 11_1 first: drivers only report rasterizer ordered views on an 11.1+
	// device.  Runtimes without 11.1 reject the whole list with E_INVALIDARG,
	// so the creation retries without that entry.
	const std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
	};
	D3D_FEATURE_LEVEL selectedFeatureLevel{};

	HRESULT result = D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		featureLevels.data(),
		static_cast<UINT>(featureLevels.size()),
		D3D11_SDK_VERSION,
		&swapChainDescription,
		&m_impl->swapChain,
		&m_impl->device,
		&selectedFeatureLevel,
		&m_impl->context);

	if (result == E_INVALIDARG)
	{
		result = D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			featureLevels.data() + 1,
			static_cast<UINT>(featureLevels.size()) - 1,
			D3D11_SDK_VERSION,
			&swapChainDescription,
			&m_impl->swapChain,
			&m_impl->device,
			&selectedFeatureLevel,
			&m_impl->context);
	}

	if (FAILED(result))
	{
		result = D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			featureLevels.data(),
			static_cast<UINT>(featureLevels.size()),
			D3D11_SDK_VERSION,
			&swapChainDescription,
			&m_impl->swapChain,
			&m_impl->device,
			&selectedFeatureLevel,
			&m_impl->context);
	}

	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("D3D11CreateDeviceAndSwapChain", result);
		return false;
	}

	// DXGI installs its own Alt+Enter handler on the swap chain window.  The
	// game already toggles fullscreen through SDL, so both would fire on the
	// same key press and the window would bounce straight back.  Hand the key
	// back to SDL and keep DXGI out of the window state entirely.
	{
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> dxgiAdapter;
		ComPtr<IDXGIFactory> dxgiFactory;
		if (SUCCEEDED(m_impl->device.As(&dxgiDevice)) &&
			SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) &&
			SUCCEEDED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory))))
		{
			dxgiFactory->MakeWindowAssociation(
				m_impl->windowHandle, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		}
	}

	m_impl->outputWidth = static_cast<int>(initialWidth);
	m_impl->outputHeight = static_cast<int>(initialHeight);
	if (!m_impl->CreateBackBufferView())
	{
		return false;
	}

	m_impl->ready = true;
	return true;
}

void GpuRenderDevice::Shutdown()
{
	if (!m_impl)
	{
		return;
	}
	if (m_impl->context)
	{
		m_impl->context->ClearState();
		m_impl->context->Flush();
	}
	m_impl = std::make_unique<Impl>();
}

bool GpuRenderDevice::IsReady() const
{
	return m_impl && m_impl->ready;
}

const std::string& GpuRenderDevice::GetLastError() const
{
	static const std::string noError;
	return m_impl ? m_impl->lastError : noError;
}

void GpuRenderDevice::SetLastError(const std::string& message)
{
	if (m_impl)
	{
		m_impl->lastError = message;
	}
}

bool GpuRenderDevice::EnsureOutputSize()
{
	if (!IsReady())
	{
		return false;
	}

	RECT clientRect{};
	if (!GetClientRect(m_impl->windowHandle, &clientRect))
	{
		m_impl->lastError = "GetClientRect failed";
		return false;
	}

	const int width = clientRect.right - clientRect.left;
	const int height = clientRect.bottom - clientRect.top;
	if (width <= 0 || height <= 0)
	{
		return true;
	}

	if (width == m_impl->outputWidth && height == m_impl->outputHeight && m_impl->backBufferView)
	{
		return true;
	}

	m_impl->context->OMSetRenderTargets(0, nullptr, nullptr);
	m_impl->backBufferView.Reset();

	const HRESULT result = m_impl->swapChain->ResizeBuffers(
		0,
		static_cast<UINT>(width),
		static_cast<UINT>(height),
		DXGI_FORMAT_UNKNOWN,
		0);
	if (FAILED(result))
	{
		m_impl->lastError = GpuHResultMessage("IDXGISwapChain::ResizeBuffers", result);
		return false;
	}

	m_impl->outputWidth = width;
	m_impl->outputHeight = height;
	return m_impl->CreateBackBufferView();
}

int GpuRenderDevice::GetOutputWidth() const
{
	return m_impl ? m_impl->outputWidth : 0;
}

int GpuRenderDevice::GetOutputHeight() const
{
	return m_impl ? m_impl->outputHeight : 0;
}

ID3D11Device* GpuRenderDevice::GetDevice() const
{
	return m_impl ? m_impl->device.Get() : nullptr;
}

ID3D11DeviceContext* GpuRenderDevice::GetContext() const
{
	return m_impl ? m_impl->context.Get() : nullptr;
}

IDXGISwapChain* GpuRenderDevice::GetSwapChain() const
{
	return m_impl ? m_impl->swapChain.Get() : nullptr;
}

ID3D11RenderTargetView* GpuRenderDevice::GetBackBufferView() const
{
	return m_impl ? m_impl->backBufferView.Get() : nullptr;
}

#else

struct GpuRenderDevice::Impl
{
	std::string lastError = "The D3D11 render device is only available on Windows";
};

GpuRenderDevice::GpuRenderDevice() : m_impl(std::make_unique<Impl>())
{
}

GpuRenderDevice::~GpuRenderDevice() = default;

GpuRenderDevice& GpuRenderDevice::Get()
{
	static GpuRenderDevice instance;
	return instance;
}

bool GpuRenderDevice::Initialize(SDL_Window*)
{
	return false;
}

void GpuRenderDevice::Shutdown()
{
}

bool GpuRenderDevice::IsReady() const
{
	return false;
}

const std::string& GpuRenderDevice::GetLastError() const
{
	return m_impl->lastError;
}

void GpuRenderDevice::SetLastError(const std::string& message)
{
	m_impl->lastError = message;
}

bool GpuRenderDevice::EnsureOutputSize()
{
	return false;
}

int GpuRenderDevice::GetOutputWidth() const
{
	return 0;
}

int GpuRenderDevice::GetOutputHeight() const
{
	return 0;
}

#endif
