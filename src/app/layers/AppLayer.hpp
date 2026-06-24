#pragma once

#include <cstdint>

#include "utils/ServiceContainer.hpp"

namespace aether::app
{
	struct LayerContext
	{
		ServiceContainer& services;
		double deltaTimeSeconds = 0.0;
		double elapsedTimeSeconds = 0.0;
		std::uint64_t frameIndex = 0;

		template<typename T>
		[[nodiscard]] T& Get() const
		{
			return services.Get<T>();
		}

		template<typename T>
		[[nodiscard]] T* TryGet() const
		{
			return services.TryGet<T>();
		}
	};

	class AppLayer
	{
	public:
		virtual ~AppLayer() = default;

		virtual void OnAttach(LayerContext& context);
		virtual void OnDetach(LayerContext& context);
		virtual void OnUpdate(LayerContext& context);
		virtual void OnGui(LayerContext& context);
	};
} // namespace aether::app
