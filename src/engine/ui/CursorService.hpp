#pragma once

#include <string>
#include <glm/glm.hpp>

namespace aether::ui
{
	/// The engine's mouse pointer.
	///
	/// A game that wants its own pointer used to have to hand-roll one: hide the OS cursor, spawn a UI
	/// image, keep it alive across scene loads, and keep it sorted above every other canvas. That is
	/// engine work, not game work - every project needs the same thing and there is exactly one right
	/// answer - so it lives here, and a project only configures it.
	///
	/// Configuration comes from settings (see EngineSettings::Cursor), so a project turns it on and
	/// names its art in ProjectSettings.toml without writing any code. Games that want more than one
	/// pointer - a pen while drawing, an arrow in menus - push a runtime override on top and drop it
	/// again with Reset().
	///
	/// Drawn by UiRenderer as the very last command of the frame, so it is over everything, needs no
	/// entity and cannot be outlived by a scene load.
	class CursorService
	{
	public:
		struct Look
		{
			std::string texture;              // VFS path; empty = nothing to draw
			glm::vec2 hotspot{0.0f, 0.0f};    // 0..1 within the image: the pixel that sits on the mouse
			float size = 32.0f;               // on-screen square size in pixels
			bool pixelArt = true;             // nearest sampling, so small art scales up crisp
		};

		// --- configuration (settings-driven; a project sets these, not game code) ----------------

		void Configure(bool enabled, const Look& look)
		{
			m_enabled = enabled;
			m_configured = look;
			if (!m_overridden)
			{
				m_active = look;
			}
		}

		/// Whether this project draws its own pointer at all. When off the engine leaves the OS cursor
		/// alone and draws nothing, which is what a tool front-end (and the editor) wants.
		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		// --- runtime state (game code) ------------------------------------------------------------

		/// Swap the pointer's look. Stays until Reset().
		void SetLook(const Look& look)
		{
			m_active = look;
			m_overridden = true;
		}

		/// Back to whatever the project configured.
		void Reset()
		{
			m_active = m_configured;
			m_overridden = false;
		}

		/// Hide the pointer without disabling the system - for the stretch of a game that draws its own
		/// marker at the cursor (an aiming reticle, a brush preview) and would otherwise draw two.
		void SetVisible(bool visible)
		{
			m_visible = visible;
		}

		[[nodiscard]] bool IsVisible() const
		{
			return m_visible;
		}

		/// Hand the pointer back for a while without changing what the project configured. The editor
		/// uses this the moment the mouse leaves the game view: its own panels need the real OS pointer,
		/// and a game's cursor has no business being drawn over them.
		void SetSuppressed(bool suppressed)
		{
			m_suppressed = suppressed;
		}

		/// Fed once per frame from Input, in the same pixel space the UI lays out in.
		void SetPosition(glm::vec2 positionPx)
		{
			m_position = positionPx;
		}

		[[nodiscard]] glm::vec2 GetPosition() const
		{
			return m_position;
		}

		[[nodiscard]] const Look& GetLook() const
		{
			return m_active;
		}

		/// Everything that has to be true for there to be a pointer on screen this frame.
		[[nodiscard]] bool ShouldDraw() const
		{
			return m_enabled && !m_suppressed && m_visible && !m_active.texture.empty() && m_active.size > 0.0f;
		}

		/// Whether the OS pointer should currently be painted. False as soon as the project has taken
		/// the job over, whether or not it is showing a pointer this instant - a cursor that flickers
		/// back to the desktop arrow every time the game hides its own is worse than either alone.
		[[nodiscard]] bool WantsOsCursor() const
		{
			return !m_enabled || m_suppressed;
		}

	private:
		bool m_enabled = false;
		bool m_suppressed = false;
		bool m_visible = true;
		bool m_overridden = false;
		glm::vec2 m_position{0.0f};
		Look m_configured;
		Look m_active;
	};
} // namespace aether::ui
