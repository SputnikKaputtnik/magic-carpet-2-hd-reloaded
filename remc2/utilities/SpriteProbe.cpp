#include "SpriteProbe.h"

#include <array>
#include <cstring>
#include <vector>

#include "../engine/globals.h"
#include "../portability/port_filesystem.h"
#include "../engine/CommandLineParser.h"

namespace
{
	constexpr int CaseCount = 8;      // dword0x1e selects one of eight octants
	constexpr int AnchorCount = 3;    // DrawSprite_41BD3 is called with a1 = 0, 1, 2

	int g_samplesPerCase = 0;
	int g_samplesTaken[AnchorCount][CaseCount] = {};
	int g_totalLogged = 0;

	bool g_snapshotHeld = false;
	std::vector<uint8_t> g_snapshot;

	struct EntryState
	{
		int32_t screenX = 0;
		int32_t screenY = 0;
	};
	EntryState g_entry;

	inline size_t BufferPixels()
	{
		return static_cast<size_t>(iScreenWidth_DE560) * screenHeight_180624;
	}

	// A case is worth sampling only until we have enough examples of it.
	bool WantsSample(uint32_t anchorMode, int32_t rotationCase)
	{
		if (g_samplesPerCase <= 0)
			return false;
		if (anchorMode >= AnchorCount || rotationCase < 0 || rotationCase >= CaseCount)
			return false;
		return g_samplesTaken[anchorMode][rotationCase] < g_samplesPerCase;
	}
}

bool SpriteProbe::IsActive()
{
	// Picked up lazily from --probe_sprites, so no startup hook is needed.
	static bool initialised = false;
	if (!initialised)
	{
		initialised = true;
		if (g_samplesPerCase == 0)
			g_samplesPerCase = CommandLineParams.GetProbeSprites();
	}
	return g_samplesPerCase > 0;
}

void SpriteProbe::SetSamplesPerCase(int samplesPerCase)
{
	g_samplesPerCase = samplesPerCase;
	std::memset(g_samplesTaken, 0, sizeof(g_samplesTaken));
	g_totalLogged = 0;
}

bool SpriteProbe::Begin(
	const type_F2C20ar& parameters, uint32_t anchorMode, const uint8_t* screenBuffer)
{
	g_snapshotHeld = false;
	if (!IsActive() || !screenBuffer)
		return false;
	if (!WantsSample(anchorMode, parameters.dword0x1e))
		return false;

	const size_t pixels = BufferPixels();
	if (pixels == 0)
		return false;

	g_snapshot.resize(pixels);
	std::memcpy(g_snapshot.data(), screenBuffer, pixels);

	// The anchor correction mutates the struct in place, so keep the values the
	// caller handed in; the port needs both sides of that correction.
	g_entry.screenX = parameters.dword0x03_screenX;
	g_entry.screenY = parameters.dword0x04_screenY;
	g_snapshotHeld = true;
	return true;
}

