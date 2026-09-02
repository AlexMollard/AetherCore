#include "imgui/UiAutomationMethods.hpp"

#include <optional>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include "imgui/UiAutomation.hpp"

namespace aether::editor
{
	namespace
	{
		using nlohmann::json;

		json Obj(json properties = json::object(), const std::vector<std::string>& required = {})
		{
			json schema{{"type", "object"}, {"properties", std::move(properties)}};
			if (!required.empty())
			{
				schema["required"] = required;
			}
			return schema;
		}

		json StrProp() { return json{{"type", "string"}}; }
		json NumProp() { return json{{"type", "number"}}; }
		json BoolProp() { return json{{"type", "boolean"}}; }

		// Resolve a target to a screen-space centre: explicit {x,y}, else {window,label}.
		bool ResolveTarget(const json& p, float& cx, float& cy, std::string& err)
		{
			if (p.contains("x") && p.contains("y"))
			{
				cx = p.value("x", 0.0f);
				cy = p.value("y", 0.0f);
				return true;
			}
			const std::string label = p.value("label", std::string{});
			if (label.empty())
			{
				err = "provide either {x,y} or {window,label}";
				return false;
			}
			const std::string window = p.value("window", std::string{});
			const auto item = app::UiAutomation::Get().FindItem(window, label, err);
			if (!item.has_value())
			{
				return false;
			}
			cx = item->x + item->w * 0.5f;
			cy = item->y + item->h * 0.5f;
			return true;
		}

		int KeyFromName(const std::string& name)
		{
			static const std::unordered_map<std::string, int> kMap{
			        {"enter", ImGuiKey_Enter}, {"escape", ImGuiKey_Escape}, {"tab", ImGuiKey_Tab}, {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete}, {"space", ImGuiKey_Space}, {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow}, {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"home", ImGuiKey_Home}, {"end", ImGuiKey_End}};
			const auto it = kMap.find(name);
			if (it != kMap.end())
			{
				return it->second;
			}
			// Function keys, which several editor shortcuts use (F2 to rename, F5 to play) and
			// which no test could reach while the table held only the named editing keys.
			if (name.size() >= 2 && (name[0] == 'f' || name[0] == 'F'))
			{
				const std::string digits = name.substr(1);
				if (std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
				{
					const int index = std::atoi(digits.c_str());
					if (index >= 1 && index <= 12)
					{
						return ImGuiKey_F1 + (index - 1);
					}
				}
			}

			// Single letters and digits, so an editor shortcut can be named the way it is
			// written down: ctrl+z, ctrl+shift+s, and so on.
			if (name.size() == 1)
			{
				const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
				if (c >= 'a' && c <= 'z')
				{
					return ImGuiKey_A + (c - 'a');
				}
				if (c >= '0' && c <= '9')
				{
					return ImGuiKey_0 + (c - '0');
				}
			}
			return 0;
		}
	} // namespace

