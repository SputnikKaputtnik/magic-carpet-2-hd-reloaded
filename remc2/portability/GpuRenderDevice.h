#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Window;

#ifdef _WIN32
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;
#endif

// Owns the single D3D11 device, context and swap chain that every GPU render
// stage of the HD renderer shares.  The palette presenter composes the final
// frame, the world renderer rasterises geometry into an offscreen index target;
// both need to talk to the same device, so it lives here instead of inside one
// of them.
class GpuRenderDevice
{
public:
	static GpuRenderDevice& Get();

	GpuRenderDevice(const GpuRenderDevice&) = delete;
	GpuRenderDevice& operator=(const GpuRenderDevice&) = delete;

	bool Initialize(SDL_Window* window);
	void Shutdown();

	bool IsReady() const;
	const std::string& GetLastError() const;
	void SetLastError(const std::string& message);

	// Resizes the swap chain to the current client area when needed.  Returns
	// false only on a real device error; a zero-sized client area succeeds and
	// leaves the output size at 0.
	bool EnsureOutputSize();
	int GetOutputWidth() const;
	int GetOutputHeight() const;

#ifdef _WIN32
	ID3D11Device* GetDevice() const;
	ID3D11DeviceContext* GetContext() const;
	IDXGISwapChain* GetSwapChain() const;
	ID3D11RenderTargetView* GetBackBufferView() const;
#endif

private:
	GpuRenderDevice();
	~GpuRenderDevice();

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// Formats "<operation> failed (HRESULT 0x...)" for the GPU stages.
std::string GpuHResultMessage(const char* operation, long result);
