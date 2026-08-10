#include "RenderProfiler.h"

#include "../engine/CommandLineParser.h"
#include "../portability/port_filesystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

namespace
{
	constexpr uint64_t SamplesPerReport = 120;

	struct StageStatistics
	{
		uint64_t sampleCount = 0;
		uint64_t totalMicroseconds = 0;
		uint64_t minimumMicroseconds = std::numeric_limits<uint64_t>::max();
		uint64_t maximumMicroseconds = 0;
	};

	std::array<StageStatistics, static_cast<size_t>(RenderProfileStage::Count)> Statistics;
	std::mutex StatisticsMutex;

	const char* StageName(RenderProfileStage stage)
	{
		switch (stage)
		{
		case RenderProfileStage::World:
			return "world";
		case RenderProfileStage::Presentation:
			return "presentation";
		case RenderProfileStage::TerrainTotal:
			return "terrain-total";
		case RenderProfileStage::TerrainNormal:
			return "terrain-normal";
		case RenderProfileStage::TerrainInverse:
			return "terrain-inverse";
		case RenderProfileStage::TerrainCave:
			return "terrain-cave";
		case RenderProfileStage::Sky:
			return "sky";
		default:
			return "unknown";
		}
	}

	void Record(RenderProfileStage stage, uint64_t microseconds)
	{
		std::lock_guard<std::mutex> lock(StatisticsMutex);
		auto& statistics = Statistics[static_cast<size_t>(stage)];
		++statistics.sampleCount;
		statistics.totalMicroseconds += microseconds;
		statistics.minimumMicroseconds =
			std::min(statistics.minimumMicroseconds, microseconds);
		statistics.maximumMicroseconds =
			std::max(statistics.maximumMicroseconds, microseconds);

		if (statistics.sampleCount < SamplesPerReport)
			return;

		const double averageMilliseconds =
			static_cast<double>(statistics.totalMicroseconds) /
			static_cast<double>(statistics.sampleCount) / 1000.0;
		Logger->info(
			"Renderer profile [{}]: avg={:.3f} ms, min={:.3f} ms, max={:.3f} ms, samples={}",
			StageName(stage),
			averageMilliseconds,
			static_cast<double>(statistics.minimumMicroseconds) / 1000.0,
			static_cast<double>(statistics.maximumMicroseconds) / 1000.0,
			statistics.sampleCount);
		Logger->flush();
		statistics = StageStatistics{};
	}
}

ScopedRenderProfile::ScopedRenderProfile(RenderProfileStage stage) :
	m_stage(stage),
	m_enabled(CommandLineParams.DoProfileRenderer())
{
	if (m_enabled)
		m_start = std::chrono::steady_clock::now();
}

ScopedRenderProfile::~ScopedRenderProfile()
{
	if (!m_enabled)
		return;

	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - m_start);
	Record(m_stage, static_cast<uint64_t>(elapsed.count()));
}
