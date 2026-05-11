#pragma once

// Engine wrapper for Dear ImGui.
//
// Include this header instead of <imgui.h> in all application/layer code.
// When AETHER_IMGUI is defined the real library is forwarded; otherwise a set
// of minimal no-op stubs is provided so call sites compile cleanly to nothing
// without any #ifdef guards at the use site.
//
// Low-level ImGui internals (ImDrawList, ImDrawVert, etc.) are NOT stubbed -
// those are only used inside ImGuiRenderer which is itself fully guarded.

#ifdef AETHER_IMGUI

#	include <imgui.h>

#else // ── No-op stubs ────────────────────────────────────────────────────────

struct ImVec2
{
	float x = 0.f, y = 0.f;
	constexpr ImVec2() = default;

	constexpr ImVec2(float x_, float y_)
	      : x(x_), y(y_)
	{
	}
};

struct ImVec4
{
	float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
	constexpr ImVec4() = default;

	constexpr ImVec4(float x_, float y_, float z_, float w_)
	      : x(x_), y(y_), z(z_), w(w_)
	{
	}
};

struct ImGuiIO
{
	ImVec2 DisplaySize;
};

using ImGuiWindowFlags = int;
using ImGuiCond = int;

constexpr ImGuiWindowFlags ImGuiWindowFlags_AlwaysAutoResize = 0;
constexpr ImGuiWindowFlags ImGuiWindowFlags_NoCollapse = 0;
constexpr ImGuiCond ImGuiCond_FirstUseEver = 0;

namespace ImGui
{
	inline ImGuiIO& GetIO()
	{
		static ImGuiIO s;
		return s;
	}

	// Begin always returns false so the standard "if (!Begin) { End; return; }" pattern exits early.
	inline bool Begin(const char*, bool* = nullptr, ImGuiWindowFlags = 0)
	{
		return false;
	}

	inline void End()
	{
	}

	inline void SetNextWindowPos(ImVec2, ImGuiCond = 0, ImVec2 = {})
	{
	}

	inline void SetNextWindowSize(ImVec2, ImGuiCond = 0)
	{
	}

	inline void SeparatorText(const char*)
	{
	}

	inline void Columns(int, const char* = nullptr, bool = true)
	{
	}

	inline void NextColumn()
	{
	}

	inline void TextUnformatted(const char*, const char* = nullptr)
	{
	}

	inline void Text(const char*, ...)
	{
	} // NOLINT

	inline void TextColored(ImVec4, const char*, ...)
	{
	} // NOLINT

	inline void TextDisabled(const char*, ...)
	{
	} // NOLINT

	inline void Spacing()
	{
	}

	inline void SameLine(float = 0.f, float = -1.f)
	{
	}

	inline ImVec2 GetContentRegionAvail()
	{
		return {};
	}

	inline void PlotHistogram(const char*, const float*, int, int = 0, const char* = nullptr, float = 3.402823466e+38f, float = 3.402823466e+38f, ImVec2 = {})
	{
	}
} // namespace ImGui

#endif // AETHER_IMGUI
