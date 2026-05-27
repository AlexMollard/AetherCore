#include "utils/LoadingManager.hpp"
#include "utils/Profiler.hpp"

namespace aether
{

	void LoadingManager::AddTask(Task task, std::string_view name)
	{
		AE_PROFILE_ZONE();
		m_tasks.push_back({ std::move(task), std::string(name) });
		++m_total;
	}

	bool LoadingManager::Update()
	{
		AE_PROFILE_ZONE();
		if (m_tasks.empty())
		{
			return false;
		}

		auto& task = m_tasks.front();
		m_currentTask = task.name;
		task.fn();
		m_tasks.pop_front();
		++m_completed;

		// Intentionally keep m_currentTask set to the last completed task
		// so the loading overlay shows meaningful text between frames.

		return !m_tasks.empty();
	}

} // namespace aether
