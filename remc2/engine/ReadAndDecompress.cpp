#include "ReadAndDecompress.h"
#include "EventsFunctions.h"
#include "read_config.h"
#include "../portability/GpuWorldRenderer.h"

#include <filesystem>
uint8_t BigTextureBuffer[128 * 128 * 160];
uint32_t terrainBlockBufferBytes = 0;

namespace
{
	// Every write into a terrain block buffer goes through these two helpers so
	// the GPU world renderer learns both the valid length of the buffer and
	// that its content changed (day/night/cave switch reuse the same buffer).
	void LoadTerrainBlocks(const char* dataPath)
	{
		const int length = DataFileIO::ReadFileAndDecompress(dataPath, &BLOCK32DAT_BEGIN_BUFFER);//2bac2c
		terrainBlockBufferBytes = length > 0 ? static_cast<uint32_t>(length) : 0u;
		GpuWorldRenderer::Get().InvalidateTextureAtlas();
	}

	void LoadBigTerrainBlocks(const char* dataPath)
	{
		ReadGraphicsfile(dataPath, BigTextureBuffer);//advance graphics
		GpuWorldRenderer::Get().InvalidateTextureAtlas();
	}
}

//----- (00054630) --------------------------------------------------------
void sub_54630_load_psxblock(uint16_t TextSize)//235630
{
	switch (TextSize)
	{
	case 16:
		DataFileIO::LoadFileArray_84250(psxadatablock16dat);
		break;
	case 32:
		DataFileIO::LoadFileArray_84250(psxadatablock32dat);
		{
			// LoadFileArray_84250 takes the Pathstruct by value, so the size it
			// computed never reaches the global; ask the unpack header again.
			const int unpacked = DataFileIO::sub_AB9E1_get_file_unpack_size(xadatablock32dat.path);
			terrainBlockBufferBytes = unpacked > 0 ? static_cast<uint32_t>(unpacked) : 0u;
		}
		GpuWorldRenderer::Get().InvalidateTextureAtlas();
		break;
	case 128:
		break;
	}
}

//----- (00054660) --------------------------------------------------------
void sub_54660_read_and_decompress_sky_and_blocks(MapType_t GraphicsType, uint8_t GraphicsSize)//235660
{
	char dataPath[MAX_PATH];

	switch (GraphicsType)
	{
	case MapType_t::Day://basic graphics
	{
		switch (GraphicsSize)
		{
		case 16:
		{
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BLOCK16.DAT");
			LoadTerrainBlocks(dataPath);
			break;
		}
		case 32:
		{
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BLOCK32.DAT");
			LoadTerrainBlocks(dataPath);
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/SKYD0-0.DAT");
			DataFileIO::ReadFileAndDecompress(dataPath, &off_D41A8_sky);//2a51a8
			break;
		}
		case 128:
		{
			sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "block128.data");
			LoadBigTerrainBlocks(dataPath);
			sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "skyd1024.data");
			ReadGraphicsfile(dataPath, off_D41A8_sky);//2a51a8
			break;
		}
		}
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TMAPS0-0.TAB");
		DataFileIO::ReadFileAndDecompress(dataPath, (uint8_t**)&str_TMAPS00TAB_BEGIN_BUFFER);//2c7ed0
		break;
	}
	case MapType_t::Night://? and night
	{
		switch (GraphicsSize)
		{
		case 16:
		{
			if (D41A0_0.terrain_2FECE.byte_0x2FED2 & 2)
			{
				sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL16F0-0.DAT");
				LoadTerrainBlocks(dataPath);
			}
			else
			{
				sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL16N0-0.DAT");
				LoadTerrainBlocks(dataPath);
			}
			break;
		}
		case 32:
		{
			if (D41A0_0.terrain_2FECE.byte_0x2FED2 & 2)
			{
				sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL32F0-0.DAT");
				LoadTerrainBlocks(dataPath);
			}
			else
			{
				sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL32N0-0.DAT");
				LoadTerrainBlocks(dataPath);
			}
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/SKYN0-0.DAT");
			DataFileIO::ReadFileAndDecompress(dataPath, &off_D41A8_sky);//2a51a8
			break;
		}
		case 128:
		{
			if (D41A0_0.terrain_2FECE.byte_0x2FED2 & 2)
			{
				sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "bl128f0-0.data");
				LoadBigTerrainBlocks(dataPath);
			}
			else
			{
				sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "bl128n0-0.data");
				LoadBigTerrainBlocks(dataPath);
			}
			sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "skyn1024.data");
			ReadGraphicsfile(dataPath, off_D41A8_sky);//2a51a8
			break;
		}
		}
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TMAPS1-0.TAB");
		DataFileIO::ReadFileAndDecompress(dataPath, (uint8_t**)&str_TMAPS00TAB_BEGIN_BUFFER);//2c7ed0
		break;
	}
	case MapType_t::Cave://cave
	{
		switch (GraphicsSize)
		{
		case 16:
		{
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL16C0-0.DAT");
			LoadTerrainBlocks(dataPath);
			break;
		}
		case 32:
		{
			sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/BL32C0-0.DAT");
			LoadTerrainBlocks(dataPath);
			break;
		}
		case 128:
		{
			sprintf(dataPath, "%s/%s", highResGraphicsPath.c_str(), "bl128c0-0.data");
			LoadBigTerrainBlocks(dataPath);
			break;
		}
		}
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TMAPS2-0.TAB");
		DataFileIO::ReadFileAndDecompress(dataPath, (uint8_t**)&str_TMAPS00TAB_BEGIN_BUFFER);//2c7ed0
		break;
	}
	}
}


