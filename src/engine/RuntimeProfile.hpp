#pragma once

namespace aether
{
	// Selects how much of the engine an AetherCore instance brings up.
	//
	// Full    - the complete runtime: ECS scene, cameras, lighting, animation, and
	//           the scene render passes (cull, shadows, GTAO, forward, post-process).
	//           Used by the editor (Editor) and the shipped game (GameRuntime).
	//
	// UiShell - a minimal tool front-end runtime: window + Vulkan device/swapchain +
	//           the Dear ImGui overlay + a render graph that only clears the swapchain
	//           each frame so the overlay presents over a defined background. No ECS
	//           scene systems, cameras, lighting, or animation are created and no
	//           scene GPU resources (shadow atlases, post-process, GTAO, pipelines)
	//           are allocated. Used by the project Launcher (a Unity-Hub-style front
	//           end that needs GLFW + ImGui and nothing else). See the multiprocess
	//           launcher design in docs/superpowers/specs/.
	enum class RuntimeProfile
	{
		Full,
		UiShell,
	};
} // namespace aether