void SpriteProbe::End(
	const type_F2C20ar& parameters, uint32_t anchorMode, const uint8_t* screenBuffer)
{
	if (!g_snapshotHeld || !screenBuffer)
	{
		g_snapshotHeld = false;
		return;
	}
	g_snapshotHeld = false;

	const int pitch = iScreenWidth_DE560;
	const int height = static_cast<int>(screenHeight_180624);

	int changed = 0;
	int minX = pitch, maxX = -1, minY = height, maxY = -1;
	// Extreme points double as the corners of the written parallelogram.
	int atMinX[2] = { -1, -1 }, atMaxX[2] = { -1, -1 };
	int atMinY[2] = { -1, -1 }, atMaxY[2] = { -1, -1 };

	for (int y = 0; y < height; ++y)
	{
		const size_t row = static_cast<size_t>(y) * pitch;
		for (int x = 0; x < pitch; ++x)
		{
			if (g_snapshot[row + x] == screenBuffer[row + x])
				continue;

			++changed;
			if (x < minX) { minX = x; atMinX[0] = x; atMinX[1] = y; }
			if (x > maxX) { maxX = x; atMaxX[0] = x; atMaxX[1] = y; }
			if (y < minY) { minY = y; atMinY[0] = x; atMinY[1] = y; }
			if (y > maxY) { maxY = y; atMaxY[0] = x; atMaxY[1] = y; }
		}
	}

	if (changed == 0)
		return;

	const auto colourAt = [&](const int point[2]) -> int {
		if (point[0] < 0 || point[1] < 0)
			return -1;
		return screenBuffer[static_cast<size_t>(point[1]) * pitch + point[0]];
	};

	// Corner texels of the source bitmap, so the corner-to-corner assignment can
	// be resolved offline instead of guessed.
	const auto* source = reinterpret_cast<const uint8_t*>(parameters.dword0x02_data);
	const int sourceWidth = parameters.dword0x08_width;
	const int sourceHeight = parameters.dword0x06_height;
	int sourceCorners[4] = { -1, -1, -1, -1 };
	if (source && sourceWidth > 0 && sourceHeight > 0)
	{
		sourceCorners[0] = source[0];
		sourceCorners[1] = source[sourceWidth - 1];
		sourceCorners[2] = source[static_cast<size_t>(sourceHeight - 1) * sourceWidth];
		sourceCorners[3] = source[static_cast<size_t>(sourceHeight - 1) * sourceWidth + sourceWidth - 1];
	}

	++g_samplesTaken[anchorMode][parameters.dword0x1e];
	++g_totalLogged;
	const bool reportNow = (g_totalLogged % (AnchorCount * CaseCount)) == 0;

	Logger->info(
		"SpriteProbe case={} anchor={} "
		"in{{sxEntry={} syEntry={} sx={} sy={} rw={} rh={} w={} h={} "
		"sin={} cos={} b1b={} b1f={} b21={} b27={} w25={} h26={} stride={} actIdx={}}} "
		"out{{changed={} bbox=({},{})-({},{}) "
		"cMinX=({},{}) cMaxX=({},{}) cMinY=({},{}) cMaxY=({},{}) "
		"colMinX={} colMaxX={} colMinY={} colMaxY={} "
		"src={},{},{},{}}}",
		parameters.dword0x1e, anchorMode,
		g_entry.screenX, g_entry.screenY,
		parameters.dword0x03_screenX, parameters.dword0x04_screenY,
		parameters.dword0x09_realWidth, parameters.dword0x0c_realHeight,
		sourceWidth, sourceHeight,
		parameters.sin_0x0d, parameters.cos_0x11,
		parameters.dword0x1b, parameters.dword0x1f, parameters.dword0x21,
		parameters.dword0x27, parameters.width0x25, parameters.height0x26,
		parameters.dword0x23_stride, parameters.dword0x0a_actIdx,
		changed, minX, minY, maxX, maxY,
		atMinX[0], atMinX[1], atMaxX[0], atMaxX[1],
		atMinY[0], atMinY[1], atMaxY[0], atMaxY[1],
		colourAt(atMinX), colourAt(atMaxX), colourAt(atMinY), colourAt(atMaxY),
		sourceCorners[0], sourceCorners[1], sourceCorners[2], sourceCorners[3]);

	if (reportNow)
		Report();
}

void SpriteProbe::Report()
{
	if (!IsActive())
		return;

	Logger->info("SpriteProbe summary: {} samples logged", g_totalLogged);
	for (int anchor = 0; anchor < AnchorCount; ++anchor)
	{
		for (int rotationCase = 0; rotationCase < CaseCount; ++rotationCase)
		{
			Logger->info(
				"SpriteProbe coverage anchor={} case={} samples={}",
				anchor, rotationCase, g_samplesTaken[anchor][rotationCase]);
		}
	}
	Logger->flush();
}
