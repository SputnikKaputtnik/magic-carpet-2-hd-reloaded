#pragma once

#ifndef GAME_RENDER_INTERFACE
#define GAME_RENDER_INTERFACE

#include <cstdint>
#include "engine_support.h"
#include "ViewPort.h"

class GameRenderInterface 
{
public:
	virtual ~GameRenderInterface() {}
	virtual void DrawWorld_411A0(int posX, int posY, int16_t yaw, int16_t posZ, int16_t pitch, int16_t roll, int16_t fov) = 0;
	// Changing the view distance while a level runs.  Only the HD renderer has
	// a scalable tile grid; the others keep their fixed original distance.
	virtual void SetViewDistanceScale(uint8_t /*viewDistanceScale*/) {}
	virtual uint8_t GetViewDistanceScale() const { return 1; }
};

#endif //GAME_RENDER_INTERFACE