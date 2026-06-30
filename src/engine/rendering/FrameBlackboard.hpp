#pragma once

#include <any>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Assert.hpp"

namespace aether
{
	// Typed registry for higher-level frame products that are not Vulkan
	// resources themselves, but still need explicit producer/consumer contracts.
	class FrameBlackboard
	{
	public:
		enum class ProductSource : std::uint8_t
		{
			Imported,
			FrameSetup,
			GraphPass,
		};

		struct ProductMetadata
		{
			std::uint32_t frameSlot = UINT32_MAX;
			std::optional<gpu::Extent2D> extent;
			gpu::Format format = gpu::Format::Undefined;
			std::uint32_t bindlessSlot = UINT32_MAX;
			bool historyValid = false;
			ProductSource source = ProductSource::GraphPass;
		};

		struct ProductInfo
		{
			std::string name;
			std::string typeName;
			std::string producerPass;
			std::vector<std::string> consumerPasses;
			ProductMetadata metadata;
		};

		template<typename T>
		[[nodiscard]] T& Create(std::string name, T value = {}, ProductMetadata metadata = {})
		{
			std::any& storage = CreateStorage(std::type_index(typeid(T)), typeid(T).name(), std::move(name), std::any{std::move(value)}, metadata, false);
			T* product = std::any_cast<T>(&storage);
			AE_ASSERT_ALWAYS(product != nullptr, "FrameBlackboard product storage type mismatch.");
			return *product;
		}

		template<typename T>
		[[nodiscard]] T& CreateOrReplace(std::string name, T value = {}, ProductMetadata metadata = {})
		{
			std::any& storage = CreateStorage(std::type_index(typeid(T)), typeid(T).name(), std::move(name), std::any{std::move(value)}, metadata, true);
			T* product = std::any_cast<T>(&storage);
			AE_ASSERT_ALWAYS(product != nullptr, "FrameBlackboard product storage type mismatch.");
			return *product;
		}

		template<typename T>
		[[nodiscard]] T& Import(std::string name, T value = {}, ProductMetadata metadata = {}, std::string_view producerName = "Imported")
		{
			metadata.source = ProductSource::Imported;
			T& product = CreateOrReplace<T>(name, std::move(value), metadata);
			MarkProducer(std::type_index(typeid(T)), name, producerName, ProductSource::Imported);
			return product;
		}

		template<typename T>
		[[nodiscard]] T& SetFrameProduct(std::string name, T value = {}, ProductMetadata metadata = {}, std::string_view producerName = "FrameSetup")
		{
			metadata.source = ProductSource::FrameSetup;
			T& product = CreateOrReplace<T>(name, std::move(value), metadata);
			MarkProducer(std::type_index(typeid(T)), name, producerName, ProductSource::FrameSetup);
			return product;
		}

		template<typename T>
		[[nodiscard]] T& DeclareGraphProduct(std::string name, T value = {}, ProductMetadata metadata = {})
		{
			metadata.source = ProductSource::GraphPass;
			return CreateOrReplace<T>(std::move(name), std::move(value), metadata);
		}

		template<typename T>
		[[nodiscard]] T* TryGet(std::string_view name)
		{
			return std::any_cast<T>(TryGetStorage(std::type_index(typeid(T)), name));
		}

		template<typename T>
		[[nodiscard]] const T* TryGet(std::string_view name) const
		{
			return std::any_cast<T>(TryGetStorage(std::type_index(typeid(T)), name));
		}

		template<typename T>
		[[nodiscard]] T& Require(std::string_view name)
		{
			T* product = TryGet<T>(name);
			AE_ASSERT_ALWAYS(product != nullptr, "Required frame blackboard product is missing.");
			return *product;
		}

		template<typename T>
		[[nodiscard]] const T& Require(std::string_view name) const
		{
			const T* product = TryGet<T>(name);
			AE_ASSERT_ALWAYS(product != nullptr, "Required frame blackboard product is missing.");
			return *product;
		}

		template<typename T>
		void Remove(std::string_view name)
		{
			RemoveStorage(std::type_index(typeid(T)), name);
		}

		template<typename T>
		void MarkProduced(std::string_view name, std::string_view passName)
		{
			MarkProduced(std::type_index(typeid(T)), name, passName);
		}

		template<typename T>
		void MarkConsumed(std::string_view name, std::string_view passName)
		{
			MarkConsumed(std::type_index(typeid(T)), name, passName);
		}

		void MarkProduced(std::type_index type, std::string_view name, std::string_view passName);
		void MarkConsumed(std::type_index type, std::string_view name, std::string_view passName);

		void Clear();

		[[nodiscard]] std::vector<ProductInfo> GetProducts() const;

	private:
		struct ProductEntry
		{
			std::type_index type = std::type_index(typeid(void));
			std::string name;
			std::string typeName;
			std::any value;
			ProductMetadata metadata;
			std::string producerPass;
			std::vector<std::string> consumerPasses;
		};

		[[nodiscard]] std::any& CreateStorage(std::type_index type, std::string_view typeName, std::string name, std::any value, ProductMetadata metadata, bool allowReplace);
		[[nodiscard]] ProductEntry& EnsureContractStorage(std::type_index type, std::string_view typeName, std::string name);
		[[nodiscard]] std::any* TryGetStorage(std::type_index type, std::string_view name);
		[[nodiscard]] const std::any* TryGetStorage(std::type_index type, std::string_view name) const;
		void RemoveStorage(std::type_index type, std::string_view name);
		void MarkProducer(std::type_index type, std::string_view name, std::string_view producerName, ProductSource source);

		[[nodiscard]] static std::string MakeKey(std::type_index type, std::string_view name);

		std::vector<ProductEntry> m_products;
		std::vector<std::string> m_keys;
	};
} // namespace aether
