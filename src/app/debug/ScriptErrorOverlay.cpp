#include "debug/ScriptErrorOverlay.hpp"

#include <cctype>
#include <regex>
#include <vector>

#include <imgui.h>

#include "debug/OpenInEditor.hpp"
#include "layers/AppLayer.hpp"
#include "Color.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"

namespace aether::editor
{
	void ScriptErrorOverlay::ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine)
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

				ParseErrorLocation(singleErr, toast.filePath, toast.line);
				m_toasts.push_back(std::move(toast));
			}
		}
	}

	void ScriptErrorOverlay::Draw()
	{
		if (m_toasts.empty())
		{
			return;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + viewport->WorkSize.y - 88.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - 32.0f, 68.0f), ImGuiCond_Always);
		ImGui::Begin("Script Errors", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		const Toast& toast = m_toasts.front();
		const auto& scriptErr = colors::Error;
		ImGui::TextColored(ImVec4(scriptErr.r, scriptErr.g, scriptErr.b, scriptErr.a), "Script Error%s", m_toasts.size() > 1 ? "s" : "");
		ImGui::SameLine();
		ImGui::TextUnformatted(toast.summary.c_str());
		if (!toast.filePath.empty())
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Open"))
			{
				OpenInEditor(toast.filePath, toast.line);
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Dismiss All"))
		{
			m_toasts.clear();
		}
		ImGui::End();
	}

	void ScriptErrorOverlay::Clear()
	{
		m_toasts.clear();
	}
} // namespace aether::editor
