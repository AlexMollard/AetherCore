#include "scene/SceneDocumentExpansion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "scene/TransformUtils.hpp"
#include "utils/TomlParse.hpp"

namespace aether::app::scene
{
	namespace
	{
		using Status = std::expected<void, std::string>;

		bool IsReference(const ScriptPropertyValue& value)
		{
			return value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
		}

		struct Expansion
		{
			const SceneDocumentReader& reader;
			SceneDocumentExpansionLimits limits;
			SceneDescription output;
			std::vector<bool> removed;
			std::unordered_set<std::string> stack;

			Status Rebase(EntityRecord& record, std::size_t base, std::size_t count)
			{
				auto index = [base, count](int& value) -> Status
				{
					if (value < -1 || (value >= 0 && static_cast<std::size_t>(value) >= count))
					{
						return std::unexpected("Scene document contains an invalid positional entity reference");
					}
					if (value >= 0)
					{
						value += static_cast<int>(base);
					}
					return {};
				};
				if (auto status = index(record.parentIndex); !status)
				{
					return status;
				}
				record.parentNodeId = 0;
				if (record.joint)
				{
					if (auto status = index(record.joint->targetIndex); !status)
					{
						return status;
					}
				}
				if (auto status = index(record.joint2DTargetIndex); !status)
				{
					return status;
				}
				for (auto& joint: record.scriptJoints)
				{
					if (auto status = index(joint.targetIndex); !status)
					{
						return status;
					}
				}
				for (auto& script: record.scripts)
				{
					for (const auto& [name, node]: script.nodeRefs)
					{
						const auto property = script.properties.find(name);
						if (node == 0 || property == script.properties.end() || !IsReference(property->second))
						{
							return std::unexpected("Scene document contains a malformed script node reference");
						}
					}
					for (auto& [name, value]: script.properties)
					{
						if (IsReference(value) && !script.nodeRefs.contains(name))
						{
							if (value.i64 < -1 || (value.i64 >= 0 && static_cast<std::uint64_t>(value.i64) >= count))
							{
								return std::unexpected("Scene document contains an invalid script entity reference");
							}
							if (value.i64 >= 0)
							{
								value.i64 += static_cast<std::int64_t>(base);
							}
						}
					}
				}
				return {};
			}

