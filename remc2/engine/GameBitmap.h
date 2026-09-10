#pragma once

#ifndef GAME_BITMAP
#define GAME_BITMAP

#include "Basic.h"

class GameBitmap
{
public:
    static void DrawColourizedBitmap(uint8_t* ptrBitmapData, uint8_t colour, uint8_t* ptrScreenBuffer, uint32_t stride, int16_t posX, int16_t posY, uint8_t height, uint8_t scale = 1);
	static void DrawColourizedBitmap(int16_t posX, int16_t posY, bitmap_pos_struct_t a3, uint8_t colour, uint8_t scale = 1);
	static void DrawBitmap(uint8_t* ptrBitmapData, uint8_t* ptrScreenBuffer, uint32_t stride, int16_t posX, int16_t posY, uint8_t height, uint8_t scale = 1);
	static void DrawBitmap(uint8_t* ptrBitmapData, uint8_t* ptrScreenBuffer, uint32_t stride, int16_t posX, int16_t posY, uint16_t width, uint16_t height, uint8_t v134);
	static void DrawTransparentBitmap_2DE80(int16_t posX, int16_t posY, bitmap_pos_struct_t a3, uint8_t scale = 1);
	// Draws an RLE bitmap stretched to an arbitrary screen rectangle (nearest
	// neighbour, transparency preserved).  The integer scale of DrawBitmap
	// cannot fill a 4K screen from 640x480 art exactly, and its clipping paths
	// are not scale aware; this decodes the tile once and maps it freely.
	static void DrawBitmapStretched(
		const bitmap_pos_struct_t& bitmap,
		uint8_t* ptrScreenBuffer,
		uint32_t stride,
		int destinationLeft,
		int destinationTop,
		int destinationRight,
		int destinationBottom);
	static void DrawMenuGraphic(uint16_t width, uint16_t height, uint8_t scale, uint8_t* ptrSrc, uint8_t* ptrDest);
	static void ScaleMenuGraphic(uint16_t height, uint8_t scale, uint8_t* ptrSrc, uint8_t* ptrDest);
	static void PaletteToRgb(uint8_t* ptrPalette, uint8_t colorAddressIdx, uint8_t truColorOut[3]);
	static void PaletteToRgba(uint8_t* ptrPalette, uint8_t colorIdx, uint8_t truColorOut[4]);
	static uint8_t DeriveBlendAlpha(uint8_t* ptrPalette, uint8_t srcIndex, uint8_t dstIndex, uint8_t resultIndex);
};

#endif //GAME_BITMAP