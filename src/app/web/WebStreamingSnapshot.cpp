#include "WebStreamingSnapshot.hpp"
#include "WebScene.hpp"

#include <bit>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <toml++/toml.hpp>
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/SceneStreamingReferences.hpp"

namespace aether::app::web
{
	namespace
	{
		struct Writer
		{
			std::string data;
			bool ok = true;

			void Number(std::uint64_t value, unsigned bytes)
			{
				if (data.size() > kStreamingScriptSnapshotLimit - bytes)
				{
					ok = false;
					return;
				}
				for (unsigned i = 0; i < bytes; ++i)
				{
					data.push_back(static_cast<char>(value >> (8 * i)));
				}
			}

			void String(std::string_view value)
			{
				if (value.size() > kStreamingScriptSnapshotLimit - 4 || data.size() > kStreamingScriptSnapshotLimit - 4 - value.size())
				{
					ok = false;
					return;
				}
				Number(value.size(), 4);
				data.append(value);
			}
		};

		struct Reader
		{
			std::string_view data;
			std::size_t pos = 0;
			bool ok = true;

			std::uint64_t Number(unsigned bytes)
			{
				if (bytes > data.size() - pos)
				{
					ok = false;
					return 0;
				}
				std::uint64_t value = 0;
				for (unsigned i = 0; i < bytes; ++i)
				{
					value |= std::uint64_t(static_cast<unsigned char>(data[pos++])) << (8 * i);
				}
				return value;
			}

			std::string String()
			{
				const auto size = Number(4);
				if (!ok || size > data.size() - pos)
				{
					ok = false;
					return {};
				}
				std::string value(data.substr(pos, static_cast<std::size_t>(size)));
				pos += static_cast<std::size_t>(size);
				return value;
			}
		};

		AetherError Invalid()
		{
			return AetherError::Engine("Invalid or oversized browser streaming script snapshot");
		}
	} // namespace

	Expected<std::uint64_t> ComputeRegionSnapshotWorkspace(const RegionSnapshotStorage& storage, std::uint64_t scriptBytes, std::uint64_t runtimeEntryBytes)
	{
		std::uint64_t result = 0;
		const auto add = [&](std::uint64_t value) -> bool
		{
			if (value > std::numeric_limits<std::uint64_t>::max() - result)
			{
				return false;
			}
			result += value;
			return true;
		};
		const auto product = [](std::uint64_t a, std::uint64_t b, std::uint64_t& out) -> bool
		{
			if (b && a > std::numeric_limits<std::uint64_t>::max() / b)
			{
				return false;
			}
			out = a * b;
			return true;
		};
		std::uint64_t escaped = 0, scalarText = 0, text = 0, tree = 0, runtime = 0;
		// Each logical node is conservatively charged as one table, array and
		// scalar value plus a named entry, even though actual nodes are only one.
		constexpr std::uint64_t nodeBytes = sizeof(toml::table) + sizeof(toml::array) + sizeof(toml::value<std::string>) + sizeof(std::pair<const std::string, std::unique_ptr<toml::node>>);
		if (!product(storage.stringBytes, 6, escaped) || !product(storage.logicalNodes, 64, scalarText) || escaped > std::numeric_limits<std::uint64_t>::max() - scalarText || !product(storage.logicalNodes, nodeBytes, tree)
		        || !product(storage.entityCount, runtimeEntryBytes, runtime))
		{
			return std::unexpected(Invalid());
		}
		text = escaped + scalarText;
		// Charge two logical slots per geometrically grown container, including
		// the output stream, returned TOML and final-frame copy.
		if (!add(storage.recordBytes) || !add(storage.recordBytes) || !add(storage.stringBytes) || !add(storage.stringBytes) || !add(storage.stringBytes) || !add(storage.stringBytes) || !add(tree) || !add(tree) || !add(text) || !add(text)
		        || !add(text) || !add(text) || !add(text) || !add(text) || !add(runtime) || !add(runtime) || !add(runtime) || !add(runtime) || !add(scriptBytes) || !add(scriptBytes) || !add(4 * sizeof(std::uint32_t) + sizeof(std::uint64_t)))
		{
			return std::unexpected(Invalid());
		}
		return result;
	}

