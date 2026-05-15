#include "SystemFactory.hpp"

#include "utils/Logger.hpp"

namespace aether::app
{
	void SystemFactory::Register(std::string name, Factory factory)
	{
		m_factories.emplace(std::move(name), std::move(factory));
	}

	std::unique_ptr<aether::System> SystemFactory::Create(std::string_view name) const
	{
		auto it = m_factories.find(std::string(name));
		if (it == m_factories.end())
		{
			WARN(LogCategory::App, "SystemFactory: unknown system '{}'", name);
			return nullptr;
		}
		return it->second();
	}

	bool SystemFactory::Has(std::string_view name) const
	{
		return m_factories.contains(std::string(name));
	}
} // namespace aether::app
