#pragma once

#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
struct ID3D11ShaderResourceView;
#endif

// One projected vertex as the HD software rasteriser produces it.
// X/Y are viewport relative pixel coordinates, U/V and Brightness are 16.16
// fixed point values whose bits 16..23 select the texel column/row and the
// shade level respectively - exactly what DrawTriangleInProjectionSpace_B6253
// extracts with its BYTE1/BYTE2 accesses.
struct GpuWorldVertex
{
	int32_t X = 0;
	int32_t Y = 0;
	int32_t U = 0;
	int32_t V = 0;
	int32_t Brightness = 0;
};

// One world sprite as DrawSprite_41BD3 blits it: a parallelogram in viewport
// relative pixel coordinates (the octant DDA of the CPU blit is a two-shear
// decomposition of one camera roll rotation) that samples an 8-bit bitmap
// affinely.  Corner order is P00, P10, P11, P01 along the sprite u/v axes;
// u0/u1 are the texel coordinates at the u=0 / u=w edges, v0/v1 at the
// v=0 / v=h edges (the caller bakes the pixel-centre correction in, the
// renderer adds the atlas offset).
struct GpuSpriteQuad
{
	float cornerX[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float cornerY[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
	const uint8_t* pixels = nullptr; // height rows of width bytes, stride = width
	int width = 0;
	int height = 0;
	uint8_t mode = 0;     // dword0x01_rotIdx: 0 copy, 1 shade LUT, 4/5 colour blend
	uint8_t constant = 0; // shade level (mode 1) or player colour (mode 4/5)
};

// The sky layer of DrawSky_40950: an 8-bit texture (256x256, or 1024x1024
// with big textures) projected over the viewport, rotated by the camera roll.
// All values are taken verbatim from the software loop, including its 16.16
// fixed point conventions, so the shader can reproduce it bit for bit.
struct GpuSkyParams
{
	int32_t beginX = 0;     // texture origin of viewport row 0, 16.16
	int32_t beginY = 0;
	int32_t sinRoll = 0;    // per row / per column steps, 16.16
	int32_t cosRoll = 0;
	int32_t textureSize = 256;
	const uint8_t* pixels = nullptr; // textureSize * textureSize bytes
};

// Rasterises the world geometry of the HD renderer on the GPU into an 8-bit
// palette index target, so the CPU no longer has to touch those pixels.  The
// class is deliberately free of D3D types in its interface: the engine side
// only speaks in projected vertices and the shading mode byte (x_BYTE_E126D).
class GpuWorldRenderer
{
public:
	static GpuWorldRenderer& Get();

	GpuWorldRenderer(const GpuWorldRenderer&) = delete;
	GpuWorldRenderer& operator=(const GpuWorldRenderer&) = delete;

	bool Initialize();
	void Shutdown();
	bool IsReady() const;
	const std::string& GetLastError() const;

	// Feature flag: geometry only leaves the CPU when this is on.
	void SetGeometryEnabled(bool enabled);
	bool IsGeometryEnabled() const;

	// Feature flag: world sprites join the terrain vertex stream when this is
	// on (requires the geometry path, sprites live in the same draw call).
	void SetSpritesEnabled(bool enabled);
	bool AreSpritesEnabled() const;

	// Feature flag: the sky layer is rendered by the GPU instead of the
	// software loop.  Only effective together with ordered views, because
	// without them the destination blend approximation reads the CPU drawn
	// background layer that the GPU sky would no longer produce.
	void SetSkyEnabled(bool enabled);
	bool IsSkyEnabled() const;

	// Emits the sky as the first quad of the frame, before any terrain.
	// Returns false when the caller has to run the software sky loop.
	bool SubmitSky(const GpuSkyParams& params);

	// Debug: reject terrain triangles by winding at submission time, which is
	// where the software rasteriser rejects them too (its equal-Y branches
	// bail out on the vertex X order).  0 = keep everything (previous
	// behaviour), 1 = drop clockwise, 2 = drop counter-clockwise.  Sprites and
	// the sky quad are never affected.
	void SetTriangleCullMode(int mode);

	// Feature flag: exact destination blending through rasterizer ordered
	// views (terrain mode 0x1A and the destination reading sprite modes).
	// Only effective when the hardware supports ROVs; off falls back to the
	// background-layer approximation.
	void SetExactBlendEnabled(bool enabled);
	// True when destination reading pixel modes render exactly on the GPU.
	bool SupportsDestinationReads() const;
	// Why ordered views are unavailable (empty when they are).
	std::string GetOrderedViewsDiagnostic() const;

	// True when the current frame is being captured for the GPU, i.e. the
	// software rasteriser must stay silent.
	bool IsCapturing() const;

	// The exit warp.  The engine renders the world into its blur buffer, blends
	// that against the screen - which still carries the previous frame - and
	// then leaves DrawWorld before the normal world pass, so the blended image
	// is the frame.  Because that single pass targets the blur buffer instead
	// of the screen, the GPU path declines it and the whole world falls back to
	// the software rasteriser while the warp lasts.
	//
	// The GPU version lives in the palette presenter, after the palette
	// resolve: in RGB there is real fractional blending, which index space
	// cannot offer (a blend result must be an index the palette has, and
	// dithering around that limitation reads as grain).  The renderer only
	// carries the request and its parameters; the presenter picks them up at
	// present time.  Set per frame, before the world pass.
	void SetWarpBlurEnabled(bool enabled) { m_warpBlurEnabled = enabled; }
	bool IsWarpBlurEnabled() const { return m_warpBlurEnabled; }
	// How long the trail lasts.  The trail buffer decays towards the current
	// world at a rate derived from elapsed time, so this is a duration and not
	// a number of frames - the effect keeps its length whatever frame rate the
	// renderer reaches.
	void SetWarpDecaySeconds(float seconds) { m_warpDecaySeconds = seconds; }
	float GetWarpDecaySeconds() const { return m_warpDecaySeconds; }
	// How much of the trail the displayed image carries, 0..1.
	void SetWarpStrength(float strength) { m_warpStrength = strength; }
	float GetWarpStrength() const { return m_warpStrength; }
	// Reported by the presenter once its warp passes compiled.  False means the
	// warp has to stay on the engine's software path.
	void SetWarpTrailAvailable(bool available) { m_warpTrailAvailable = available; }
	bool SupportsWarpBlur() const { return m_warpTrailAvailable; }

	// True for the shading modes the pixel shader reproduces exactly.
	static bool SupportsMode(uint8_t mode);

	// Called once per world frame after the CPU has filled the viewport with
	// the sky / background and before the first terrain triangle.  Takes a copy
	// of that background as the initial content of the index target and clears
	// the viewport region of the CPU buffer to index 0, which then marks
	// "nothing drawn by the CPU here" for the composition step.
	void BeginWorld(
		uint8_t* screenBuffer,
		int screenWidth,
		int screenHeight,
		int viewportX,
		int viewportY,
		int viewportWidth,
		int viewportHeight);

	void SubmitTriangle(
		const GpuWorldVertex& vertex1,
		const GpuWorldVertex& vertex2,
		const GpuWorldVertex& vertex3,
		uint32_t textureOffset,
		uint8_t mode,
		uint8_t constantShade,
		uint8_t textureSize);

	// Appends a sprite as two triangles to the same vertex stream as the
	// terrain, which keeps the painter order exact.  Returns false when the
	// sprite cannot be taken (capture off, sprites disabled, atlas full);
	// the caller then blits it on the CPU as before.
	bool SubmitSprite(const GpuSpriteQuad& quad);

	// Counts a world sprite the engine had to keep on the CPU while the GPU
	// was capturing (camera roll, unsupported pixel mode, atlas full).
	void NoteSpriteRejected();

	// Uploads the shared tables and flushes the batch into the index target.
	void EndWorld(
		const uint8_t* shadeTable,
		const uint8_t* textureAtlas,
		uint32_t textureAtlasBytes);

	// Marks the terrain atlas as changed (level load / texture preparation).
	void InvalidateTextureAtlas();

	struct CompositeInfo
	{
		bool valid = false;
		int viewportX = 0;
		int viewportY = 0;
		int viewportWidth = 0;
		int viewportHeight = 0;
		int targetWidth = 0;
		int targetHeight = 0;
	};

	// Describes the frame the presenter has to compose with the CPU buffer.
	CompositeInfo GetCompositeInfo() const;

	// Invalidates the composition data after it has been presented, so a frame
	// without world rendering (menus, briefings) does not reuse the last one.
	void ConsumeComposite();

	// Copies the rasterised palette indices back to system memory.  Only used by
	// the offline GPU/software pixel comparison, never in the normal frame path.
	bool ReadbackIndexTarget(uint8_t* destination, int destinationPitch);

	// Statistics for the render profiler.
	uint32_t GetSubmittedTriangleCount() const;
	// "mode=count" pairs, for verifying which rasteriser modes the world uses.
	std::string GetModeStatistics() const;
	uint32_t GetRejectedTriangleCount() const;
	uint32_t GetSubmittedSpriteCount() const;
	uint32_t GetRejectedSpriteCount() const;

#ifdef _WIN32
	ID3D11ShaderResourceView* GetIndexTargetView() const;
#endif

private:
	GpuWorldRenderer();
	~GpuWorldRenderer();

	struct Impl;
	std::unique_ptr<Impl> m_impl;

	// The exit warp request, carried here for the presenter (see above).
	bool m_warpBlurEnabled = false;
	bool m_warpTrailAvailable = false;
	float m_warpDecaySeconds = 0.35f;
	float m_warpStrength = 0.35f;
};