	void AppendUiAutomationMethods(std::vector<ControlMethod>& methods)
	{
		methods.push_back({"ui.query",
		        "ui_query",
		        "List on-screen ImGui widgets from the last frame: window, label, screen rect, hovered/active state, and whether the widget was clipped by its window (part of it, usually the end of a label, is not visible). Optional 'window'/'label' substring filters. Use this to navigate the UI before ui_click.",
		        false,
		        Obj({{"window", StrProp()}, {"label", StrProp()}}),
		        [](const json& params, MethodContext&) -> json
		        {
			        const std::string winFilter = params.value("window", std::string{});
			        const std::string labelFilter = params.value("label", std::string{});
			        const ImGuiContext* g = ImGui::GetCurrentContext();
			        json items = json::array();
			        for (const app::UiItem& item: app::UiAutomation::Get().Snapshot())
			        {
				        if (!winFilter.empty() && item.window.find(winFilter) == std::string::npos)
				        {
					        continue;
				        }
				        if (!labelFilter.empty() && item.label.find(labelFilter) == std::string::npos)
				        {
					        continue;
				        }
				        items.push_back(json{{"window", item.window}, {"label", item.label}, {"x", item.x}, {"y", item.y}, {"w", item.w}, {"h", item.h}, {"hovered", g != nullptr && g->HoveredId == item.id}, {"active", g != nullptr && g->ActiveId == item.id}, {"clipped", item.clipped}});
			        }
			        return json{{"items", std::move(items)}};
		        }});

		methods.push_back({"ui.click",
		        "ui_click",
		        "Click a widget by {window,label} or a raw {x,y} screen point. Optional 'button' ('left'|'right'), 'double', and 'ctrl'/'shift'/'alt' held for the click (multi-select and range-select). Async: the click plays over a few frames; query/screenshot afterwards to see the effect.",
		        true,
		        Obj({{"x", NumProp()}, {"y", NumProp()}, {"window", StrProp()}, {"label", StrProp()}, {"button", StrProp()}, {"double", BoolProp()},
		                {"ctrl", BoolProp()}, {"shift", BoolProp()}, {"alt", BoolProp()}}),
		        [](const json& params, MethodContext&) -> json
		        {
			        float cx = 0.0f;
			        float cy = 0.0f;
			        std::string err;
			        if (!ResolveTarget(params, cx, cy, err))
			        {
				        return json{{"error", err}};
			        }
			        const int button = params.value("button", std::string{"left"}) == "right" ? 1 : 0;
			        app::UiAutomation::Get().Input().QueueClick(cx, cy, button, params.value("double", false), params.value("ctrl", false), params.value("shift", false), params.value("alt", false));
			        return json{{"status", "queued"}, {"target", json{{"x", cx}, {"y", cy}}}};
		        }});

		methods.push_back({"ui.drag",
		        "ui_drag",
		        "Press at one widget or point and release at another: {from_window,from_label} or {from_x,from_y} to {to_window,to_label} or {to_x,to_y}. This is how a node-graph link or any drag-and-drop is made. hold_frames keeps the button down at the destination longer, for dwell-triggered behaviour. Async: query or screenshot afterwards.",
		        true,
		        Obj({{"from_x", NumProp()}, {"from_y", NumProp()}, {"from_window", StrProp()}, {"from_label", StrProp()},
		                {"to_x", NumProp()}, {"to_y", NumProp()}, {"to_window", StrProp()}, {"to_label", StrProp()}, {"button", StrProp()},
		                {"hold_frames", NumProp()}}),
		        [](const json& params, MethodContext&) -> json
		        {
			        // Both ends go through the same resolver as ui_click, by renaming the
			        // prefixed keys onto the ones it expects.
			        const auto endpoint = [&](const char* prefix, float& x, float& y, std::string& err)
			        {
				        json one;
				        const std::string p(prefix);
				        if (params.contains(p + "x")) { one["x"] = params[p + "x"]; }
				        if (params.contains(p + "y")) { one["y"] = params[p + "y"]; }
				        if (params.contains(p + "window")) { one["window"] = params[p + "window"]; }
				        if (params.contains(p + "label")) { one["label"] = params[p + "label"]; }
				        return ResolveTarget(one, x, y, err);
			        };

			        float fromX = 0.0f;
			        float fromY = 0.0f;
			        float toX = 0.0f;
			        float toY = 0.0f;
			        std::string err;
			        if (!endpoint("from_", fromX, fromY, err))
			        {
				        return json{{"error", "drag start: " + err}};
			        }
			        if (!endpoint("to_", toX, toY, err))
			        {
				        return json{{"error", "drag end: " + err}};
			        }
			        const int button = params.value("button", std::string{"left"}) == "right" ? 1 : 0;
			        const int holdFrames = params.value("hold_frames", 0);
			        app::UiAutomation::Get().Input().QueueDrag(fromX, fromY, toX, toY, button, holdFrames);
			        return json{{"status", "queued"}, {"from", json{{"x", fromX}, {"y", fromY}}}, {"to", json{{"x", toX}, {"y", toY}}}};
		        }});

		methods.push_back({"ui.hover",
		        "ui_hover",
		        "Hold the mouse over a widget ({window,label}) or {x,y}. Reveals hover-only UI (tooltips, hover buttons) and sets up a subsequent ui_click on a context-menu item. Held until the next action.",
		        true,
		        Obj({{"x", NumProp()}, {"y", NumProp()}, {"window", StrProp()}, {"label", StrProp()}}),
		        [](const json& params, MethodContext&) -> json
		        {
			        float cx = 0.0f;
			        float cy = 0.0f;
			        std::string err;
			        if (!ResolveTarget(params, cx, cy, err))
			        {
				        return json{{"error", err}};
			        }
			        app::UiAutomation::Get().Input().QueueHover(cx, cy);
			        return json{{"status", "queued"}, {"target", json{{"x", cx}, {"y", cy}}}};
		        }});

		methods.push_back({"ui.input_text",
		        "ui_input_text",
		        "Focus a text field ({window,label} or {x,y}) and replace its contents with 'text' (select-all, delete, then type).",
		        true,
		        Obj({{"x", NumProp()}, {"y", NumProp()}, {"window", StrProp()}, {"label", StrProp()}, {"text", StrProp()}}, {"text"}),
		        [](const json& params, MethodContext&) -> json
		        {
			        float cx = 0.0f;
			        float cy = 0.0f;
			        std::string err;
			        if (!ResolveTarget(params, cx, cy, err))
			        {
				        return json{{"error", err}};
			        }
			        app::UiAutomation& a = app::UiAutomation::Get();
			        a.Input().QueueClick(cx, cy, 0, false);
			        a.Input().QueueText(params.value("text", std::string{}));
			        return json{{"status", "queued"}};
		        }});

		methods.push_back({"ui.key",
		        "ui_key",
		        "Press a key, optionally with modifiers: enter, escape, tab, backspace, delete, space, left, right, up, down, home, end, or any single letter or digit. Set ctrl/shift/alt to send an editor shortcut such as ctrl+z.",
		        true,
		        Obj({{"key", StrProp()}, {"ctrl", json{{"type", "boolean"}}}, {"shift", json{{"type", "boolean"}}}, {"alt", json{{"type", "boolean"}}}}, {"key"}),
		        [](const json& params, MethodContext&) -> json
		        {
			        const std::string name = params.value("key", std::string{});
			        const int key = KeyFromName(name);
			        if (key == 0)
			        {
				        return json{{"error", "unknown key '" + name + "'"}};
			        }
			        const bool ctrl = params.value("ctrl", false);
			        const bool shift = params.value("shift", false);
			        const bool alt = params.value("alt", false);
			        app::UiAutomation::Get().Input().QueueKeyChord(key, ctrl, shift, alt);
			        return json{{"status", "queued"}, {"key", name}, {"ctrl", ctrl}, {"shift", shift}, {"alt", alt}};
		        }});
	}
} // namespace aether::editor
