#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"

namespace aether::editor
{
	enum class TileTool : std::uint8_t
	{
		None = 0,
		Pencil,
		Rectangle,
		Fill,
		Erase,
		Picker,
	};

	struct TilePaintEdit
	{
		std::size_t layer = 0;
		glm::ivec2 cell{0};
		std::uint32_t before = 0;
		std::uint32_t after = 0;
	};

	// One user gesture (drag, rect commit, fill). Undo/redo replays these
	// chunk-local diffs through TileMapAsset::SetCell - never map snapshots.
	struct TilePaintStroke
	{
		std::string tilemapPath;
		std::vector<TilePaintEdit> edits;
	};

	// Shared between the Tile Palette panel (tool/tile selection) and the viewport
	// (painting). Registered as a service by DebugLayer. Paint gestures are recorded
	// as TileStrokeCommands on the main editor UndoStack, so tile and entity edits
	// share one Ctrl+Z history.
	struct TilePaintingState
	{
		TileTool tool = TileTool::None;
		AssetObjectId selectedTile{};
		bool flipX = false;
		bool flipY = false;
		std::size_t activeLayer = 0;

		bool mapDirty = false; // unsaved tilemap edits
	};
} // namespace aether::editor