	Expected<StreamingScriptCaptureBudget> MeasureStreamingScriptCapture(scripting::GameplayHost& host, std::span<const StreamingScriptInstance> instances)
	{
		if (instances.size() > 65536)
		{
			return std::unexpected(Invalid());
		}
		std::uint64_t largestProperty = 0;
		std::uint64_t instanceBytes = 0;
		for (const auto& instance: instances)
		{
			if (!instance.handle || instance.type.size() > kStreamingScriptSnapshotLimit)
			{
				return std::unexpected(Invalid());
			}
			const auto instanceCost = static_cast<std::uint64_t>(sizeof(StreamingScriptInstance)) + instance.type.size() + 1;
			if (instanceBytes > std::numeric_limits<std::uint64_t>::max() - instanceCost)
			{
				return std::unexpected(Invalid());
			}
			instanceBytes += instanceCost;
			const auto properties = host.BorrowScriptProperties(instance.type);
			if (properties.size() > 65536)
			{
				return std::unexpected(Invalid());
			}
			for (std::size_t i = 0; i < properties.size(); ++i)
			{
				if (properties[i].name.size() > kStreamingScriptSnapshotLimit)
				{
					return std::unexpected(Invalid());
				}
				auto required = host.GetPropertyRequiredBytes(instance.handle, static_cast<int>(i));
				if (!required)
				{
					return std::unexpected(required.error());
				}
				if (*required > kStreamingScriptSnapshotLimit)
				{
					return std::unexpected(Invalid());
				}
				largestProperty = std::max(largestProperty, static_cast<std::uint64_t>(*required));
			}
		}
		const std::uint64_t fixed = instances.empty() ? 2 * sizeof(std::uint32_t) : kStreamingScriptSnapshotLimit + 2ull * kStreamingScriptCaptureScratch;
		if (instanceBytes > std::numeric_limits<std::uint64_t>::max() - fixed || largestProperty > (std::numeric_limits<std::uint64_t>::max() - fixed - instanceBytes) / 2)
		{
			return std::unexpected(Invalid());
		}
		return StreamingScriptCaptureBudget{fixed + instanceBytes + 2 * largestProperty, static_cast<std::uint32_t>(largestProperty)};
	}

