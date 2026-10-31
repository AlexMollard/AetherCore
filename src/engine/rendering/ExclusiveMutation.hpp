#pragma once

#include <utility>

namespace aether
{
	// The mutation must retain usable published state on failure. The guard also
	// covers failures while parking or waiting for GPU idle.
	template<typename RenderState, typename WaitIdle, typename Mutation>
	void RunPausedMutation(RenderState& renderState, WaitIdle&& waitIdle, Mutation&& mutation)
	{
		renderState.SetReloadInProgress(true);
		struct Resume
		{
			RenderState& state;
			~Resume() { state.SetReloadInProgress(false); }
		} resume{renderState};
		renderState.WaitPaused();
		std::forward<WaitIdle>(waitIdle)();
		std::forward<Mutation>(mutation)();
	}
}
