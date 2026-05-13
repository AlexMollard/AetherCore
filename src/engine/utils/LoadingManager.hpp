#pragma once

#include <deque>
#include <functional>
#include <string>

namespace aether
{

	class LoadingManager
	{
	public:
		using Task = std::function<void()>;

		void AddTask(Task task, std::string_view name);

		// Process one pending task. Returns true if more tasks remain.
		bool Update();

		[[nodiscard]] float GetProgress() const
		{
			return m_total == 0 ? 1.0f : static_cast<float>(m_completed) / static_cast<float>(m_total);
		}

		[[nodiscard]] std::string_view GetCurrentTask() const
		{
			return m_currentTask;
		}

		[[nodiscard]] bool IsComplete() const
		{
			return m_tasks.empty();
		}

	private:
		struct TaskItem
		{
			Task fn;
			std::string name;
		};

		std::deque<TaskItem> m_tasks;
		std::size_t m_total = 0;
		std::size_t m_completed = 0;
		std::string m_currentTask;
	};

} // namespace aether
