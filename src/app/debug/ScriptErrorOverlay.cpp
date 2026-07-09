#include "debug/ScriptErrorOverlay.hpp"

#include <cctype>
#include <regex>
#include <vector>

#include <imgui.h>

#include "debug/OpenInEditor.hpp"
#include "layers/AppLayer.hpp"
#include "Color.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"

namespace aether::app
{
	void ScriptErrorOverlay::ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine)
	{
		outPath.clear();
		outLine = 0;

		// .NET exception stack traces read "... in <path>\Script.cs:line 42".
		// Anchor on the ".cs:" marker, then take the preceding path and the line
		// number after an optional "line " token so the toast can offer to open
		// the offending script.
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
			while (lineEnd < error.size() && std::isdigit(static_cast<unsigned char>(error[lineEnd])))
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

	void ScriptErrorOverlay::Poll(LayerContext& context)
	{
		auto scripting = context.TryGet<scripting::CSharpScriptingSubsystem>();
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
			static const std::regex errorPattern(R"(error\[\d+\]:)");
			auto begin = std::sregex_iterator(err.begin(), err.end(), errorPattern);
			auto end = std::sregex_iterator();

			if (begin == end)
			{
				individualErrors.push_back(err);
			}
			else
			{
				std::size_t lastPos = 0;
				for (auto it = begin; it != end; ++it)
				{
					const std::smatch& match = *it;
					if (it == begin)
					{
						lastPos = match.position();
					}
					else
					{
						individualErrors.push_back(err.substr(lastPos, match.position() - lastPos));
						lastPos = match.position();
					}
				}
				individualErrors.push_back(err.substr(lastPos));
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
					std::string line = singleErr.substr(pos, lineEnd - pos);

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
} // namespace aether::app
