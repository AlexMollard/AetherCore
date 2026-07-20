#include "debug/PixelArtDocument.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <utility>

#include "io/ImageIO.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::uint32_t Pack(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
		{
			return static_cast<std::uint32_t>(a) << 24 | static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(r);
		}

		constexpr std::size_t kMaxUndo = 64;
	} // namespace

	PixelArtDocument::PixelArtDocument()
	{
		// A small default palette so the panel is usable immediately.
		m_palette = {
		        Pack(0, 0, 0, 255), Pack(255, 255, 255, 255), Pack(228, 59, 68, 255), Pack(247, 118, 34, 255), Pack(255, 231, 98, 255), Pack(99, 199, 77, 255), Pack(38, 152, 220, 255), Pack(104, 56, 108, 255),
		        Pack(255, 160, 200, 255), Pack(140, 100, 60, 255), Pack(120, 120, 120, 255), Pack(0, 0, 0, 0)};
		New(32, 32);
	}

	std::uint32_t PixelArtDocument::GetPixel(int x, int y) const noexcept
	{
		return InBounds(x, y) ? m_pixels[static_cast<std::size_t>(y) * m_width + x] : 0u;
	}

	void PixelArtDocument::New(int width, int height)
	{
		m_width = std::clamp(width, 1, kMaxSize);
		m_height = std::clamp(height, 1, kMaxSize);
		m_pixels.assign(static_cast<std::size_t>(m_width) * m_height, 0u);
		m_undo.clear();
		m_redo.clear();
		m_path.clear();
		m_dirty = false;
	}

	void PixelArtDocument::PushUndo()
	{
		m_undo.push_back(m_pixels);
		if (m_undo.size() > kMaxUndo)
		{
			m_undo.erase(m_undo.begin());
		}
	}

	void PixelArtDocument::Snapshot()
	{
		PushUndo();
		m_redo.clear();
	}

	void PixelArtDocument::Undo()
	{
		if (m_undo.empty())
		{
			return;
		}
		m_redo.push_back(m_pixels);
		m_pixels = std::move(m_undo.back());
		m_undo.pop_back();
		m_dirty = true;
	}

	void PixelArtDocument::Redo()
	{
		if (m_redo.empty())
		{
			return;
		}
		m_undo.push_back(m_pixels);
		m_pixels = std::move(m_redo.back());
		m_redo.pop_back();
		m_dirty = true;
	}

	void PixelArtDocument::SetPixel(int x, int y, std::uint32_t rgba) noexcept
	{
		if (InBounds(x, y))
		{
			m_pixels[static_cast<std::size_t>(y) * m_width + x] = rgba;
			m_dirty = true;
		}
	}

	void PixelArtDocument::Clear(std::uint32_t rgba)
	{
		std::fill(m_pixels.begin(), m_pixels.end(), rgba);
		m_dirty = true;
	}

	void PixelArtDocument::FloodFill(int x, int y, std::uint32_t rgba)
	{
		if (!InBounds(x, y))
		{
			return;
		}
		const std::uint32_t target = GetPixel(x, y);
		if (target == rgba)
		{
			return;
		}
		std::queue<std::pair<int, int>> open;
		open.emplace(x, y);
		while (!open.empty())
		{
			const auto [cx, cy] = open.front();
			open.pop();
			if (!InBounds(cx, cy) || m_pixels[static_cast<std::size_t>(cy) * m_width + cx] != target)
			{
				continue;
			}
			m_pixels[static_cast<std::size_t>(cy) * m_width + cx] = rgba;
			open.emplace(cx + 1, cy);
			open.emplace(cx - 1, cy);
			open.emplace(cx, cy + 1);
			open.emplace(cx, cy - 1);
		}
		m_dirty = true;
	}

	void PixelArtDocument::DrawLine(int x0, int y0, int x1, int y1, std::uint32_t rgba)
	{
		// Integer Bresenham.
		int dx = std::abs(x1 - x0);
		int dy = -std::abs(y1 - y0);
		int sx = x0 < x1 ? 1 : -1;
		int sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (;;)
		{
			SetPixel(x0, y0, rgba);
			if (x0 == x1 && y0 == y1)
			{
				break;
			}
			const int e2 = 2 * err;
			if (e2 >= dy)
			{
				err += dy;
				x0 += sx;
			}
			if (e2 <= dx)
			{
				err += dx;
				y0 += sy;
			}
		}
	}

	void PixelArtDocument::DrawRect(int x0, int y0, int x1, int y1, std::uint32_t rgba, bool filled)
	{
		const int lx = std::min(x0, x1);
		const int hx = std::max(x0, x1);
		const int ly = std::min(y0, y1);
		const int hy = std::max(y0, y1);
		if (filled)
		{
			for (int y = ly; y <= hy; ++y)
			{
				for (int x = lx; x <= hx; ++x)
				{
					SetPixel(x, y, rgba);
				}
			}
			return;
		}
		for (int x = lx; x <= hx; ++x)
		{
			SetPixel(x, ly, rgba);
			SetPixel(x, hy, rgba);
		}
		for (int y = ly; y <= hy; ++y)
		{
			SetPixel(lx, y, rgba);
			SetPixel(hx, y, rgba);
		}
	}

	void PixelArtDocument::DrawEllipse(int x0, int y0, int x1, int y1, std::uint32_t rgba, bool filled)
	{
		const int lx = std::min(x0, x1);
		const int hx = std::max(x0, x1);
		const int ly = std::min(y0, y1);
		const int hy = std::max(y0, y1);
		const double cx = (lx + hx) * 0.5;
		const double cy = (ly + hy) * 0.5;
		const double rx = std::max((hx - lx) * 0.5, 0.5);
		const double ry = std::max((hy - ly) * 0.5, 0.5);
		for (int y = ly; y <= hy; ++y)
		{
			for (int x = lx; x <= hx; ++x)
			{
				const double nx = (x - cx) / rx;
				const double ny = (y - cy) / ry;
				const double d = nx * nx + ny * ny;
				if (filled)
				{
					if (d <= 1.0)
					{
						SetPixel(x, y, rgba);
					}
				}
				else if (d <= 1.0 && d > 0.55) // hollow ring approximation
				{
					SetPixel(x, y, rgba);
				}
			}
		}
	}

	bool PixelArtDocument::Load(const std::filesystem::path& path)
	{
		std::vector<std::uint8_t> bytes;
		int w = 0;
		int h = 0;
		if (!io::LoadPngRgba(path, bytes, w, h) || w > kMaxSize || h > kMaxSize)
		{
			return false;
		}
		m_width = w;
		m_height = h;
		m_pixels.resize(static_cast<std::size_t>(w) * h);
		std::memcpy(m_pixels.data(), bytes.data(), m_pixels.size() * sizeof(std::uint32_t));
		m_undo.clear();
		m_redo.clear();
		m_path = path;
		m_dirty = false;
		return true;
	}

	bool PixelArtDocument::Save(const std::filesystem::path& path)
	{
		if (!io::WritePngRgba(path, reinterpret_cast<const std::uint8_t*>(m_pixels.data()), m_width, m_height))
		{
			return false;
		}
		m_path = path;
		m_dirty = false;
		return true;
	}
} // namespace aether::editor