//----- (00054800) --------------------------------------------------------
void sub_54800_read_and_decompress_tables(MapType_t a1)//235800
{
	char dataPath[MAX_PATH];

	if (a1 == MapType_t::Day)
	{
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TABLESD.DAT");
		DataFileIO::ReadFileAndDecompress(dataPath, &x_BYTE_F6EE0_tablesx_pre);//2c7ee0
		keyColor2_D4B7E = 0x00;
		keyColor1_D4B7C = 0xfe;
	}
	else if (a1 == MapType_t::Night)
	{
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TABLESN.DAT");
		DataFileIO::ReadFileAndDecompress(dataPath, &x_BYTE_F6EE0_tablesx_pre);
		keyColor2_D4B7E = 0xff;
		keyColor1_D4B7C = 0x00;
	}
	else if (a1 == MapType_t::Cave)
	{
		sprintf(dataPath, "%s/%s", cdDataPath.c_str(), "DATA/TABLESC.DAT");
		DataFileIO::ReadFileAndDecompress(dataPath, &x_BYTE_F6EE0_tablesx_pre);
		keyColor1_D4B7C = 0xfe;
		keyColor2_D4B7E = 0xff;
	}
}

bool ToggleHighResTerrainTextures(std::string* message)
{
	const bool enable = (x_BYTE_D41B5_texture_size != 128);
	if (enable)
	{
		if (highResGraphicsPath.empty() || !std::filesystem::is_directory(highResGraphicsPath))
		{
			if (message)
				*message = "High-res graphics folder not found";
			return false;
		}
	}
	else if (!BLOCK32DAT_BEGIN_BUFFER)
	{
		// The game started with the high-res set, so the CD block buffer was
		// never allocated (sub_54630_load_psxblock skips size 128).  Allocate
		// it the way the start-up path would.
		if (DataFileIO::UnpackAndLoadMemoryFromPath(xadatablock32dat) <= 0 || !BLOCK32DAT_BEGIN_BUFFER)
		{
			if (message)
				*message = "BLOCK32.DAT could not be loaded";
			return false;
		}
	}

	bigTextures = enable;
	texturepixels = enable ? 128 : 32;
	x_BYTE_D41B5_texture_size = static_cast<uint8_t>(texturepixels);

	// Blocks and sky for the current map type (the loaders invalidate the GPU
	// atlas), then the tile address / UV tables for the new size.
	sub_54660_read_and_decompress_sky_and_blocks(D41A0_0.terrain_2FECE.MapType, x_BYTE_D41B5_texture_size);
	PrepareTerrainTextureAddresses();

	if (message)
		*message = enable ? "High-res terrain textures ON" : "High-res terrain textures OFF";
	return true;
}
