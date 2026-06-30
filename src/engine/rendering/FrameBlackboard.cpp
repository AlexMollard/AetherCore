#include "rendering/FrameBlackboard.hpp"

#include <algorithm>

#include "utils/Logger.hpp"

namespace aether
{
	std::any& FrameBlackboard::CreateStorage(std::type_index type, std::string_view typeName, std::string name, std::any value, ProductMetadata metadata, bool allowReplace)
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt != m_keys.end())
		{
			const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
			ProductEntry& entry = m_products[index];
			if (!allowReplace)
			{
				AE_WARN(LogCategory::Engine, "FrameBlackboard: product '{}' of type '{}' already exists; keeping the original producer.", entry.name, entry.typeName);
				return entry.value;
			}

			entry.value = std::move(value);
			entry.metadata = metadata;
			return entry.value;
		}

		m_keys.push_back(key);
		ProductEntry& entry = m_products.emplace_back();
		entry.type = type;
		entry.name = std::move(name);
		entry.typeName = std::string{typeName};
		entry.value = std::move(value);
		entry.metadata = metadata;
		return entry.value;
	}

	std::any* FrameBlackboard::TryGetStorage(std::type_index type, std::string_view name)
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt == m_keys.end())
		{
			return nullptr;
		}

		const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
		return &m_products[index].value;
	}

	const std::any* FrameBlackboard::TryGetStorage(std::type_index type, std::string_view name) const
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt == m_keys.end())
		{
			return nullptr;
		}

		const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
		return &m_products[index].value;
	}

	void FrameBlackboard::RemoveStorage(std::type_index type, std::string_view name)
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt == m_keys.end())
		{
			return;
		}

		const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
		m_keys.erase(m_keys.begin() + static_cast<std::ptrdiff_t>(index));
		m_products.erase(m_products.begin() + static_cast<std::ptrdiff_t>(index));
	}

	void FrameBlackboard::MarkProduced(std::type_index type, std::string_view name, std::string_view passName)
	{
		MarkProducer(type, name, passName, ProductSource::GraphPass);
	}

	void FrameBlackboard::MarkProducer(std::type_index type, std::string_view name, std::string_view producerName, ProductSource source)
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt == m_keys.end())
		{
			AE_WARN(LogCategory::Engine, "FrameBlackboard: producer '{}' tried to produce missing product '{}'.", producerName, name);
			return;
		}

		const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
		ProductEntry& entry = m_products[index];
		if (!entry.producerPass.empty() && entry.producerPass != producerName)
		{
			AE_WARN(LogCategory::Engine, "FrameBlackboard: product '{}' of type '{}' already has producer '{}'; replacing with '{}'.", entry.name, entry.typeName, entry.producerPass, producerName);
		}
		if (entry.metadata.source != source && entry.metadata.source == ProductSource::GraphPass && source != ProductSource::GraphPass)
		{
			AE_WARN(LogCategory::Engine, "FrameBlackboard: product '{}' of type '{}' changed from graph-produced to external setup source.", entry.name, entry.typeName);
		}
		else if (entry.metadata.source != source && entry.metadata.source != ProductSource::GraphPass && source == ProductSource::GraphPass)
		{
			AE_WARN(LogCategory::Engine, "FrameBlackboard: product '{}' of type '{}' changed from external setup source to graph-produced.", entry.name, entry.typeName);
			return;
		}
		entry.metadata.source = source;
		entry.producerPass = std::string{producerName};
	}

	void FrameBlackboard::MarkConsumed(std::type_index type, std::string_view name, std::string_view passName)
	{
		const std::string key = MakeKey(type, name);
		const auto keyIt = std::ranges::find(m_keys, key);
		if (keyIt == m_keys.end())
		{
			AE_WARN(LogCategory::Engine, "FrameBlackboard: pass '{}' tried to consume missing product '{}'.", passName, name);
			return;
		}

		const std::size_t index = static_cast<std::size_t>(std::distance(m_keys.begin(), keyIt));
		ProductEntry& entry = m_products[index];
		if (std::ranges::find(entry.consumerPasses, passName) == entry.consumerPasses.end())
		{
			entry.consumerPasses.emplace_back(passName);
		}
	}

	void FrameBlackboard::Clear()
	{
		m_products.clear();
		m_keys.clear();
	}

	std::vector<FrameBlackboard::ProductInfo> FrameBlackboard::GetProducts() const
	{
		std::vector<ProductInfo> products;
		products.reserve(m_products.size());
		for (const ProductEntry& entry: m_products)
		{
			products.push_back({
			        .name = entry.name,
			        .typeName = entry.typeName,
			        .producerPass = entry.producerPass,
			        .consumerPasses = entry.consumerPasses,
			        .metadata = entry.metadata,
			});
		}
		return products;
	}

	std::string FrameBlackboard::MakeKey(std::type_index type, std::string_view name)
	{
		std::string key = std::to_string(type.hash_code());
		key.push_back(':');
		key.append(name);
		return key;
	}
} // namespace aether
