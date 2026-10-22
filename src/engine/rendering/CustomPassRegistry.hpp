#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	// Game-thread registry + submission queue for project-registered custom render passes. A project
	// registers a pass once (name -> shader + stage), then submits its data buffer + params each frame;
	// PrepareFrame snapshots the frame's submissions into the render packet (RenderCustomPassData) and
	// clears them. Both the script update and the extraction run on the game/producer thread in
	// sequence, so no locking is needed.
	//
	// Registered as a service so the app-side interop and the engine-side extraction share one instance.
	// The engine stays entirely game-agnostic: it only knows "run this project shader over a fullscreen
	// pass with this float4 buffer bound" - what the pass means lives in the project's shader + scripts.
	struct CustomPassRegistry
	{
		struct Registration
		{
			std::string shader;                                // project shader stem
			CustomPassStage stage = CustomPassStage::OverScene2D;
		};

		std::unordered_map<std::string, Registration> registered; // pass name -> registration
		RenderCustomPassData frame;                               // submissions this frame (cleared after extract)

		// Caps the per-pass buffer so a runaway script can't blow up the fullscreen shader's work.
		static constexpr std::size_t kMaxVec4 = 4096;

		void Register(std::string name, std::string shader, CustomPassStage stage)
		{
			registered[std::move(name)] = {std::move(shader), stage};
		}

		void Unregister(const std::string& name)
		{
			registered.erase(name);
		}

		// Appends this frame's submission; submitting to an unregistered name is a no-op (false).
		// `at(i)` yields element i of `count` source float4s; elements beyond kMaxVec4 are dropped.
		template <typename At>
		bool SubmitIndexed(const std::string& name, std::size_t count, At&& at, const glm::vec4& params, const glm::vec4& color0, const glm::vec4& color1)
		{
			const auto it = registered.find(name);
			if (it == registered.end())
			{
				return false;
			}
			CustomPassDraw draw;
			draw.shader = it->second.shader;
			draw.stage = it->second.stage;
			draw.params = params;
			draw.color0 = color0;
			draw.color1 = color1;
			count = std::min(count, kMaxVec4);
			draw.data.resize(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				draw.data[i] = at(i);
			}
			frame.passes.push_back(std::move(draw));
			return true;
		}
	};
} // namespace aether
