#pragma once

#include <cassert>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

#include "utils/Assert.hpp"

namespace aether
{
	// Type-erased service locator / dependency injection container.
	//
	// Thread safety: NOT thread-safe. All registrations must occur during
	// initialization (single-threaded). Reads during the frame loop are safe
	// as long as no concurrent modifications happen. Use external synchronization
	// if Register/Unregister/Clear are called concurrently with Get/TryGet/Has.
class ServiceContainer
{
public:
	template<typename T>
	void Register(T& service)
	{
		m_services[std::type_index(typeid(T))] = &service;
	}

	template<typename T>
	[[nodiscard]] T& Get() const
	{
		auto it = m_services.find(std::type_index(typeid(T)));
		AE_ASSERT_ALWAYS(it != m_services.end(),
			std::string("Service not registered: ") + typeid(T).name());
		return *static_cast<T*>(it->second);
	}

	template<typename T>
	[[nodiscard]] T* TryGet() const
	{
		auto it = m_services.find(std::type_index(typeid(T)));
		return it != m_services.end() ? static_cast<T*>(it->second) : nullptr;
	}

	template<typename T>
	[[nodiscard]] bool Has() const
	{
		return m_services.contains(std::type_index(typeid(T)));
	}

	template<typename T>
	void Unregister()
	{
		m_services.erase(std::type_index(typeid(T)));
	}

	void Clear()
	{
		m_services.clear();
	}

private:
	std::unordered_map<std::type_index, void*> m_services;
};
} // namespace aether
