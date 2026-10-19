#pragma once

#include <filesystem>
#include <thread>

#include "scripting/GameplayServices.h"

namespace aether::scripting
{
	class GameplayAccess;
}

namespace aether::app::scripting
{
	struct SceneContext;

	// Owner-thread services. Registry tokens invalidate copied API tables on teardown.
	class RustGameplayServices
	{
	public:
		RustGameplayServices(SceneContext& context, aether::scripting::GameplayAccess& access);
		~RustGameplayServices();
		RustGameplayServices(const RustGameplayServices&) = delete;
		RustGameplayServices& operator=(const RustGameplayServices&) = delete;
		[[nodiscard]] AetherGameplayServicesApi EngineApi() const noexcept;

	private:
		static AetherGameplayStatus Invoke(uint64_t context, const AetherGameplayServiceRequest* request, AetherGameplayPropertyValue* outValue, AetherGameplayUtf8Buffer stringBuffer) noexcept;
		AetherGameplayStatus Execute(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& outValue, AetherGameplayUtf8Buffer stringBuffer);
		SceneContext& m_context;
		aether::scripting::GameplayAccess& m_access;
		uint64_t m_token;
		std::thread::id m_owner;
		bool m_invoking = false;
		std::filesystem::path m_save_directory;
	};
} // namespace aether::app::scripting
