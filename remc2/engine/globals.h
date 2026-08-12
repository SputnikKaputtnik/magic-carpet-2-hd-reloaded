#pragma once

#include <cstdint>
#include "Type_F2C20ar.h"

extern uint8_t* pdwScreenBuffer_351628;
extern int16_t x_WORD_180660_VGA_type_resolution;
extern type_F2C20ar str_F2C20ar;
extern uint32_t screenHeight_180624;
extern uint32_t screenWidth_18062C;
extern int iScreenWidth_DE560;
extern uint8_t* ViewPortRenderBufferStart_DE558;
extern uint8_t* ViewPortRenderBufferAltStart_DE554;
// Versioning: 1.0 would suggest "finished", which this is not - the fork
// starts public life at 0.8.  Every bug fix bumps the patch digit (0.8.1,
// 0.8.2, ...); feature milestones move the minor version.
inline extern const char* VersionNumber = "Magic Carpet 2 HD - Reloaded fork 0.8.1";
