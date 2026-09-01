#include "debug/ScriptErrorOverlay.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <vector>

#include "debug/EditorChrome.hpp"

#include <imgui.h>

#include "debug/OpenInEditor.hpp"
#include "layers/AppLayer.hpp"
#include "Color.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"

namespace aether::editor
{
	void ParseScriptErrorLocation(const std::string& error, std::string& outPath, int& outLine)
	{
		outPath.clear();
		outLine = 0;

		// The C# toolchain writes "Foo.cs(10,17):"; the older runtime wrote "Foo.cs:10".
		// Only the second was handled, so a build error never offered its Open button.
		if (const auto paren = error.find(".cs("); paren != std::string::npos)
		{
			std::size_t start = paren;
			while (start > 0 && error[start - 1] != ' ' && error[start - 1] != '\n' && error[start - 1] != '\r')
			{
				--start;
			}
			outPath = error.substr(start, paren + 3 - start);
			int parsed = 0;
			for (std::size_t i = paren + 4; i < error.size() && error[i] >= '0' && error[i] <= '9'; ++i)
			{
				parsed = parsed * 10 + (error[i] - '0');
			}
			outLine = parsed;
			return;
		}

		std::size_t searchPos = 0;
		while (searchPos < error.size())
		{
			const auto extPos = error.find(".cs:", searchPos);
			if (extPos == std::string::npos)
			{
				break;
			}

			std::size_t start = extPos;
			while (start > 0 && error[start - 1] != ' ' && error[start - 1] != '\n' && error[start - 1] != '\r')
			{
				--start;
			}
			outPath = error.substr(start, extPos + 3 - start);

			std::size_t lineStart = extPos + 4;
			if (error.compare(lineStart, 5, "line ") == 0)
			{
				lineStart += 5;
			}

			std::size_t lineEnd = lineStart;
			while (lineEnd < error.size() && (std::isdigit(static_cast<unsigned char>(error[lineEnd])) != 0))
			{
				++lineEnd;
			}

			if (lineEnd > lineStart)
			{
				try
				{
					outLine = std::stoi(error.substr(lineStart, lineEnd - lineStart));
				}
				catch (...)
				{
					outLine = 0;
				}
			}

			if (outLine > 0)
			{
				break;
			}

			outPath.clear();
			searchPos = extPos + 1;
		}
	}

	void ScriptErrorOverlay::Poll(app::LayerContext& context)
	{
		auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>();
		if (!scripting)
		{
			return;
		}

		if (scripting->ConsumeErrorsCleared())
		{
			m_toasts.clear();
		}

		auto errors = scripting->PollPendingErrors();
		if (!errors.empty())
		{
			m_toasts.clear();
		}

		for (auto& err: errors)
		{
			std::vector<std::string> individualErrors;
			// One toast per compiler error. The C# toolchain writes "error CS1525:"; the older
			// "error[1234]:" form is kept because the same overlay reports runtime script
			// faults, which still use it. Matching neither is what collapsed a whole build
			// into a single toast reading "Project script build failed:" and nothing else.
			static const std::regex errorPattern(R"(error (?:CS)?\d+:|error \[?[A-Z]*\d+\]?:)");
			auto begin = std::sregex_iterator(err.begin(), err.end(), errorPattern);
			auto end = std::sregex_iterator();

			if (begin == end)
			{
				individualErrors.push_back(err);
			}
			else
			{
				// Text before the first match is the wrapper line ("Project script build
				// failed:"), which says nothing on its own - the errors below it are the
				// message. Starting at the first match drops it.
				std::size_t lastPos = std::string::npos;
				for (auto it = begin; it != end; ++it)
				{
					const std::smatch& match = *it;
					std::size_t lineStart = err.rfind('\n', match.position());
					lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
					if (lastPos != std::string::npos && lineStart > lastPos)
					{
						individualErrors.push_back(err.substr(lastPos, lineStart - lastPos));
					}
					lastPos = lineStart;
				}
				if (lastPos != std::string::npos)
				{
					individualErrors.push_back(err.substr(lastPos));
				}
			}

			for (const auto& singleErr: individualErrors)
			{
				if (singleErr.empty())
				{
					continue;
				}

				Toast toast;
				toast.message = singleErr;

				std::size_t pos = 0;
				while (pos < singleErr.size())
				{
					auto lineEnd = singleErr.find('\n', pos);
					if (lineEnd == std::string::npos)
					{
						lineEnd = singleErr.size();
					}
					const std::string line = singleErr.substr(pos, lineEnd - pos);

					std::size_t first = line.find_first_not_of(" \t");
					if (first != std::string::npos && !line.empty())
					{
						toast.summary = line.substr(first);
						break;
					}
					pos = lineEnd + 1;
				}

				ParseScriptErrorLocation(singleErr, toast.filePath, toast.line);
				m_toasts.push_back(std::move(toast));
			}
		}
	}