			Status Append(const std::vector<EntityRecord>& records)
			{
				const std::size_t base = output.entities.size();
				if (base > limits.maxEntities || records.size() > limits.maxEntities - base || base + records.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				{
					return std::unexpected("Scene document expansion exceeds its entity limit");
				}
				for (std::size_t i = 0; i < records.size(); ++i)
				{
					// Check the authored hierarchy before rebasing; bounded traversal also rejects cycles.
					int parent = records[i].parentIndex;
					std::size_t steps = 0;
					while (parent >= 0 && static_cast<std::size_t>(parent) < records.size())
					{
						if (++steps > limits.maxDepth || static_cast<std::size_t>(parent) == i)
						{
							return std::unexpected("Scene document hierarchy is cyclic or exceeds its depth limit");
						}
						parent = records[static_cast<std::size_t>(parent)].parentIndex;
					}
					EntityRecord record = records[i];
					if (record.hasTransform)
					{
						for (int axis = 0; axis < 3; ++axis)
						{
							if (!std::isfinite(record.position[axis]) || !std::isfinite(record.eulerDeg[axis]) || !std::isfinite(record.scale[axis]))
							{
								return std::unexpected("Scene document contains a non-finite transform");
							}
						}
					}
					if (auto status = Rebase(record, base, records.size()); !status)
					{
						return status;
					}
					output.entities.push_back(std::move(record));
					removed.push_back(false);
				}
				return {};
			}

			Status Document(const SceneDescription& document, std::size_t depth, bool prefab)
			{
				if (document.version > kSceneFormatVersion || document.version < 1)
				{
					return std::unexpected("Unsupported scene document version");
				}
				if (depth > limits.maxDepth)
				{
					return std::unexpected("Scene document expansion exceeds its dependency depth limit");
				}
				if (prefab && (!document.includes.empty() || !document.lights.empty() || document.environment))
				{
					return std::unexpected("Prefab includes, legacy lights and environments cannot be flattened faithfully");
				}
				output.features = output.features | document.features;
				output.assetManifest.insert(output.assetManifest.end(), document.assetManifest.begin(), document.assetManifest.end());
				output.lights.insert(output.lights.end(), document.lights.begin(), document.lights.end());
				if (auto status = Append(document.entities); !status)
				{
					return status;
				}
				for (const auto& instance: document.prefabInstances)
				{
					if (auto status = Prefab(instance, depth + 1); !status)
					{
						return status;
					}
				}
				std::unordered_set<std::string> included;
				for (const auto& path: document.includes)
				{
					if (!included.insert(path).second)
					{
						continue;
					}
					if (auto status = Read(path, SceneDocumentKind::Scene, depth + 1); !status)
					{
						return status;
					}
				}
				return {};
			}

			Status Read(const std::string& path, SceneDocumentKind kind, std::size_t depth)
			{
				const std::string key = (kind == SceneDocumentKind::Prefab ? "prefab:" : "scene:") + path;
				if (path.empty() || !reader || !stack.insert(key).second)
				{
					return std::unexpected("Missing scene document reader/path or cyclic dependency: " + key);
				}
				auto document = reader(path, kind);
				if (!document)
				{
					return std::unexpected("Cannot read " + key + ": " + document.error());
				}
				auto status = Document(*document, depth, kind == SceneDocumentKind::Prefab);
				stack.erase(key);
				return status;
			}

			Status Prefab(const PrefabInstanceRecord& instance, std::size_t depth)
			{
				const std::string key = "prefab:" + instance.prefabPath;
				if (!reader || instance.prefabPath.empty() || depth > limits.maxDepth || !stack.insert(key).second)
				{
					return std::unexpected("Missing prefab reader/path, cyclic dependency or depth limit: " + key);
				}
				auto source = reader(instance.prefabPath, SceneDocumentKind::Prefab);
				if (!source)
				{
					return std::unexpected("Cannot read " + key + ": " + source.error());
				}
				const std::size_t base = output.entities.size();
				std::size_t root = source->entities.size();
				std::unordered_map<std::uint64_t, std::size_t> byGuid;
				for (std::size_t i = 0; i < source->entities.size(); ++i)
				{
					if (!byGuid.emplace(EffectiveGuid(source->entities[i], i), i).second)
					{
						return std::unexpected("Prefab contains ambiguous duplicate entity guids: " + key);
					}
					if (root == source->entities.size() && source->entities[i].parentIndex < 0)
					{
						root = i;
					}
				}
				if (root == source->entities.size())
				{
					return std::unexpected("Prefab has no authored root: " + key);
				}
				if (auto status = Document(*source, depth, true); !status)
				{
					return status;
				}
				stack.erase(key);
				const std::size_t rootIndex = base + root;
				const auto& rootSource = source->entities[root];
				if (rootSource.hasTransform)
				{
					const glm::mat4 oldWorld = ComposeTransform(rootSource.position, rootSource.eulerDeg, rootSource.scale);
					if (std::abs(glm::determinant(oldWorld)) < 1e-8f)
					{
						return std::unexpected("Prefab root transform is singular: " + key);
					}
					const glm::mat4 delta = ComposeTransform(instance.position, instance.eulerDeg, instance.scale) * glm::inverse(oldWorld);
					for (std::size_t i = base; i < output.entities.size(); ++i)
					{
						std::size_t ancestor = i;
						while (ancestor != rootIndex && output.entities[ancestor].parentIndex >= 0)
						{
							ancestor = static_cast<std::size_t>(output.entities[ancestor].parentIndex);
						}
						auto& record = output.entities[i];
						if (ancestor == rootIndex && record.hasTransform)
						{
							const glm::mat4 transformed = delta * ComposeTransform(record.position, record.eulerDeg, record.scale);
							DecomposeTRS(transformed, record.position, record.eulerDeg, record.scale);
							const glm::mat4 recomposed = ComposeTransform(record.position, record.eulerDeg, record.scale);
							for (int column = 0; column < 4; ++column)
							{
								for (int row = 0; row < 4; ++row)
								{
									if (!std::isfinite(transformed[column][row]) || std::abs(transformed[column][row] - recomposed[column][row]) > 0.001f * std::max(1.0f, std::abs(transformed[column][row])))
									{
										return std::unexpected("Prefab placement produces a transform that cannot be represented as TRS: " + key);
									}
								}
							}
						}
					}
					output.entities[rootIndex].position = instance.position;
					output.entities[rootIndex].eulerDeg = instance.eulerDeg;
					output.entities[rootIndex].scale = instance.scale;
				}
				if (instance.node != 0)
				{
					output.entities[rootIndex].nodeId = instance.node;
				}
				if (!instance.name.empty() && !output.entities[rootIndex].name.empty())
				{
					output.entities[rootIndex].name = instance.name;
				}
				for (const auto guid: instance.removedGuids)
				{
					const auto found = byGuid.find(guid);
					if (found == byGuid.end() || found->second == root)
					{
						continue;
					}
					const std::size_t target = base + found->second;
					for (std::size_t i = base; i < output.entities.size(); ++i)
					{
						std::size_t ancestor = i;
						while (ancestor != target && output.entities[ancestor].parentIndex >= 0)
						{
							ancestor = static_cast<std::size_t>(output.entities[ancestor].parentIndex);
						}
						removed[i] = removed[i] || ancestor == target;
					}
				}
				for (const auto& overrideRecord: instance.overrides)
				{
					if (!ParseTomlDocument(overrideRecord.partialToml))
					{
						return std::unexpected("Malformed prefab override: " + key);
					}
					const auto found = byGuid.find(overrideRecord.guid);
					if (found == byGuid.end() || removed[base + found->second])
					{
						continue;
					}
					const std::size_t target = base + found->second;
					EntityRecord merged = MergePrefabOverride(source->entities[found->second], overrideRecord.partialToml);
					merged.parentIndex = -1;
					if (auto status = Rebase(merged, base, source->entities.size()); !status)
					{
						return status;
					}
					const auto& previous = output.entities[target];
					merged.parentIndex = previous.parentIndex;
					if (target == rootIndex)
					{
						merged.name = previous.name;
						if (previous.hasTransform)
						{
							merged.hasTransform = true;
							merged.position = previous.position;
							merged.eulerDeg = previous.eulerDeg;
							merged.scale = previous.scale;
						}
					}
					if (merged.nodeId == 0 || target == rootIndex)
					{
						merged.nodeId = previous.nodeId;
					}
					output.entities[target] = std::move(merged);
				}
				const std::size_t addedBase = output.entities.size();
				if (auto status = Append(instance.addedEntities); !status)
				{
					return status;
				}
				for (std::size_t i = addedBase; i < output.entities.size(); ++i)
				{
					if (output.entities[i].parentIndex < 0)
					{
						output.entities[i].parentIndex = static_cast<int>(rootIndex);
					}
				}
				return {};
			}

			Status Finish()
			{
				std::vector<int> mapping(output.entities.size(), -1);
				std::unordered_map<std::uint64_t, int> nodes;
				std::unordered_set<std::uint64_t> ambiguous;
				int next = 0;
				for (std::size_t i = 0; i < output.entities.size(); ++i)
				{
					if (!removed[i])
					{
						mapping[i] = next++;
						const auto node = output.entities[i].nodeId;
						if (node != 0 && !nodes.emplace(node, mapping[i]).second)
						{
							ambiguous.insert(node);
						}
					}
				}
				std::vector<EntityRecord> entities;
				for (std::size_t i = 0; i < output.entities.size(); ++i)
				{
					if (removed[i])
					{
						continue;
					}
					auto record = std::move(output.entities[i]);
					auto remap = [&mapping](int& index)
					{
						if (index >= 0)
						{
							index = mapping[static_cast<std::size_t>(index)];
						}
					};
					remap(record.parentIndex);
					if (record.joint)
					{
						remap(record.joint->targetIndex);
					}
					remap(record.joint2DTargetIndex);
					for (auto& joint: record.scriptJoints)
					{
						remap(joint.targetIndex);
					}
					for (auto& script: record.scripts)
					{
						for (auto& [name, value]: script.properties)
						{
							if (!IsReference(value))
							{
								continue;
							}
							if (const auto node = script.nodeRefs.find(name); node != script.nodeRefs.end())
							{
								if (ambiguous.contains(node->second))
								{
									return std::unexpected("Script reference names an ambiguous scene node");
								}
								if (const auto found = nodes.find(node->second); found != nodes.end())
								{
									value.i64 = found->second;
									script.nodeRefs.erase(node);
								}
								else if (limits.preserveExternalNodeReferences || !output.streamingRegions.empty())
								{
									value.i64 = -1;
								}
								else
								{
									return std::unexpected("Script reference names a missing scene node");
								}
							}
							else if (value.i64 >= 0)
							{
								value.i64 = mapping[static_cast<std::size_t>(value.i64)];
							}
						}
					}
					entities.push_back(std::move(record));
				}
				output.entities = std::move(entities);
				return {};
			}
		};
	} // namespace

	std::expected<SceneDescription, std::string> ExpandSceneDocument(const SceneDescription& scene, const SceneDocumentReader& reader, SceneDocumentExpansionLimits limits)
	{
		Expansion expansion{reader, limits, {}, {}, {}};
		expansion.output = scene;
		expansion.output.entities.clear();
		expansion.output.prefabInstances.clear();
		expansion.output.includes.clear();
		expansion.output.assetManifest.clear();
		expansion.output.lights.clear();
		if (!scene.name.empty())
		{
			expansion.stack.insert("scene:" + scene.name);
		}
		if (auto status = expansion.Document(scene, 0, false); !status)
		{
			return std::unexpected(status.error());
		}
		if (auto status = expansion.Finish(); !status)
		{
			return std::unexpected(status.error());
		}
		return std::move(expansion.output);
	}
} // namespace aether::app::scene