	Expected<std::string> CaptureStreamingScripts(World& world, scripting::GameplayHost& host, const std::vector<StreamingScriptInstance>& instances, std::span<const std::string> states, std::uint32_t maxPropertyBytes)
	{
		if (instances.size() > 65536 || states.size() != instances.size())
		{
			return std::unexpected(Invalid());
		}
		Writer out;
		out.Number(0x31535357, 4);
		out.Number(instances.size(), 4);
		for (std::size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
		{
			const auto& instance = instances[instanceIndex];
			const auto* node = world.TryGet<SceneNodeComponent>(instance.entity);
			if (!node || !node->id || !instance.handle)
			{
				return std::unexpected(Invalid());
			}
			out.Number(node->id, 8);
			out.Number(instance.index, 4);
			out.String(instance.type);
			const auto properties = host.BorrowScriptProperties(instance.type);
			if (properties.size() > 65536)
			{
				return std::unexpected(Invalid());
			}
			out.Number(properties.size(), 4);
			for (std::size_t i = 0; i < properties.size(); ++i)
			{
				ScriptPropertyValue value;
				auto required = host.GetPropertyRequiredBytes(instance.handle, static_cast<int>(i));
				if (!required)
				{
					return std::unexpected(required.error());
				}
				if (*required > maxPropertyBytes || !host.GetPropertyValueBounded(instance.handle, static_cast<int>(i), maxPropertyBytes, value))
				{
					return std::unexpected(Invalid());
				}
				out.String(properties[i].name);
				out.Number(static_cast<std::uint32_t>(value.type), 4);
				for (float f: value.f4)
				{
					out.Number(std::bit_cast<std::uint32_t>(f), 4);
				}
				std::uint64_t integer = static_cast<std::uint64_t>(value.i64);
				if ((value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component) && value.i64 != 0)
				{
					if (value.i64 < 0 || static_cast<std::uint64_t>(value.i64) > UINT32_MAX)
					{
						return std::unexpected(Invalid());
					}
					const Entity target{static_cast<std::uint32_t>(value.i64)};
					if (!world.GetRegistry().valid(World::ToEntt(target)))
					{
						return std::unexpected(Invalid());
					}
					auto* targetNode = world.TryGet<SceneNodeComponent>(target);
					if (!targetNode)
					{
						targetNode = &world.Emplace<SceneNodeComponent>(target, SceneNodeComponent{scene::GenerateSceneNodeId()});
					}
					if (!targetNode->id)
					{
						targetNode->id = scene::GenerateSceneNodeId();
					}
					integer = targetNode->id;
				}
				else if (value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component)
				{
					if (const auto* pending = world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>())
					{
						for (const auto& reference: pending->entries)
						{
							if (reference.ownerNode == node->id && reference.scriptIndex == instance.index
							    && reference.scriptType == instance.type && reference.property == properties[i].name && reference.expected == value.i64)
							{
								integer = reference.targetNode;
								break;
							}
						}
					}
				}
				out.Number(integer, 8);
				out.String(value.str);
				if (!out.ok)
				{
					return std::unexpected(Invalid());
				}
			}
			if (states[instanceIndex].size() >= kStreamingScriptCaptureScratch)
			{
				return std::unexpected(Invalid());
			}
			out.String(states[instanceIndex]);
			if (!out.ok)
			{
				return std::unexpected(Invalid());
			}
		}
		return std::move(out.data);
	}

	Expected<StreamingScriptRestoreCursor> PrepareStreamingScriptRestore(World& world, scripting::GameplayHost& host, const std::vector<StreamingScriptInstance>& instances, std::string_view data)
	{
		if (data.size() > kStreamingScriptSnapshotLimit)
		{
			return std::unexpected(Invalid());
		}
		Reader in{data};
		if (in.Number(4) != 0x31535357)
		{
			return std::unexpected(Invalid());
		}
		const auto count = in.Number(4);
		if (!in.ok || count > 65536 || count != instances.size())
		{
			return std::unexpected(Invalid());
		}
		StreamingScriptRestoreCursor cursor;
		auto& scripts = cursor.scripts;
		scripts.reserve(static_cast<std::size_t>(count));
		for (std::uint64_t i = 0; i < count; ++i)
		{
			StreamingSavedScript script{};
			script.node = in.Number(8);
			script.index = static_cast<std::uint32_t>(in.Number(4));
			script.type = in.String();
			const auto propertyCount = in.Number(4);
			if (!in.ok || !script.node || propertyCount > 65536)
			{
				return std::unexpected(Invalid());
			}
			for (std::uint64_t p = 0; p < propertyCount; ++p)
			{
				StreamingSavedProperty property;
				property.name = in.String();
				const auto type = in.Number(4);
				if (type > static_cast<std::uint32_t>(ScriptPropertyValue::Type::Component))
				{
					return std::unexpected(Invalid());
				}
				property.value.type = static_cast<ScriptPropertyValue::Type>(type);
				for (float& f: property.value.f4)
				{
					f = std::bit_cast<float>(static_cast<std::uint32_t>(in.Number(4)));
				}
				const auto integer = in.Number(8);
				property.value.i64 = std::bit_cast<std::int64_t>(integer);
				if (property.value.type == ScriptPropertyValue::Type::Entity || property.value.type == ScriptPropertyValue::Type::Component)
				{
					property.node = integer;
				}
				property.value.str = in.String();
				if (!in.ok)
				{
					return std::unexpected(Invalid());
				}
				script.properties.push_back(std::move(property));
			}
			script.state = in.String();
			if (!in.ok || script.state.size() >= kStreamingScriptCaptureScratch)
			{
				return std::unexpected(Invalid());
			}
			scripts.push_back(std::move(script));
		}
		if (in.pos != data.size())
		{
			return std::unexpected(Invalid());
		}
		const scene::StreamingNodeIndex nodes(world);
		if (!nodes.ambiguousExact.empty())
		{
			return std::unexpected(Invalid());
		}
		std::unordered_set<std::uint64_t> handles;
		for (auto& script: scripts)
		{
			auto found = nodes.exact.find(script.node);
			if (found == nodes.exact.end())
			{
				return std::unexpected(Invalid());
			}
			std::uint64_t handle = 0;
			for (const auto& instance: instances)
			{
				if (instance.entity == found->second && instance.index == script.index && instance.type == script.type)
				{
					if (handle)
					{
						return std::unexpected(Invalid());
					}
					handle = instance.handle;
				}
			}
			if (!handle)
			{
				return std::unexpected(Invalid());
			}
			if (!handles.insert(handle).second)
			{
				return std::unexpected(Invalid());
			}
			for (auto& property: script.properties)
			{
				property.propertyIndex = host.FindPropertyIndex(script.type, property.name);
				if (property.propertyIndex < 0)
				{
					return std::unexpected(Invalid());
				}
				if (property.node)
				{
					AE_TRY(target, nodes.Resolve(property.node));
					property.value.i64 = target->id;
				}
			}
			script.handle = handle;
			script.entity = found->second;
		}
		auto& context = world.GetRegistry().ctx();
		auto& pending = context.contains<scene::detail::StreamingScriptNodeReferences>()
		    ? context.get<scene::detail::StreamingScriptNodeReferences>()
		    : context.emplace<scene::detail::StreamingScriptNodeReferences>();
		for (const auto& script: scripts)
		{
			auto* component = world.TryGet<ScriptComponent>(script.entity);
			if (!component || script.index >= component->scripts.size() || component->scripts[script.index].path != script.type)
			{
				return std::unexpected(Invalid());
			}
			auto& cached = component->scripts[script.index];
			for (const auto& property: script.properties)
			{
				cached.properties[property.name] = property.value;
				std::erase_if(pending.entries, [&](const auto& reference)
				{
					return reference.ownerNode == script.node && reference.scriptIndex == script.index && reference.property == property.name;
				});
				if (property.node && property.value.i64 == 0)
				{
					pending.entries.push_back({script.node, property.node, script.index, script.type, property.name, 0});
				}
			}
		}
		return cursor;
	}

	Expected<bool> StepStreamingScriptRestore(StreamingScriptRestoreCursor& cursor, scripting::GameplayHost& host, std::size_t maxCount)
	{
		if (cursor.failed || cursor.next > cursor.scripts.size())
		{
			return std::unexpected(Invalid());
		}
		std::size_t consumed = 0;
		while (cursor.next < cursor.scripts.size() && consumed < maxCount)
		{
			auto& script = cursor.scripts[cursor.next];
			for (const auto& property: script.properties)
			{
				auto result = host.TrySetPropertyValue(script.handle, property.propertyIndex, property.value);
				if (!result)
				{
					cursor.failed = true;
					return std::unexpected(result.error());
				}
			}
			auto result = host.RestoreStreamingState(script.handle, script.state);
			if (!result)
			{
				cursor.failed = true;
				return std::unexpected(result.error());
			}
			++cursor.next;
			++consumed;
		}
		return cursor.next == cursor.scripts.size();
	}

} // namespace aether::app::web
