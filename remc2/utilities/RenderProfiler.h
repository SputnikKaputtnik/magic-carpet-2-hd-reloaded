#pragma once

#include <chrono>

enum class RenderProfileStage
{
	World,
	Presentation,
	TerrainTotal,
	TerrainNormal,
	TerrainInverse,
	TerrainCave,
	Sky,
	Count
};

class ScopedRenderProfile
{
public:
	explicit ScopedRenderProfile(RenderProfileStage stage);
	~ScopedRenderProfile();

	ScopedRenderProfile(const ScopedRenderProfile&) = delete;
	ScopedRenderProfile& operator=(const ScopedRenderProfile&) = delete;

private:
	RenderProfileStage m_stage;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_start;
};
