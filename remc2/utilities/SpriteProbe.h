#pragma once

#include <cstdint>

#include "../engine/Type_F2C20ar.h"

// Measures where DrawSprite_41BD3 actually writes, without touching a single
// line of its unrolled blit code: the screen buffer is snapshotted before the
// call and diffed afterwards.  The changed pixels of a rotated/sheared sprite
// form a parallelogram, so the four extreme points are its corners - together
// with the logged parameters that is enough to derive the affine mapping
// sprite texel -> screen pixel for every dword0x1e case.
//
// Enabled with --probe_sprites <samplesPerCase>; costs nothing when off.
namespace SpriteProbe
{
	bool IsActive();
	void SetSamplesPerCase(int samplesPerCase);

	// True while a snapshot is held, i.e. End() must be called.
	bool Begin(const type_F2C20ar& parameters, uint32_t anchorMode, const uint8_t* screenBuffer);
	void End(const type_F2C20ar& parameters, uint32_t anchorMode, const uint8_t* screenBuffer);

	// Writes the collected table to the log; called once at shutdown.
	void Report();
}
