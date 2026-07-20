#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aether::editor
{
	// The pixel-art canvas model, shared as a service between the PixelArtPanel
	// (interactive editing) and the control server (scripted editing). Pixels are
	// stored one std::uint32_t per texel in RGBA memory order (byte 0 = R ... byte
	// 3 = A), which is both ImGui's default IM_COL32 layout and stb's PNG layout,
	// so drawing and disk I/O need no conversion. Single layer.
	class PixelArtDocument
	{
	public:
		static constexpr int kMaxSize = 512;

		PixelArtDocument();

		[[nodiscard]] int Width() const noexcept
		{
			return m_width;
		}
		[[nodiscard]] int Height() const noexcept
		{
			return m_height;
		}
		[[nodiscard]] const std::vector<std::uint32_t>& Pixels() const noexcept
		{
			return m_pixels;
		}
		[[nodiscard]] std::uint32_t GetPixel(int x, int y) const noexcept;
		[[nodiscard]] bool InBounds(int x, int y) const noexcept
		{
			return x >= 0 && y >= 0 && x < m_width && y < m_height;
		}

		// Fresh transparent canvas of the given size (clamped to [1, kMaxSize]).
		// Clears undo history and the current file path.
		void New(int width, int height);

		// Snapshot the current pixels so the next batch of edits can be undone as
		// one unit. Call once at the start of a stroke / before a scripted op.
		void Snapshot();
		void Undo();
		void Redo();
		[[nodiscard]] bool CanUndo() const noexcept
		{
			return !m_undo.empty();
		}
		[[nodiscard]] bool CanRedo() const noexcept
		{
			return !m_redo.empty();
		}

		// Direct edit primitives (no implicit undo - wrap a group in Snapshot()).
		void SetPixel(int x, int y, std::uint32_t rgba) noexcept;
		void Clear(std::uint32_t rgba = 0);
		void FloodFill(int x, int y, std::uint32_t rgba);
		void DrawLine(int x0, int y0, int x1, int y1, std::uint32_t rgba);
		void DrawRect(int x0, int y0, int x1, int y1, std::uint32_t rgba, bool filled);
		void DrawEllipse(int x0, int y0, int x1, int y1, std::uint32_t rgba, bool filled);

		// Disk I/O (PNG, RGBA). Load also adopts the path for a subsequent Save().
		[[nodiscard]] bool Load(const std::filesystem::path& path);
		[[nodiscard]] bool Save(const std::filesystem::path& path);
		[[nodiscard]] const std::filesystem::path& Path() const noexcept
		{
			return m_path;
		}

		[[nodiscard]] std::uint32_t Color() const noexcept
		{
			return m_color;
		}
		void SetColor(std::uint32_t rgba) noexcept
		{
			m_color = rgba;
		}
		[[nodiscard]] std::vector<std::uint32_t>& Palette() noexcept
		{
			return m_palette;
		}

		[[nodiscard]] bool Dirty() const noexcept
		{
			return m_dirty;
		}

	private:
		void PushUndo();

		int m_width = 32;
		int m_height = 32;
		std::vector<std::uint32_t> m_pixels;
		std::uint32_t m_color = 0xFF000000u; // opaque black (A=0xFF, RGB=0)
		std::vector<std::uint32_t> m_palette;
		std::vector<std::vector<std::uint32_t>> m_undo;
		std::vector<std::vector<std::uint32_t>> m_redo;
		std::filesystem::path m_path;
		bool m_dirty = false;
	};
} // namespace aether::editor
