#include "debug/SpriteAnimationPanel.hpp"

#include <algorithm>
#include <filesystem>
#include <format>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "assets/AssetDatabase.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	namespace
	{
		template<std::size_t N>
		void SetBuffer(std::array<char, N>& destination, std::string_view value)
		{
			destination.fill('\0');
			const std::size_t count = std::min(value.size(), N - 1);
			std::copy_n(value.data(), count, destination.data());
		}
	} // namespace

	void SpriteAnimationPanel::OnImGui(app::LayerContext& context)
	{
		if (!m_visible)
		{
			return;
		}
		if (!ImGui::Begin("Sprite Animation", &m_visible))
		{
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputText("Animation File", m_animationPath.data(), m_animationPath.size());
		ImGui::SameLine();
		if (ImGui::Button("Load"))
		{
			const auto loaded = SpriteAnimationAsset::Load(std::filesystem::path(m_animationPath.data()));
			if (loaded.has_value())
			{
				m_animation = *loaded;
				SetBuffer(m_atlasPath, m_animation.atlasPath);
				m_selectedFrame = m_animation.frames.empty() ? -1 : 0;
				m_previewFrame = 0;
				m_previewFrameTime = 0.0f;
				LoadAtlas(context);
				m_status = "Animation loaded.";
				m_statusError = false;
			}
			else
			{
				m_status = loaded.error().ToString();
				m_statusError = true;
			}
		}

		ImGui::InputText("Clip Name", &m_animation.name);
		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputText("Atlas", m_atlasPath.data(), m_atlasPath.size());
		ImGui::SameLine();
		if (ImGui::Button("Load Atlas"))
		{
			LoadAtlas(context);
		}
		int loopMode = static_cast<int>(m_animation.loopMode);
		const char* loopNames[] = {"Loop", "Once", "Ping Pong", "Hold"};
		if (ImGui::Combo("Loop Mode", &loopMode, loopNames, static_cast<int>(std::size(loopNames))))
		{
			m_animation.loopMode = static_cast<SpriteAnimationLoopMode>(loopMode);
		}

		if (ImGui::BeginTable("##animationEditorLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch, 0.7f);
			ImGui::TableSetupColumn("Atlas", ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableNextColumn();

			if (ImGui::Button(m_previewPlaying ? "Pause" : "Play"))
			{
				m_previewPlaying = !m_previewPlaying;
			}
			ImGui::SameLine();
			if (ImGui::Button("Restart"))
			{
				m_previewFrame = 0;
				m_previewDirection = 1;
				m_previewFrameTime = 0.0f;
				m_previewPlaying = true;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			ImGui::SliderFloat("Preview Speed", &m_previewSpeed, 0.1f, 4.0f, "%.2fx");
			if (m_previewPlaying)
			{
				AdvancePreview(ImGui::GetIO().DeltaTime * m_previewSpeed);
			}

			ImGui::SeparatorText("Preview");
			if (!m_animation.frames.empty())
			{
				const std::uint32_t frame = std::min<std::uint32_t>(m_previewFrame, static_cast<std::uint32_t>(m_animation.frames.size() - 1));
				const AssetObjectId id = m_animation.frames[frame].spriteId;
				const SpriteRegion* region = m_atlas.Find(id);
				ImGui::BeginChild("##animationPreview", ImVec2(0.0f, 110.0f), ImGuiChildFlags_Borders);
				ImGui::SetCursorPosY(28.0f);
				const std::string label = region != nullptr ? std::format("{}  ({} x {})", region->name, region->pixelRect.width, region->pixelRect.height) : std::format("Missing sprite {:016x}", id.value);
				const float textWidth = ImGui::CalcTextSize(label.c_str()).x;
				ImGui::SetCursorPosX(std::max(8.0f, (ImGui::GetContentRegionAvail().x - textWidth) * 0.5f));
				ImGui::TextUnformatted(label.c_str());
				ImGui::SetCursorPosX(8.0f);
				ImGui::ProgressBar(m_animation.frames[frame].durationSeconds > 0.0f ? m_previewFrameTime / m_animation.frames[frame].durationSeconds : 0.0f, ImVec2(-8.0f, 0.0f));
				ImGui::EndChild();
			}

			ImGui::SeparatorText("Timeline");
			ImGui::BeginChild("##spriteTimeline", ImVec2(0.0f, 135.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			for (std::size_t index = 0; index < m_animation.frames.size(); ++index)
			{
				if (index != 0)
				{
					ImGui::SameLine(0.0f, 3.0f);
				}
				const SpriteAnimationFrame& frame = m_animation.frames[index];
				const float width = std::clamp(frame.durationSeconds * 300.0f, 48.0f, 220.0f);
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable(std::format("{}\n{:.0f} ms", index, frame.durationSeconds * 1000.0f).c_str(), m_selectedFrame == static_cast<std::int32_t>(index), 0, ImVec2(width, 72.0f)))
				{
					m_selectedFrame = static_cast<std::int32_t>(index);
					m_previewFrame = static_cast<std::uint32_t>(index);
					m_previewFrameTime = 0.0f;
				}
				if (m_previewFrame == index)
				{
					const ImVec2 min = ImGui::GetItemRectMin();
					const ImVec2 max = ImGui::GetItemRectMax();
					ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y + 3.0f), ImVec2(max.x, max.y + 3.0f), IM_COL32(255, 190, 55, 255), 3.0f);
				}
				ImGui::PopID();
			}
			ImGui::EndChild();

			if (m_selectedFrame >= 0 && m_selectedFrame < static_cast<std::int32_t>(m_animation.frames.size()))
			{
				auto& frame = m_animation.frames[static_cast<std::size_t>(m_selectedFrame)];
				ImGui::DragFloat("Frame Duration", &frame.durationSeconds, 0.005f, 0.001f, 60.0f, "%.3f s");
				if (ImGui::Button("Move Left") && m_selectedFrame > 0)
				{
					std::swap(m_animation.frames[static_cast<std::size_t>(m_selectedFrame)], m_animation.frames[static_cast<std::size_t>(m_selectedFrame - 1)]);
					--m_selectedFrame;
				}
				ImGui::SameLine();
				if (ImGui::Button("Move Right") && m_selectedFrame + 1 < static_cast<std::int32_t>(m_animation.frames.size()))
				{
					std::swap(m_animation.frames[static_cast<std::size_t>(m_selectedFrame)], m_animation.frames[static_cast<std::size_t>(m_selectedFrame + 1)]);
					++m_selectedFrame;
				}
				ImGui::SameLine();
				if (ImGui::Button("Delete Frame"))
				{
					m_animation.frames.erase(m_animation.frames.begin() + m_selectedFrame);
					m_selectedFrame = std::min(m_selectedFrame, static_cast<std::int32_t>(m_animation.frames.size()) - 1);
					m_previewFrame = 0;
				}
			}

			ImGui::SeparatorText("Events");
			if (ImGui::Button("Add Event"))
			{
				m_animation.events.push_back({m_selectedFrame >= 0 ? static_cast<std::uint32_t>(m_selectedFrame) : 0u, "Event", {}});
				m_selectedEvent = static_cast<std::int32_t>(m_animation.events.size() - 1);
			}
			for (std::size_t index = 0; index < m_animation.events.size(); ++index)
			{
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable(std::format("Frame {}: {}", m_animation.events[index].frameIndex, m_animation.events[index].name).c_str(), m_selectedEvent == static_cast<std::int32_t>(index)))
				{
					m_selectedEvent = static_cast<std::int32_t>(index);
				}
				ImGui::PopID();
			}
			if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<std::int32_t>(m_animation.events.size()))
			{
				auto& event = m_animation.events[static_cast<std::size_t>(m_selectedEvent)];
				ImGui::DragScalar("Event Frame", ImGuiDataType_U32, &event.frameIndex, 1.0f);
				ImGui::InputText("Event Name", &event.name);
				ImGui::InputText("Payload", &event.payload);
				if (ImGui::Button("Delete Event"))
				{
					m_animation.events.erase(m_animation.events.begin() + m_selectedEvent);
					m_selectedEvent = -1;
				}
			}

			ImGui::TableNextColumn();
			ImGui::SeparatorText("Atlas Regions");
			if (ImGui::Button("Add All Regions", ImVec2(-1.0f, 0.0f)))
			{
				for (const SpriteRegion& region: m_atlas.sprites)
				{
					m_animation.frames.push_back({region.id, 0.1f});
				}
				m_selectedFrame = m_animation.frames.empty() ? -1 : 0;
			}
			ImGui::BeginChild("##atlasRegions", ImVec2(0.0f, 360.0f), ImGuiChildFlags_Borders);
			for (std::size_t index = 0; index < m_atlas.sprites.size(); ++index)
			{
				const SpriteRegion& region = m_atlas.sprites[index];
				if (ImGui::Selectable(region.name.c_str(), m_selectedAtlasRegion == static_cast<std::int32_t>(index)))
				{
					m_selectedAtlasRegion = static_cast<std::int32_t>(index);
				}
			}
			ImGui::EndChild();
			if (ImGui::Button("Add Selected Frame", ImVec2(-1.0f, 0.0f)) && m_selectedAtlasRegion >= 0 && m_selectedAtlasRegion < static_cast<std::int32_t>(m_atlas.sprites.size()))
			{
				m_animation.frames.push_back({m_atlas.sprites[static_cast<std::size_t>(m_selectedAtlasRegion)].id, 0.1f});
				m_selectedFrame = static_cast<std::int32_t>(m_animation.frames.size() - 1);
			}

			ImGui::Separator();
			if (ImGui::Button("Save Animation", ImVec2(-1.0f, 0.0f)))
			{
				m_animation.atlasPath = m_atlasPath.data();
				auto saved = context.Get<SpriteAssetStore>().SaveAnimation(m_animationPath.data(), m_animation);
				if (saved.has_value())
				{
					context.Get<AssetDatabase>().Register(MakeSpriteAnimationSource(m_animationPath.data()), std::filesystem::path(m_animationPath.data()).stem().string());
					m_status = std::format("Saved {} frames and {} events.", m_animation.frames.size(), m_animation.events.size());
					m_statusError = false;
				}
				else
				{
					m_status = saved.error().ToString();
					m_statusError = true;
				}
			}

			const auto* selection = context.TryGet<SceneSelection>();
			if (selection != nullptr && selection->Primary().IsValid() && ImGui::Button("Assign To Selected Entity", ImVec2(-1.0f, 0.0f)))
			{
				World& world = context.Get<World>();
				const Entity entity = selection->Primary();
				if (!world.Has<SpriteRendererComponent>(entity))
				{
					world.Emplace<SpriteRendererComponent>(entity);
				}
				world.EmplaceOrReplace<SpriteAnimatorComponent>(entity, SpriteAnimatorComponent{.animationPath = m_animationPath.data()});
				m_status = "Animation assigned to selected entity.";
				m_statusError = false;
			}

			if (!m_status.empty())
			{
				ImGui::TextColored(m_statusError ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "%s", m_status.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}

	void SpriteAnimationPanel::LoadAtlas(app::LayerContext& context)
	{
		const auto atlas = context.Get<SpriteAssetStore>().LoadAtlas(m_atlasPath.data());
		if (atlas.has_value())
		{
			m_atlas = **atlas;
			m_animation.atlasPath = m_atlasPath.data();
			m_selectedAtlasRegion = m_atlas.sprites.empty() ? -1 : 0;
			m_status = std::format("Loaded {} atlas regions.", m_atlas.sprites.size());
			m_statusError = false;
		}
		else
		{
			m_status = atlas.error().ToString();
			m_statusError = true;
		}
	}

	void SpriteAnimationPanel::AdvancePreview(float dt)
	{
		if (m_animation.frames.empty() || dt <= 0.0f)
		{
			return;
		}
		m_previewFrame = std::min<std::uint32_t>(m_previewFrame, static_cast<std::uint32_t>(m_animation.frames.size() - 1));
		m_previewFrameTime += dt;
		for (std::uint32_t transitions = 0; transitions < 128; ++transitions)
		{
			const float duration = std::max(m_animation.frames[m_previewFrame].durationSeconds, 0.001f);
			if (m_previewFrameTime < duration)
			{
				break;
			}
			m_previewFrameTime -= duration;
			if (m_animation.loopMode == SpriteAnimationLoopMode::Loop)
			{
				m_previewFrame = (m_previewFrame + 1u) % static_cast<std::uint32_t>(m_animation.frames.size());
			}
			else if (m_animation.loopMode == SpriteAnimationLoopMode::PingPong && m_animation.frames.size() > 1)
			{
				std::int32_t next = static_cast<std::int32_t>(m_previewFrame) + m_previewDirection;
				if (next >= static_cast<std::int32_t>(m_animation.frames.size()))
				{
					m_previewDirection = -1;
					next = static_cast<std::int32_t>(m_animation.frames.size()) - 2;
				}
				else if (next < 0)
				{
					m_previewDirection = 1;
					next = 1;
				}
				m_previewFrame = static_cast<std::uint32_t>(next);
			}
			else if (m_previewFrame + 1u < m_animation.frames.size())
			{
				++m_previewFrame;
			}
			else
			{
				m_previewPlaying = false;
				m_previewFrameTime = m_animation.loopMode == SpriteAnimationLoopMode::Hold ? duration : 0.0f;
				break;
			}
		}
	}
} // namespace aether::editor
