#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "scene/System.hpp"

namespace aether::app
{
	// Maps string names to factory functions that produce System instances.
	// Scripts activate C++ systems by name via register_system("FoxSystem").
	class SystemFactory
	{
	public:
		using Factory = std::function<std::unique_ptr<aether::System>()>;

		void Register(std::string name, Factory factory);

		// Returns nullptr if name is unknown.
		[[nodiscard]] std::unique_ptr<aether::System> Create(std::string_view name) const;

		[[nodiscard]] bool Has(std::string_view name) const;

	private:
		std::unordered_map<std::string, Factory> m_factories;
	};
} // namespace aether::app
