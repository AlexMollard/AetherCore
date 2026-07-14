#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scene/SceneSerializer.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::editor
{
	class UndoStack
	{
	public:
		static constexpr std::size_t kMaxDepth = 32;

		void Push(World& world, ServiceContainer& services);

		bool Undo(World& world, ServiceContainer& services);
		bool Redo(World& world, ServiceContainer& services);

		[[nodiscard]] std::size_t UndoDepth() const
		{
			return m_undo.size();
		}

		[[nodiscard]] std::size_t RedoDepth() const
		{
			return m_redo.size();
		}

	private:
		struct Entry
		{
			app::scene::SceneDescription desc;
			std::string key;
		};

		bool Capture(World& world, ServiceContainer& services, Entry& out);

		std::vector<Entry> m_undo;
		std::vector<Entry> m_redo;
	};
} // namespace aether::editor
