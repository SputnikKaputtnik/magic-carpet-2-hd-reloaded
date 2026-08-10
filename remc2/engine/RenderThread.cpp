#include "RenderThread.h"
#include <mutex>
#include <utility>

RenderThread::RenderThread()
{
	m_core = 0;
	m_running = false;
	m_isTaskRunning = false;
	m_task = 0;
	StartWorkerThread();
}

RenderThread::RenderThread(uint8_t core)
{
	m_core = 0;
	m_running = false;
	m_isTaskRunning = false;
	m_task = 0;
	StartWorkerThread(core);
}

RenderThread::~RenderThread()
{
#ifndef __ANDROID__
	try
#endif
	{
		StopWorkerThread();
	}
#ifndef __ANDROID__
	catch (const std::exception& e)
	{
		Logger->error("Error Stopping Worker Thread: {}", e.what());
	}
#endif
}

void RenderThread::StartWorkerThread(int8_t core)
{
	Logger->debug("Starting Worker Thread: {}", core);
	int8_t numCores = std::thread::hardware_concurrency();

	if (numCores < core)
		return;

	m_running = true;
	m_core = core;
	m_renderThread = std::thread([this, core] {
		constexpr int taskSpinCount = 4096;

#ifdef _MSC_VER
		if (core >= 0) {
			SetThreadIdealProcessor(GetCurrentThread(), core);
			DWORD_PTR dw = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << ((uint8_t)core));
		}
#endif

		while (m_running)
		{
			for (int spin = 0;
				spin < taskSpinCount && !m_isTaskRunning && m_running;
				++spin)
			{
#ifdef _MSC_VER
				YieldProcessor();
#else
				if ((spin & 63) == 0)
					std::this_thread::yield();
#endif
			}

			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(m_taskMutex);
				m_nextTaskCondition.wait(lock, [this] {
					return m_isTaskRunning || !m_running;
				});

				if (!m_running)
					break;

				task = std::move(m_task);
			}

			if (task)
				task();
			m_isTaskRunning = false;
		}
	});
}

void RenderThread::StopWorkerThread()
{
	if (m_running)
	{
		Logger->debug("Stoping Worker Thread");
		m_running = false;
		m_nextTaskCondition.notify_all();
		if (m_renderThread.joinable()) {
			m_renderThread.join();
		}
	}
}

void RenderThread::Run(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> guard(m_taskMutex);
		m_isTaskRunning = true;
		m_task = std::move(task);
	}
	m_nextTaskCondition.notify_all();
}

bool RenderThread::IsRunning()
{
	return m_running;
}

bool RenderThread::GetIsTaskRunning()
{
	return m_isTaskRunning;
}
