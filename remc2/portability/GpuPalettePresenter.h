#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Color;
struct SDL_Window;

// Presents the engine's native 8-bit framebuffer without first expanding it
// to RGB on the CPU. On Windows the implementation uploads the palette indices
// and the 256-colour palette to D3D11 and resolves both in a pixel shader.
class GpuPalettePresenter
{
public:
	GpuPalettePresenter();
	~GpuPalettePresenter();

	GpuPalettePresenter(const GpuPalettePresenter&) = delete;
	GpuPalettePresenter& operator=(const GpuPalettePresenter&) = delete;

	bool Initialize(SDL_Window* window);
	bool Present(
		const uint8_t* pixels,
		int pitch,
		int textureWidth,
		int textureHeight,
		int sourceWidth,
		int sourceHeight,
		const SDL_Color* palette,
		bool maintainAspectRatio);
	void Shutdown();

	bool IsReady() const;
	const std::string& GetLastError() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