	namespace
	{
		// "D:\...\Foo.cs(31,1): error CS1031: Type expected" -> "error CS1031: Type expected".
		// The card already says which file and line, so repeating the absolute path in the
		// message pushed the part that matters off the end of it.
		std::string MessageWithoutLocation(const std::string& summary)
		{
			if (const std::size_t paren = summary.find(".cs("); paren != std::string::npos)
			{
				if (const std::size_t close = summary.find("): ", paren); close != std::string::npos)
				{
					return summary.substr(close + 3);
				}
			}
			if (const std::size_t colon = summary.find(".cs:"); colon != std::string::npos)
			{
				if (const std::size_t sep = summary.find(": ", colon + 4); sep != std::string::npos)
				{
					return summary.substr(sep + 2);
				}
			}
			return summary;
		}
	} // namespace

	bool ScriptErrorOverlay::Draw()
	{
		if (m_toasts.empty())
		{
			return false;
		}

		// A card in the corner, sized to what it says. This used to be a full-width band
		// pinned across the bottom of the screen for a single line of text, sitting on top of
		// the Console - the one panel you would be reading to fix the error.
		//
		// The width is FIXED rather than auto-sized: wrapped text inside an auto-resizing
		// window is circular (the wrap width comes from the window, the window from the
		// wrapped text) and collapses to one character per line.
		constexpr float kMargin = 16.0f;
		constexpr float kCardWidth = 460.0f;
		constexpr std::size_t kMaxRows = 3;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const ImVec2 corner(viewport->WorkPos.x + viewport->WorkSize.x - kMargin, viewport->WorkPos.y + viewport->WorkSize.y - kMargin);
		ImGui::SetNextWindowPos(corner, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowSize(ImVec2(kCardWidth, 0.0f), ImGuiCond_Always);
		ImGui::Begin("Script Errors",
		        nullptr,
		        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

		char heading[64];
		std::snprintf(heading, sizeof(heading), "%zu SCRIPT ERROR%s", m_toasts.size(), m_toasts.size() == 1 ? "" : "S");
		chrome::SectionTag(heading);

		bool showAll = false;
		const std::size_t shown = std::min(m_toasts.size(), kMaxRows);
		std::size_t dismiss = m_toasts.size();
		std::string previousFile;
		for (std::size_t i = 0; i < shown; ++i)
		{
			const Toast& toast = m_toasts[i];
			ImGui::PushID(static_cast<int>(i));

			// Where it broke. One broken file produces a cascade, so the name is printed once
			// and the errors after it show only their line - the filename repeated down the
			// card said the same thing four times and crowded out the messages.
			if (!toast.filePath.empty())
			{
				const bool sameFile = toast.filePath == previousFile;
				const std::string name = std::filesystem::path(toast.filePath).filename().generic_string();
				const std::string where = sameFile ? "line " + std::to_string(toast.line) : name + ":" + std::to_string(toast.line);
				ImGui::TextColored(chrome::C(colors::Orange), "%s", where.c_str());
				ImGui::SetItemTooltip("%s", toast.filePath.c_str());
				previousFile = toast.filePath;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::C(colors::Error));
			ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
			ImGui::TextWrapped("%s", MessageWithoutLocation(toast.summary).c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();

			// Per-error actions. "Dismiss All" used to be the only one, so clearing a message
			// you had dealt with also threw away the ones you had not read.
			if (!toast.filePath.empty())
			{
				if (chrome::GhostButton("Open"))
				{
					OpenInEditor(toast.filePath, toast.line);
				}
				ImGui::SameLine();
			}
			if (chrome::GhostButton("Dismiss"))
			{
				dismiss = i;
			}
			ImGui::PopID();
			if (i + 1 < shown)
			{
				ImGui::Separator();
			}
		}

		// The rest are reachable rather than merely counted: this opens the Console filtered
		// to the script errors.
		if (m_toasts.size() > shown)
		{
			char more[64];
			std::snprintf(more, sizeof(more), "Show all %zu in the Console", m_toasts.size());
			if (chrome::GhostButton(more))
			{
				showAll = true;
			}
			ImGui::SameLine();
		}
		if (m_toasts.size() > 1)
		{
			if (chrome::GhostButton("Dismiss all"))
			{
				m_toasts.clear();
			}
		}
		ImGui::End();

		if (dismiss < m_toasts.size())
		{
			m_toasts.erase(m_toasts.begin() + static_cast<std::ptrdiff_t>(dismiss));
		}
		return showAll;
	}

	void ScriptErrorOverlay::Clear()
	{
		m_toasts.clear();
	}
} // namespace aether::editor
