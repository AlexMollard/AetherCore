#include "AetherCore.hpp"
#include "debug/ParticlePanel.hpp"
#include "debug/UndoStack.hpp"
#include "debug/EditorCommand.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "particles/ParticleComponents.hpp"
#include "particles/ParticleSystem.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	using iw::PropCheckbox;
	using iw::PropColor4;
	using iw::PropComboStr;
	using iw::PropDrag2;
	using iw::PropFloat;
	using iw::PropInt;
	using iw::PropLabel;
	using iw::SectionHeader;

	namespace
	{
		constexpr const char* kDot = "project://assets/textures/particles/dot_soft.png";
		constexpr const char* kSpark = "project://assets/textures/particles/spark.png";

		ParticleEmitterComponent Preset(std::string_view name)
		{
			ParticleEmitterComponent e;
			if (name == "Fountain")
			{
				e.texturePath = kDot;
				e.rate = 90.0f;
				e.lifetimeMin = 0.8f;
				e.lifetimeMax = 1.2f;
				e.speedMin = 5.0f;
				e.speedMax = 7.0f;
				e.directionDeg = 90.0f;
				e.spreadDeg = 18.0f;
				e.gravity = {0.0f, -9.0f};
				e.startSize = 0.22f;
				e.endSize = 0.05f;
				e.startColor = {0.55f, 0.8f, 1.0f, 1.0f};
				e.endColor = {0.3f, 0.5f, 0.9f, 0.0f};
				e.blendMode = SpriteBlendMode::Alpha;
			}
			else if (name == "Explosion")
			{
				e.texturePath = kSpark;
				e.burstCount = 40;
				e.emitOnStart = true;
				e.lifetimeMin = 0.3f;
				e.lifetimeMax = 0.7f;
				e.speedMin = 4.0f;
				e.speedMax = 10.0f;
				e.spreadDeg = 180.0f;
				e.gravity = {0.0f, -5.0f};
				e.startSize = 0.5f;
				e.endSize = 0.0f;
				e.startColor = {1.0f, 0.85f, 0.35f, 1.0f};
				e.endColor = {1.0f, 0.25f, 0.05f, 0.0f};
				e.blendMode = SpriteBlendMode::Additive;
			}
			else if (name == "Fire")
			{
				e.texturePath = kDot;
				e.rate = 60.0f;
				e.lifetimeMin = 0.5f;
				e.lifetimeMax = 0.9f;
				e.speedMin = 1.5f;
				e.speedMax = 3.0f;
				e.directionDeg = 90.0f;
				e.spreadDeg = 22.0f;
				e.gravity = {0.0f, 1.5f};
				e.startSize = 0.5f;
				e.endSize = 0.05f;
				e.startColor = {1.0f, 0.8f, 0.3f, 1.0f};
				e.endColor = {0.9f, 0.1f, 0.0f, 0.0f};
				e.blendMode = SpriteBlendMode::Additive;
			}
			else if (name == "Smoke")
			{
				e.texturePath = kDot;
				e.rate = 24.0f;
				e.lifetimeMin = 1.4f;
				e.lifetimeMax = 2.2f;
				e.speedMin = 0.6f;
				e.speedMax = 1.4f;
				e.directionDeg = 90.0f;
				e.spreadDeg = 30.0f;
				e.gravity = {0.2f, 0.6f};
				e.startSize = 0.4f;
				e.endSize = 1.1f;
				e.startColor = {0.5f, 0.5f, 0.5f, 0.6f};
				e.endColor = {0.3f, 0.3f, 0.3f, 0.0f};
				e.blendMode = SpriteBlendMode::Alpha;
			}
			else if (name == "Sparkle")
			{
				e.texturePath = kSpark;
				e.burstCount = 14;
				e.emitOnStart = true;
				e.lifetimeMin = 0.3f;
				e.lifetimeMax = 0.55f;
				e.speedMin = 1.5f;
				e.speedMax = 4.5f;
				e.spreadDeg = 180.0f;
				e.gravity = {0.0f, -3.0f};
				e.startSize = 0.35f;
				e.endSize = 0.0f;
				e.startColor = {1.0f, 0.95f, 0.5f, 1.0f};
				e.endColor = {1.0f, 0.7f, 0.2f, 0.0f};
				e.blendMode = SpriteBlendMode::Additive;
			}
			else if (name == "Rain")
			{
				e.texturePath = kDot;
				e.rate = 120.0f;
				e.lifetimeMin = 3.0f;
				e.lifetimeMax = 3.5f;
				e.speedMin = 0.5f;
				e.speedMax = 1.5f;
				e.directionDeg = 270.0f;
				e.spreadDeg = 12.0f;
				e.gravity = {0.0f, -12.0f};
				e.startSize = 0.14f;
				e.endSize = 0.14f;
				e.startColor = {0.55f, 0.75f, 1.0f, 0.9f};
				e.endColor = {0.55f, 0.75f, 1.0f, 0.9f};
				e.blendMode = SpriteBlendMode::Alpha;
				e.collideWorld = true;
				e.collideParticles = true;
				e.bounce = 0.15f;
				e.collisionDamping = 0.4f;
				e.collisionRadius = 0.12f;
			}
			else if (name == "Confetti")
			{
				e.texturePath = kDot;
				e.burstCount = 60;
				e.emitOnStart = true;
				e.lifetimeMin = 1.2f;
				e.lifetimeMax = 2.0f;
				e.speedMin = 4.0f;
				e.speedMax = 8.0f;
				e.spreadDeg = 70.0f;
				e.gravity = {0.0f, -8.0f};
				e.startSize = 0.2f;
				e.endSize = 0.2f;
				e.startColor = {1.0f, 0.4f, 0.5f, 1.0f};
				e.endColor = {0.4f, 0.7f, 1.0f, 0.0f};
				e.blendMode = SpriteBlendMode::Alpha;
				e.collideWorld = true;
				e.bounce = 0.4f;
				e.collisionDamping = 0.3f;
				e.collisionRadius = 0.1f;
			}
			return e;
		}

		glm::vec2 CameraFocus(World& world)
		{
			if (const Entity cam = ecs::GetMainCameraEntity(world); cam.IsValid())
			{
				if (const auto* tc = world.TryGet<TransformComponent>(cam))
				{
					return glm::vec2(tc->localToWorld[3]);
				}
			}
			return {0.0f, 3.0f};
		}

		// Min/max pair on one row via ImGui's range drag.
		bool RangeRow(const char* label, float* lo, float* hi, float speed, float min, float max, const char* fmt)
		{
			ImGui::PushID(label);
			PropLabel(label);
			const bool changed = ImGui::DragFloatRange2("##range", lo, hi, speed, min, max, fmt, fmt, ImGuiSliderFlags_AlwaysClamp);
			ImGui::PopID();
			return changed;
		}

		// Draggable emission dial: grab anywhere in the circle to aim; the wedge
		// shows the spread cone.
		bool DirectionDial(float* directionDeg, float spreadDeg)
		{
			const float size = 84.0f;
			ImGui::PushID("dial");
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("##dial", ImVec2(size, size));
			const bool active = ImGui::IsItemActive();
			const ImVec2 c{p.x + size * 0.5f, p.y + size * 0.5f};
			const float r = size * 0.5f - 4.0f;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddCircleFilled(c, r, IM_COL32(26, 28, 36, 255), 40);
			dl->AddCircle(c, r, IM_COL32(92, 98, 116, 255), 40, 1.5f);

			// Spread wedge (screen Y is down, so negate the sin term).
			const float a0 = glm::radians(*directionDeg - spreadDeg);
			const float a1 = glm::radians(*directionDeg + spreadDeg);
			dl->PathClear();
			dl->PathLineTo(c);
			const int seg = 24;
			for (int i = 0; i <= seg; ++i)
			{
				const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(seg);
				dl->PathLineTo(ImVec2(c.x + std::cos(a) * r, c.y - std::sin(a) * r));
			}
			dl->PathFillConvex(IM_COL32(255, 176, 64, 55));

			const float rad = glm::radians(*directionDeg);
			const ImVec2 tip{c.x + std::cos(rad) * r, c.y - std::sin(rad) * r};
			dl->AddLine(c, tip, IM_COL32(255, 200, 90, 255), 2.0f);
			dl->AddCircleFilled(tip, 4.5f, IM_COL32(255, 222, 130, 255));

			bool changed = false;
			if (active)
			{
				const ImVec2 m = ImGui::GetIO().MousePos;
				float deg = glm::degrees(std::atan2(-(m.y - c.y), m.x - c.x));
				if (deg < 0.0f)
				{
					deg += 360.0f;
				}
				*directionDeg = deg;
				changed = true;
			}
			ImGui::PopID();
			return changed;
		}

		// Horizontal bar showing the start->end colour interpolation.
		void GradientBar(const glm::vec4& a, const glm::vec4& b, float height)
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			ImGui::Dummy(ImVec2(w, height));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			// Checker under the alpha so transparency reads.
			const ImU32 c1 = IM_COL32(60, 60, 66, 255);
			const ImU32 c2 = IM_COL32(44, 44, 50, 255);
			const float cs = 6.0f;
			for (float y = 0; y < height; y += cs)
			{
				for (float x = 0; x < w; x += cs)
				{
					const bool alt = (static_cast<int>(x / cs) + static_cast<int>(y / cs)) % 2 == 0;
					dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + cs, w), p.y + std::min(y + cs, height)), alt ? c1 : c2);
				}
			}
			const ImU32 ca = ToU32(a);
			const ImU32 cb = ToU32(b);
			dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + height), ca, cb, cb, ca);
			dl->AddRect(p, ImVec2(p.x + w, p.y + height), IM_COL32(90, 94, 108, 255), 3.0f);
		}

		void DrawPreviewCanvas(const ParticleEmitterComponent& sim, bool darkBg)
		{
			const float w = ImGui::GetContentRegionAvail().x;
			const float h = 180.0f;
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const ImVec2 p1{p0.x + w, p0.y + h};
			ImGui::InvisibleButton("##preview", ImVec2(w, h));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p0, p1, darkBg ? IM_COL32(16, 17, 23, 255) : IM_COL32(150, 160, 175, 255), 6.0f);
			dl->AddRect(p0, p1, IM_COL32(80, 84, 96, 255), 6.0f, 0, 1.5f);
			dl->PushClipRect(p0, p1, true);

			const ImVec2 origin{(p0.x + p1.x) * 0.5f, p0.y + h * 0.62f};
			constexpr float scale = 17.0f; // px per world unit

			// Emitter marker + aim.
			dl->AddCircleFilled(origin, 3.0f, IM_COL32(120, 130, 150, 200));
			const float rad = glm::radians(sim.directionDeg);
			dl->AddLine(origin, ImVec2(origin.x + std::cos(rad) * 16.0f, origin.y - std::sin(rad) * 16.0f), IM_COL32(120, 130, 150, 140), 1.0f);

			for (const Particle& pt: sim.particles)
			{
				const float t = std::clamp(pt.age / pt.lifetime, 0.0f, 1.0f);
				const float size = std::max(0.0f, glm::mix(sim.startSize, sim.endSize, t)) * pt.sizeJitter;
				const glm::vec4 col = glm::mix(sim.startColor, sim.endColor, t);
				const ImVec2 sp{origin.x + pt.position.x * scale, origin.y - pt.position.y * scale};
				const float pr = std::max(1.0f, size * scale * 0.5f);
				if (sp.x < p0.x - pr || sp.x > p1.x + pr || sp.y < p0.y - pr || sp.y > p1.y + pr)
				{
					continue;
				}
				if (sim.blendMode == SpriteBlendMode::Additive)
				{
					dl->AddCircleFilled(sp, pr * 1.7f, ToU32(glm::vec4(col.r, col.g, col.b, col.a * 0.28f)), 12);
				}
				dl->AddCircleFilled(sp, pr, ToU32(col), 12);
			}
			dl->PopClipRect();

			// Live count, bottom-left. Sat on the font's height rather than a fixed 18px: at
			// the 1.5x content scale of a high-DPI display the text is taller than that, so it
			// overran the preview frame and the frame clipped its top off.
			char meta[32];
			std::snprintf(meta, sizeof(meta), "%zu particles", sim.particles.size());
			const float inset = ImGui::GetStyle().ItemInnerSpacing.y;
			dl->AddText(ImVec2(p0.x + 8.0f, p1.y - ImGui::GetTextLineHeight() - inset), IM_COL32(150, 156, 170, 200), meta);
		}
	} // namespace

	void ParticlePanel::SyncPreview()
	{
		// Carry authored fields onto the running sim, preserving its live state.
		std::vector<Particle> buf = std::move(m_preview.particles);
		const float acc = m_preview.spawnAccumulator;
		const std::uint32_t rng = m_preview.rngState;
		const bool started = m_preview.started;
		const std::uint32_t pending = m_preview.pendingBurst;
		m_preview = m_config;
		m_preview.autoDestroyWhenDone = false;
		m_preview.particles = std::move(buf);
		m_preview.spawnAccumulator = acc;
		m_preview.rngState = rng;
		m_preview.started = started;
		m_preview.pendingBurst = pending;
	}

	void ParticlePanel::OnImGui(app::LayerContext& context)
	{
		World& world = context.Get<World>();

		// The preview simulates on its own clock while the window is open.
		SyncPreview();
		if (m_playing)
		{
			// A live preview is work: throttled to 10 fps the simulation looks broken rather
			// than idle, and emission rates read wrong because each step covers 100 ms. Only
			// while someone is here to see it, though - left ungated this kept the editor at
			// full frame rate after alt-tabbing away, animating a preview nobody was watching.
			if (auto* engine = context.TryGet<AetherCore>(); engine != nullptr && engine->IsWindowFocused())
			{
				engine->RequestActivity();
			}
			ParticleSystem::StepStandalone(m_preview, ImGui::GetIO().DeltaTime, glm::vec2(0.0f));
		}

		if (ImGui::Begin("Particles", VisiblePtr()))
		{
			chrome::PanelHeader("PARTICLE STUDIO", m_playing ? "LIVE" : "PAUSED");

			// ── Live preview ──────────────────────────────────────────────────
			DrawPreviewCanvas(m_preview, m_darkBg);
			ImGui::Spacing();
			if (chrome::GhostIconButton(m_playing ? ICON_FA_STOP : ICON_FA_PLAY, "##play", ImVec2(30.0f, 0.0f)))
			{
				m_playing = !m_playing;
			}
			ImGui::SetItemTooltip(m_playing ? "Pause preview" : "Play preview");
			ImGui::SameLine();
			if (chrome::GhostIconButton(ICON_FA_ROTATE, "##restart", ImVec2(30.0f, 0.0f)))
			{
				m_preview.particles.clear();
				m_preview.spawnAccumulator = 0.0f;
				m_preview.started = false;
				m_preview.pendingBurst = 0;
			}
			ImGui::SetItemTooltip("Restart preview");
			ImGui::SameLine();
			if (chrome::GhostIconButton(ICON_FA_BOLT, "##burst", ImVec2(30.0f, 0.0f)))
			{
				m_preview.pendingBurst += m_config.burstCount > 0 ? m_config.burstCount : 20u;
			}
			ImGui::SetItemTooltip("Fire a one-shot burst");
			ImGui::SameLine();
			ImGui::Checkbox("Dark bg", &m_darkBg);

			// ── Presets ───────────────────────────────────────────────────────
			ImGui::Spacing();
			chrome::SectionTag("PRESETS");
			ImGui::Spacing();
			static constexpr const char* kPresets[] = {"Fountain", "Explosion", "Fire", "Smoke", "Sparkle", "Rain", "Confetti"};
			const float avail = ImGui::GetContentRegionAvail().x;
			float rowX = 0.0f;
			for (const char* preset: kPresets)
			{
				const float bw = ImGui::CalcTextSize(preset).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				if (rowX > 0.0f && rowX + bw + ImGui::GetStyle().ItemSpacing.x > avail)
				{
					rowX = 0.0f;
				}
				else if (rowX > 0.0f)
				{
					ImGui::SameLine();
				}
				if (chrome::OutlineButton(preset))
				{
					m_config = Preset(preset);
					m_preview.particles.clear();
					m_preview.started = false;
					m_status = std::string("Loaded '") + preset + "' preset.";
					m_statusError = false;
				}
				rowX += bw + ImGui::GetStyle().ItemSpacing.x;
			}

			// ── Parameters ────────────────────────────────────────────────────
			ImGui::Spacing();
			if (SectionHeader(ICON_FA_BOLT "  Emission", ImGuiTreeNodeFlags_DefaultOpen))
			{
				PropFloat("Rate (/s)", &m_config.rate, 0.5f, 0.0f, 2000.0f, "%.0f", "Continuous particles per second (0 = burst only)");
				int burst = static_cast<int>(m_config.burstCount);
				if (PropInt("Burst count", &burst, 1.0f, 0, 100000))
				{
					m_config.burstCount = static_cast<std::uint32_t>(std::max(0, burst));
				}
				PropCheckbox("Emit on start", &m_config.emitOnStart, "Fire the burst once when the emitter appears");
				// One row each. PropCheckbox draws a whole row - label column and all - so
				// putting a second one on the same line re-based its column from the current
				// cursor and pushed its checkbox to the panel edge, far from the value column
				// every other one sits in, leaving neither box clearly attached to a label.
				PropCheckbox("Emitting", &m_config.emitting, "Spawn new particles at the rate above");
				PropCheckbox("Auto-destroy when done", &m_config.autoDestroyWhenDone, "One-shot effect entities retire themselves");
				int maxP = static_cast<int>(m_config.maxParticles);
				if (PropInt("Max particles", &maxP, 4.0f, 1, 100000))
				{
					m_config.maxParticles = static_cast<std::uint32_t>(std::max(1, maxP));
				}
			}

			if (SectionHeader(ICON_FA_UP_DOWN_LEFT_RIGHT "  Shape & motion", ImGuiTreeNodeFlags_DefaultOpen))
			{
				// Dial on the left, direction/spread numerics on the right.
				const ImVec2 dialTop = ImGui::GetCursorPos();
				DirectionDial(&m_config.directionDeg, m_config.spreadDeg);
				ImGui::SameLine();
				ImGui::BeginGroup();
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##dir", &m_config.directionDeg, 0.0f, 360.0f, "dir %.0f\xc2\xb0");
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##spread", &m_config.spreadDeg, 0.0f, 180.0f, "spread %.0f\xc2\xb0");
				ImGui::EndGroup();
				(void) dialTop;

				RangeRow("Lifetime (s)", &m_config.lifetimeMin, &m_config.lifetimeMax, 0.02f, 0.01f, 60.0f, "%.2f");
				RangeRow("Speed", &m_config.speedMin, &m_config.speedMax, 0.1f, 0.0f, 100.0f, "%.2f");
				PropDrag2("Gravity", &m_config.gravity.x, 0.1f, -100.0f, 100.0f, "%.1f");
			}

			if (SectionHeader(ICON_FA_PALETTE "  Appearance", ImGuiTreeNodeFlags_DefaultOpen))
			{
				iw::PropInputText("Texture", m_config.texturePath, "empty = white dot");
				if (chrome::OutlineButton("Dot"))
				{
					m_config.texturePath = kDot;
				}
				ImGui::SameLine();
				if (chrome::OutlineButton("Spark"))
				{
					m_config.texturePath = kSpark;
				}
				ImGui::SameLine();
				if (chrome::OutlineButton("None"))
				{
					m_config.texturePath.clear();
				}

				RangeRow("Size (start-end)", &m_config.startSize, &m_config.endSize, 0.02f, 0.0f, 20.0f, "%.2f");

				PropColor4("Start color", &m_config.startColor.x);
				PropColor4("End color", &m_config.endColor.x);
				PropLabel("Ramp");
				GradientBar(m_config.startColor, m_config.endColor, 16.0f);

				int blend = static_cast<int>(m_config.blendMode);
				if (PropComboStr("Blend", &blend, "Alpha\0Additive\0Multiply\0Opaque\0"))
				{
					m_config.blendMode = static_cast<SpriteBlendMode>(std::clamp(blend, 0, 3));
				}
				PropInt("Sorting layer", &m_config.sortingLayer, 1.0f, -100000, 100000);
			}

			if (SectionHeader(ICON_FA_WEIGHT_HANGING "  Collision"))
			{
				PropCheckbox("World", &m_config.collideWorld, "Bounce off physics colliders (in-scene only)");
				PropCheckbox("Each other", &m_config.collideParticles, "Particles bounce off one another");
				PropFloat("Bounce", &m_config.bounce, 0.02f, 0.0f, 1.0f, "%.2f");
				PropFloat("Damping", &m_config.collisionDamping, 0.02f, 0.0f, 1.0f, "%.2f", "Tangential speed lost on a world hit");
				PropFloat("Collision radius", &m_config.collisionRadius, 0.01f, 0.0f, 10.0f, "%.2f", "0 = derive from particle size");
				ImGui::TextColored(chrome::kMuted, "World collision only runs in a live scene, not this preview.");
			}

			// ── Commit ────────────────────────────────────────────────────────
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
			if (chrome::PrimaryButton(ICON_FA_PLUS "  Create Entity", ImVec2(half, 0.0f)))
			{
				const glm::vec2 focus = CameraFocus(world);
				const Entity e = world.Create();
				world.Emplace<NameComponent>(e, NameComponent{.name = "Particle Emitter"});
				TransformComponent tc;
				tc.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(focus.x, focus.y, 0.0f));
				world.Emplace<TransformComponent>(e, tc);
				world.Emplace<ParticleEmitterComponent>(e, m_config);
				context.Get<SceneSelection>().Select(e);
				// An entity appearing in the scene is an edit like any other; without this the
				// next Ctrl+Z reached past it.
				if (auto* undo = context.services.TryGet<UndoStack>())
				{
					if (auto command = SubtreeLifetimeCommand::Capture(world, context.services, {e}, /*createdByThisEdit=*/true, "Create Emitter"))
					{
						undo->Record(std::move(command));
					}
				}
				m_status = "Created emitter entity at the view centre.";
				m_statusError = false;
			}
			ImGui::SameLine();
			auto& selection = context.Get<SceneSelection>();
			const Entity selected = selection.Primary();
			const bool hasSelection = selected.IsValid() && world.GetRegistry().valid(World::ToEntt(selected));
			ImGui::BeginDisabled(!hasSelection);
			if (chrome::OutlineButton(ICON_FA_LINK "  Apply to Selected", ImVec2(half, 0.0f)) && hasSelection)
			{
				// Overwrites whatever the entity's emitter held, so the previous config has to
				// be recoverable - as an add when there was no emitter, or a before/after pair
				// when there was.
				auto* applyUndo = context.services.TryGet<UndoStack>();
				const bool hadEmitter = world.Has<ParticleEmitterComponent>(selected);
				nlohmann::json emitterBefore;
				bool emitterReflected = false;
				if (hadEmitter && applyUndo != nullptr)
				{
					CaptureComponentFields(world, selected, "Particle Emitter", context.services, emitterBefore, emitterReflected);
				}
				world.EmplaceOrReplace<ParticleEmitterComponent>(selected, m_config);
				if (applyUndo != nullptr)
				{
					if (!hadEmitter)
					{
						applyUndo->Record(std::make_unique<AddComponentCommand>(selected.id, "Particle Emitter"));
					}
					else
					{
						nlohmann::json emitterAfter;
						bool afterReflected = false;
						if (CaptureComponentFields(world, selected, "Particle Emitter", context.services, emitterAfter, afterReflected) && emitterAfter != emitterBefore)
						{
							applyUndo->Record(std::make_unique<SetComponentCommand>(selected.id, "Particle Emitter", std::move(emitterBefore), std::move(emitterAfter), afterReflected));
						}
					}
				}
				m_status = "Applied config to the selected entity.";
				m_statusError = false;
			}
			ImGui::EndDisabled();

			if (!m_status.empty())
			{
				ImGui::TextColored(m_statusError ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f) : chrome::kMuted, "%s", m_status.c_str());
			}
		}
		ImGui::End();
	}
} // namespace aether::editor
