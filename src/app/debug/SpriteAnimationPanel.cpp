#include "AetherCore.hpp"
#include "debug/SpriteAnimationPanel.hpp"
#include "debug/EditorCommand.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/UndoStack.hpp"
#include "debug/SpriteAuthoringUi.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	namespace
	{
		inline constexpr const char* kSpriteRegionPayload = "AETHER_SPRITE_REGION";

		bool AnimationsEqual(const SpriteAnimationAsset& a, const SpriteAnimationAsset& b)
		{
			if (a.name != b.name || a.loopMode != b.loopMode || a.atlasPath != b.atlasPath || a.frames.size() != b.frames.size() || a.events.size() != b.events.size())
			{
				return false;
			}
			for (std::size_t i = 0; i < a.frames.size(); ++i)
			{
				if (!(a.frames[i].spriteId == b.frames[i].spriteId) || a.frames[i].durationSeconds != b.frames[i].durationSeconds)
				{
					return false;
				}
			}
			for (std::size_t i = 0; i < a.events.size(); ++i)
			{
				if (a.events[i].frameIndex != b.events[i].frameIndex || a.events[i].name != b.events[i].name || a.events[i].payload != b.events[i].payload)
				{
					return false;
				}
			}
			return true;
		}

		void AddRegionImage(ImDrawList* drawList, std::uint64_t textureId, const SpriteRegion* region, ImVec2 min, ImVec2 max)
		{
			drawList->AddRectFilled(min, max, chrome::U32(chrome::kPanel));
			if (textureId == 0 || region == nullptr)
			{
				chrome::CornerBrackets(drawList, min, max, 8.0f, 1.0f, chrome::WithAlpha(chrome::kMuted, 0.55f));
				return;
			}
			const ImVec2 uv0(region->uvRect.x, region->uvRect.y);
			const ImVec2 uv1(region->uvRect.z, region->uvRect.w);
			drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(textureId)), min, max, uv0, uv1);
		}
	} // namespace

	void SpriteAnimationPanel::OnDetach(app::LayerContext& context)
	{
		ReleasePreviewTexture(context);
	}

	void SpriteAnimationPanel::OnImGui(app::LayerContext& context)
	{
		if (!m_visible)
		{
			return;
		}
		ImGui::SetNextWindowSize(ImVec2(1080.0f, 760.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin(editor::DocumentTitle("Sprite Animation", AnimationDirty()).c_str(), &m_visible))
		{
			ImGui::End();
			return;
		}

		char stat[96]{};
		std::snprintf(stat, sizeof(stat), "%zu FRAMES  \xC2\xB7  %zu EVENTS", m_animation.frames.size(), m_animation.events.size());
		chrome::PanelHeader("SPRITE ANIMATION", stat);
		DrawUnsavedAnimationPrompt(context);

		// Snapshot the clip when an interaction starts in this window; the matching
		// commit runs at the end of the frame (records onto the global undo stack).
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			CaptureAnimationBaseline();
		}

		if (const spriteui::AssetSlotChange animation = spriteui::DrawAssetSlot(context, "animationAsset", ICON_FA_FILM, "Animation Clip", m_animationPath, spriteui::AssetRole::Animation, "Created beside the atlas when you save", true);
		        animation.changed)
		{
			if (animation.path.empty())
			{
				m_animationPath.clear();
			}
			else
			{
				LoadAnimation(context, animation.path);
			}
		}
		if (const spriteui::AssetSlotChange atlas = spriteui::DrawAssetSlot(context, "animationAtlas", ICON_FA_BOX_OPEN, "Sprite Atlas", m_atlasPath, spriteui::AssetRole::Atlas, "Select or drop a saved sprite atlas");
		        atlas.changed && !atlas.path.empty())
		{
			LoadAtlas(context, atlas.path);
		}
		spriteui::DrawStatus(m_status, m_statusError);

		if (m_atlasPath.empty() || m_atlas.sprites.empty())
		{
			spriteui::DrawEmptyState(
			        ICON_FA_FILM, "Choose a sprite atlas", "Select an atlas in File Explorer and click Use Selected, drag one onto the Sprite Atlas field, or choose it from the asset catalogue. Double-click regions to build the timeline.");
		}
		else if (ImGui::BeginTable("##animationEditorLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch, 0.70f);
			ImGui::TableSetupColumn("Atlas", ImGuiTableColumnFlags_WidthStretch, 0.30f);
			ImGui::TableNextColumn();
			ImGui::BeginChild("##animationTimelineColumn", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

			chrome::SectionTag("CLIP SETTINGS");
			if (ImGui::BeginTable("##clipSettings", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 92.0f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Name");
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::InputText("##clipName", &m_animation.name);
				int loopMode = static_cast<int>(m_animation.loopMode);
				const char* loopNames[] = {"Loop", "Once", "Ping Pong", "Hold"};
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Loop mode");
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##loopMode", &loopMode, loopNames, static_cast<int>(std::size(loopNames))))
				{
					m_animation.loopMode = static_cast<SpriteAnimationLoopMode>(loopMode);
				}
				ImGui::EndTable();
			}

			chrome::SectionTag("PREVIEW");
			const SpriteRegion* previewRegion = nullptr;
			std::uint32_t previewIndex = 0;
			if (!m_animation.frames.empty())
			{
				previewIndex = std::min<std::uint32_t>(m_previewFrame, static_cast<std::uint32_t>(m_animation.frames.size() - 1));
				previewRegion = m_atlas.Find(m_animation.frames[previewIndex].spriteId);
			}
			ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::kPanel);
			ImGui::BeginChild("##animationPreview", ImVec2(0.0f, 178.0f), ImGuiChildFlags_Borders);
			const float previewSide = 118.0f;
			ImGui::SetCursorPosX(std::max(8.0f, (ImGui::GetContentRegionAvail().x - previewSide) * 0.5f));
			DrawSpriteThumbnail(previewRegion, ImVec2(previewSide, previewSide));
			const std::string previewLabel = previewRegion != nullptr ? previewRegion->name : (m_animation.frames.empty() ? "Timeline is empty" : "Missing atlas region");
			const float previewLabelWidth = ImGui::CalcTextSize(previewLabel.c_str()).x;
			ImGui::SetCursorPosX(std::max(8.0f, (ImGui::GetContentRegionAvail().x - previewLabelWidth) * 0.5f));
			ImGui::TextUnformatted(previewLabel.c_str());
			ImGui::EndChild();
			ImGui::PopStyleColor();

			if (chrome::PrimaryButton(m_previewPlaying ? "Pause" : ICON_FA_PLAY "  Play"))
			{
				m_previewPlaying = !m_previewPlaying;
			}
			ImGui::SameLine();
			if (chrome::GhostButton(ICON_FA_ROTATE "  Restart"))
			{
				m_previewFrame = 0;
				m_previewDirection = 1;
				m_previewFrameTime = 0.0f;
				m_previewPlaying = true;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(150.0f);
			ImGui::SliderFloat("Speed", &m_previewSpeed, 0.1f, 4.0f, "%.2fx");
			if (m_previewPlaying)
			{
				// A clip playing is work, even with nobody touching the keyboard. Without this
				// the idle throttle steps it at 10 fps and the preview judders - which would
				// look like the animation is wrong rather than the editor being asleep. Only
				// while the window is focused: judder nobody is looking at is not worth
				// holding the whole editor awake for.
				if (auto* engine = context.TryGet<AetherCore>(); engine != nullptr && engine->IsWindowFocused())
				{
					engine->RequestActivity();
				}
				AdvancePreview(ImGui::GetIO().DeltaTime * m_previewSpeed);
			}

			chrome::SectionTag("TIMELINE");
			ImGui::BeginChild("##spriteTimeline", ImVec2(0.0f, 122.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			for (std::size_t index = 0; index < m_animation.frames.size(); ++index)
			{
				if (index != 0)
				{
					ImGui::SameLine(0.0f, 5.0f);
				}
				const SpriteAnimationFrame& frame = m_animation.frames[index];
				const SpriteRegion* region = m_atlas.Find(frame.spriteId);
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable("##frame", m_selectedFrame == static_cast<std::int32_t>(index), ImGuiSelectableFlags_None, ImVec2(86.0f, 98.0f)))
				{
					m_selectedFrame = static_cast<std::int32_t>(index);
					m_previewFrame = static_cast<std::uint32_t>(index);
					m_previewFrameTime = 0.0f;
				}
				const ImVec2 cardMin = ImGui::GetItemRectMin();
				const ImVec2 cardMax = ImGui::GetItemRectMax();
				AddRegionImage(ImGui::GetWindowDrawList(), m_previewTextureId, region, ImVec2(cardMin.x + 8.0f, cardMin.y + 7.0f), ImVec2(cardMax.x - 8.0f, cardMin.y + 70.0f));
				ImGui::GetWindowDrawList()->AddText(ImVec2(cardMin.x + 8.0f, cardMax.y - 22.0f), chrome::U32(chrome::kMuted), std::format("{}  \xC2\xB7  {:.0f}ms", index + 1, frame.durationSeconds * 1000.0f).c_str());
				if (m_previewFrame == index)
				{
					ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cardMin.x, cardMax.y - 3.0f), cardMax, chrome::U32(chrome::kAccent));
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSpriteRegionPayload); payload != nullptr && payload->DataSize == sizeof(std::uint32_t))
				{
					const std::uint32_t regionIndex = *static_cast<const std::uint32_t*>(payload->Data);
					if (regionIndex < m_atlas.sprites.size())
					{
						m_animation.frames.push_back({m_atlas.sprites[regionIndex].id, 0.1f});
						m_selectedFrame = static_cast<std::int32_t>(m_animation.frames.size() - 1);
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (m_animation.frames.empty())
			{
				ImGui::TextDisabled("Double-click or drag atlas regions here to add frames.");
			}
			else if (m_selectedFrame >= 0 && m_selectedFrame < static_cast<std::int32_t>(m_animation.frames.size()))
			{
				auto& frame = m_animation.frames[static_cast<std::size_t>(m_selectedFrame)];
				ImGui::SetNextItemWidth(180.0f);
				ImGui::DragFloat("Frame duration", &frame.durationSeconds, 0.005f, 0.001f, 60.0f, "%.3f s");
				if (chrome::GhostButton("Move left") && m_selectedFrame > 0)
				{
					std::swap(m_animation.frames[static_cast<std::size_t>(m_selectedFrame)], m_animation.frames[static_cast<std::size_t>(m_selectedFrame - 1)]);
					--m_selectedFrame;
				}
				ImGui::SameLine();
				if (chrome::GhostButton("Move right") && m_selectedFrame + 1 < static_cast<std::int32_t>(m_animation.frames.size()))
				{
					std::swap(m_animation.frames[static_cast<std::size_t>(m_selectedFrame)], m_animation.frames[static_cast<std::size_t>(m_selectedFrame + 1)]);
					++m_selectedFrame;
				}
				ImGui::SameLine();
				if (chrome::GhostButton(ICON_FA_TRASH "  Delete", ImVec2(0.0f, 0.0f), chrome::kError))
				{
					m_animation.frames.erase(m_animation.frames.begin() + m_selectedFrame);
					m_selectedFrame = std::min(m_selectedFrame, static_cast<std::int32_t>(m_animation.frames.size()) - 1);
					m_previewFrame = 0;
				}
			}

			if (ImGui::CollapsingHeader("Animation events"))
			{
				if (chrome::OutlineButton(ICON_FA_PLUS "  Add Event"))
				{
					m_animation.events.push_back({m_selectedFrame >= 0 ? static_cast<std::uint32_t>(m_selectedFrame) : 0u, "Event", {}});
					m_selectedEvent = static_cast<std::int32_t>(m_animation.events.size() - 1);
				}
				for (std::size_t index = 0; index < m_animation.events.size(); ++index)
				{
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(std::format("Frame {}  \xC2\xB7  {}", m_animation.events[index].frameIndex + 1, m_animation.events[index].name).c_str(), m_selectedEvent == static_cast<std::int32_t>(index)))
					{
						m_selectedEvent = static_cast<std::int32_t>(index);
					}
					ImGui::PopID();
				}
				if (m_selectedEvent >= 0 && m_selectedEvent < static_cast<std::int32_t>(m_animation.events.size()))
				{
					auto& event = m_animation.events[static_cast<std::size_t>(m_selectedEvent)];
					if (ImGui::BeginTable("##eventEditor", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
					{
						ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 92.0f);
						ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
						ImGui::TableNextColumn();
						ImGui::AlignTextToFramePadding();
						ImGui::TextDisabled("Frame");
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::DragScalar("##eventFrame", ImGuiDataType_U32, &event.frameIndex, 1.0f);
						if (!m_animation.frames.empty())
						{
							event.frameIndex = std::min(event.frameIndex, static_cast<std::uint32_t>(m_animation.frames.size() - 1));
						}
						ImGui::TableNextColumn();
						ImGui::AlignTextToFramePadding();
						ImGui::TextDisabled("Name");
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::InputText("##eventName", &event.name);
						ImGui::TableNextColumn();
						ImGui::AlignTextToFramePadding();
						ImGui::TextDisabled("Payload");
						ImGui::TableNextColumn();
						ImGui::SetNextItemWidth(-1.0f);
						ImGui::InputText("##eventPayload", &event.payload);
						ImGui::EndTable();
					}
					if (chrome::GhostButton(ICON_FA_TRASH "  Delete Event", ImVec2(0.0f, 0.0f), chrome::kError))
					{
						m_animation.events.erase(m_animation.events.begin() + m_selectedEvent);
						m_selectedEvent = -1;
					}
				}
			}
			ImGui::EndChild();

			ImGui::TableNextColumn();
			ImGui::BeginChild("##animationAtlasColumn", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
			chrome::SectionTag("ATLAS REGIONS");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##regionFilter", ICON_FA_MAGNIFYING_GLASS "  Filter regions", m_regionFilter, sizeof(m_regionFilter));
			const std::string filter = spriteui::Lower(m_regionFilter);
			ImGui::BeginChild("##atlasRegions", ImVec2(0.0f, -150.0f), ImGuiChildFlags_Borders);
			for (std::size_t index = 0; index < m_atlas.sprites.size(); ++index)
			{
				const SpriteRegion& region = m_atlas.sprites[index];
				if (!filter.empty() && !spriteui::Lower(region.name).contains(filter))
				{
					continue;
				}
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Selectable("##region", m_selectedAtlasRegion == static_cast<std::int32_t>(index), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 46.0f)))
				{
					m_selectedAtlasRegion = static_cast<std::int32_t>(index);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						m_animation.frames.push_back({region.id, 0.1f});
						m_selectedFrame = static_cast<std::int32_t>(m_animation.frames.size() - 1);
					}
				}
				const ImVec2 rowMin = ImGui::GetItemRectMin();
				const ImVec2 rowMax = ImGui::GetItemRectMax();
				AddRegionImage(ImGui::GetWindowDrawList(), m_previewTextureId, &region, ImVec2(rowMin.x + 5.0f, rowMin.y + 5.0f), ImVec2(rowMin.x + 41.0f, rowMax.y - 5.0f));
				ImGui::GetWindowDrawList()->AddText(ImVec2(rowMin.x + 49.0f, rowMin.y + 8.0f), chrome::U32(chrome::kText), region.name.c_str());
				ImGui::GetWindowDrawList()->AddText(ImVec2(rowMin.x + 49.0f, rowMin.y + 25.0f), chrome::U32(chrome::kFaint), std::format("{} x {}", region.pixelRect.width, region.pixelRect.height).c_str());
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
				{
					const std::uint32_t payloadIndex = static_cast<std::uint32_t>(index);
					ImGui::SetDragDropPayload(kSpriteRegionPayload, &payloadIndex, sizeof(payloadIndex));
					ImGui::Text("%s  %s", ICON_FA_IMAGE, region.name.c_str());
					ImGui::EndDragDropSource();
				}
				ImGui::PopID();
			}
			ImGui::EndChild();

			ImGui::BeginDisabled(m_selectedAtlasRegion < 0 || m_selectedAtlasRegion >= static_cast<std::int32_t>(m_atlas.sprites.size()));
			if (chrome::PrimaryButton(ICON_FA_PLUS "  Add Selected Frame", ImVec2(-1.0f, 0.0f)))
			{
				m_animation.frames.push_back({m_atlas.sprites[static_cast<std::size_t>(m_selectedAtlasRegion)].id, 0.1f});
				m_selectedFrame = static_cast<std::int32_t>(m_animation.frames.size() - 1);
			}
			ImGui::EndDisabled();
			if (chrome::OutlineButton("Add All Visible Regions", ImVec2(-1.0f, 0.0f)))
			{
				for (const SpriteRegion& region: m_atlas.sprites)
				{
					if (filter.empty() || spriteui::Lower(region.name).contains(filter))
					{
						m_animation.frames.push_back({region.id, 0.1f});
					}
				}
				m_selectedFrame = m_animation.frames.empty() ? -1 : static_cast<std::int32_t>(m_animation.frames.size() - 1);
			}

			if (ImGui::CollapsingHeader("Advanced file locations"))
			{
				ImGui::InputText("Animation path", &m_animationPath);
				ImGui::InputText("Atlas path", &m_atlasPath);
				if (chrome::GhostButton("Load animation"))
				{
					LoadAnimation(context, m_animationPath);
				}
				ImGui::SameLine();
				if (chrome::GhostButton("Load atlas"))
				{
					LoadAtlas(context, m_atlasPath);
				}
			}

			ImGui::BeginDisabled(m_animationPath.empty());
			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save Animation", ImVec2(-1.0f, 0.0f)))
			{
				SaveAnimation(context);
			}
			ImGui::EndDisabled();

			const auto* selection = context.TryGet<SceneSelection>();
			const Entity target = selection != nullptr ? selection->LastEntityPrimary() : Entity{};
			ImGui::BeginDisabled(!target.IsValid() || m_animationPath.empty());
			if (chrome::OutlineButton("Assign To Last Selected Entity", ImVec2(-1.0f, 0.0f)))
			{
				World& world = context.Get<World>();
				// Assigning adds up to three components and rewrites the sprite renderer's
				// fields. All of it is scene data, so it belongs in history as ONE entry -
				// otherwise Ctrl+Z after an accidental assign undoes an unrelated edit.
				auto* assignUndo = context.services.TryGet<UndoStack>();
				UndoStack::ScopedGroup assignGroup(assignUndo, "Assign Animation");
				const auto recordAdd = [&](bool alreadyPresent, const char* componentName)
				{
					if (!alreadyPresent && assignUndo != nullptr)
					{
						assignUndo->Record(std::make_unique<AddComponentCommand>(target.id, componentName));
					}
				};
				nlohmann::json rendererBefore;
				bool rendererReflected = false;
				const bool hadRenderer = world.Has<SpriteRendererComponent>(target);
				if (hadRenderer && assignUndo != nullptr)
				{
					CaptureComponentFields(world, target, "Sprite Renderer", context.services, rendererBefore, rendererReflected);
				}

				recordAdd(world.Has<TransformComponent>(target), "Transform");
				if (!world.Has<TransformComponent>(target))
				{
					world.Emplace<TransformComponent>(target);
				}
				recordAdd(hadRenderer, "Sprite Renderer");
				if (!hadRenderer)
				{
					world.Emplace<SpriteRendererComponent>(target);
				}
				recordAdd(world.Has<SpriteAnimatorComponent>(target), "Sprite Animator");
				auto& renderer = world.Get<SpriteRendererComponent>(target);
				if (!m_animation.frames.empty())
				{
					if (const SpriteRegion* region = m_atlas.Find(m_animation.frames.front().spriteId); region != nullptr)
					{
						renderer.texturePath = m_atlas.texturePath;
						renderer.atlasPath = m_atlasPath;
						renderer.spriteId = region->id;
						renderer.uvRect = region->uvRect;
						renderer.pixelSize = region->pixelSize;
						renderer.pivot = region->pivot;
						renderer.pixelsPerUnit = m_atlas.pixelsPerUnit;
						renderer.visible = true;
					}
				}
				world.EmplaceOrReplace<SpriteAnimatorComponent>(target, SpriteAnimatorComponent{.animationPath = m_animationPath});
				// A renderer that already existed had its fields overwritten rather than added,
				// so the add command above does not cover it - the before/after pair does.
				if (hadRenderer && assignUndo != nullptr)
				{
					nlohmann::json rendererAfter;
					bool afterReflected = false;
					if (CaptureComponentFields(world, target, "Sprite Renderer", context.services, rendererAfter, afterReflected) && rendererAfter != rendererBefore)
					{
						assignUndo->Record(std::make_unique<SetComponentCommand>(target.id, "Sprite Renderer", std::move(rendererBefore), std::move(rendererAfter), afterReflected));
					}
				}
				m_status = "Animation assigned and the first frame is visible in the viewport.";
				m_statusError = false;
			}
			ImGui::EndDisabled();
			ImGui::EndChild();
			ImGui::EndTable();
		}

		// Finalize an in-flight clip edit once the mouse is released.
		CommitAnimationEdit(context);

		ImGui::End();
	}

	std::uint64_t SpriteAnimationPanel::AnimationSignature() const
	{
		editor::DocumentHash hash;
		hash.Add(m_animation.name);
		hash.Add(m_animation.atlasPath);
		hash.Add(static_cast<std::int64_t>(m_animation.loopMode));
		hash.Add(static_cast<std::int64_t>(m_animation.frames.size()));
		for (const SpriteAnimationFrame& frame: m_animation.frames)
		{
			hash.Add(static_cast<std::int64_t>(frame.spriteId.value));
			hash.Add(frame.durationSeconds);
		}
		return hash.Value();
	}

	bool SpriteAnimationPanel::AnimationDirty() const
	{
		return !m_animationPath.empty() && AnimationSignature() != m_savedAnimationSignature;
	}

	void SpriteAnimationPanel::DrawUnsavedAnimationPrompt(app::LayerContext& context)
	{
		if (m_pendingAnimationPath.empty())
		{
			return;
		}
		ImGui::OpenPopup("Unsaved animation##animSwitch");
		if (ImGui::BeginPopupModal("Unsaved animation##animSwitch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s has unsaved changes.", spriteui::DisplayName(m_animationPath).c_str());
			ImGui::TextDisabled("Opening another clip will lose them.");
			ImGui::Spacing();
			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save and open", ImVec2(150.0f, 0.0f)))
			{
				const std::string next = m_pendingAnimationPath;
				m_pendingAnimationPath.clear();
				SaveAnimation(context);
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				LoadAnimation(context, next);
				return;
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Discard", ImVec2(110.0f, 0.0f)))
			{
				const std::string next = m_pendingAnimationPath;
				m_pendingAnimationPath.clear();
				m_savedAnimationSignature = AnimationSignature();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				LoadAnimation(context, next);
				return;
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Keep editing", ImVec2(130.0f, 0.0f)))
			{
				m_pendingAnimationPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void SpriteAnimationPanel::LoadAnimation(app::LayerContext& context, std::string path)
	{
		if (path.empty())
		{
			return;
		}
		// Timing a clip frame by frame is slow work with no undo for the file.
		if (AnimationDirty() && path != m_animationPath)
		{
			m_pendingAnimationPath = path;
			return;
		}
		const auto loaded = context.Get<SpriteAssetStore>().LoadAnimation(path);
		if (!loaded.has_value())
		{
			m_status = loaded.error().ToString();
			m_statusError = true;
			return;
		}
		m_animationPath = std::move(path);
		m_animation = **loaded;
		m_savedAnimationSignature = AnimationSignature();
		m_selectedFrame = m_animation.frames.empty() ? -1 : 0;
		m_previewFrame = 0;
		m_previewFrameTime = 0.0f;
		if (!m_animation.atlasPath.empty())
		{
			LoadAtlas(context, m_animation.atlasPath);
		}
		context.Get<AssetDatabase>().Register(MakeSpriteAnimationSource(m_animationPath), spriteui::DisplayName(m_animationPath));
		m_status = std::format("Animation loaded with {} frames.", m_animation.frames.size());
		m_statusError = false;
	}

	void SpriteAnimationPanel::LoadAtlas(app::LayerContext& context, std::string path)
	{
		if (path.empty())
		{
			return;
		}
		const auto atlas = context.Get<SpriteAssetStore>().LoadAtlas(path);
		if (!atlas.has_value())
		{
			m_status = atlas.error().ToString();
			m_statusError = true;
			return;
		}
		m_atlasPath = std::move(path);
		m_atlas = **atlas;
		m_animation.atlasPath = m_atlasPath;
		m_selectedAtlasRegion = m_atlas.sprites.empty() ? -1 : 0;
		if (m_animationPath.empty())
		{
			m_animationPath = spriteui::CompanionPath(m_atlasPath, ".spriteanim.toml");
		}
		LoadPreviewTexture(context);
		context.Get<AssetDatabase>().Register(MakeSpriteAtlasSource(m_atlasPath), spriteui::DisplayName(m_atlasPath));
		m_status = std::format("Atlas ready with {} regions.", m_atlas.sprites.size());
		m_statusError = false;
	}

	void SpriteAnimationPanel::LoadPreviewTexture(app::LayerContext& context)
	{
		ReleasePreviewTexture(context);
		if (m_atlas.texturePath.empty())
		{
			return;
		}
		auto& textures = context.Get<AssetManager>().GetTextureRegistry();
		m_previewTexture = textures.Acquire(m_atlas.texturePath);
		const std::uint32_t slot = textures.ResolveSlot(m_previewTexture);
		if (auto* imgui = context.TryGet<ImguiSubsystem>())
		{
			for (const gpu::DebugTextureInfo& info: gpu::ResourceRegistry::ListDebugTextures())
			{
				if (!info.hasBindlessSampled || info.bindlessSampledSlot != slot || info.view == nullptr)
				{
					continue;
				}
				const ImTextureID id = imgui->RegisterTexture(info.view, gpu::ImageLayout::ShaderReadOnly);
				if (id != ImTextureID_Invalid)
				{
					m_previewTextureId = static_cast<std::uint64_t>(id);
				}
				break;
			}
		}
	}

	void SpriteAnimationPanel::ReleasePreviewTexture(app::LayerContext& context)
	{
		if (m_previewTextureId != 0)
		{
			if (auto* imgui = context.TryGet<ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_previewTextureId));
			}
			m_previewTextureId = 0;
		}
		if (m_previewTexture.IsValid())
		{
			context.Get<AssetManager>().GetTextureRegistry().Release(m_previewTexture);
			m_previewTexture = {};
		}
	}

	void SpriteAnimationPanel::SaveAnimation(app::LayerContext& context)
	{
		m_animation.atlasPath = m_atlasPath;
		const auto saved = context.Get<SpriteAssetStore>().SaveAnimation(m_animationPath, m_animation);
		if (saved.has_value())
		{
			context.Get<AssetDatabase>().Register(MakeSpriteAnimationSource(m_animationPath), spriteui::DisplayName(m_animationPath));
			m_savedAnimationSignature = AnimationSignature();
			m_status = std::format("Saved {} frames and {} events.", m_animation.frames.size(), m_animation.events.size());
			m_statusError = false;
		}
		else
		{
			m_status = saved.error().ToString();
			m_statusError = true;
		}
	}

	void SpriteAnimationPanel::DrawSpriteThumbnail(const SpriteRegion* region, ImVec2 size) const
	{
		ImGui::Dummy(size);
		AddRegionImage(ImGui::GetWindowDrawList(), m_previewTextureId, region, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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

	void SpriteAnimationPanel::CaptureAnimationBaseline()
	{
		if (!m_animUndoActive)
		{
			m_animUndoBaseline = m_animation;
			m_animUndoActive = true;
		}
	}

	void SpriteAnimationPanel::CommitAnimationEdit(app::LayerContext& context)
	{
		if (!m_animUndoActive || ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			return; // wait for the interaction to finish
		}
		if (!AnimationsEqual(m_animation, m_animUndoBaseline))
		{
			if (auto* undo = context.services.TryGet<UndoStack>())
			{
				undo->Record(std::make_unique<SpriteAnimationEditCommand>(m_animUndoBaseline, m_animation, [this](const SpriteAnimationAsset& animation) { ApplyUndoneAnimation(animation); }));
			}
		}
		m_animUndoActive = false;
	}

	void SpriteAnimationPanel::ApplyUndoneAnimation(const SpriteAnimationAsset& animation)
	{
		m_animation = animation;
		if (m_animation.frames.empty())
		{
			m_selectedFrame = -1;
			m_previewFrame = 0;
		}
		else
		{
			m_selectedFrame = std::min(m_selectedFrame, static_cast<std::int32_t>(m_animation.frames.size()) - 1);
			m_previewFrame = std::min<std::uint32_t>(m_previewFrame, static_cast<std::uint32_t>(m_animation.frames.size() - 1));
		}
		if (m_selectedEvent >= static_cast<std::int32_t>(m_animation.events.size()))
		{
			m_selectedEvent = -1;
		}
		m_previewFrameTime = 0.0f;
	}

	bool SpriteAnimationPanel::HasUnsavedWork(app::LayerContext& /*context*/) const
	{
		return AnimationDirty();
	}

	bool SpriteAnimationPanel::SaveUnsavedWork(app::LayerContext& context)
	{
		SaveAnimation(context);
		return !AnimationDirty();
	}
} // namespace aether::editor
