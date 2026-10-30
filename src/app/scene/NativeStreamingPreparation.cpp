#include "scene/NativeStreamingPreparation.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <unordered_set>

#include "io/FileSystem.hpp"
#include "scene/SceneDocumentExpansion.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "scene/TransformUtils.hpp"

namespace aether::app::scene
{
	namespace
	{
		constexpr std::size_t kMaxDocumentBytes = 4 * 1024 * 1024;
		constexpr std::size_t kMaxDocuments = 128;
		constexpr std::size_t kMaxEntities = 10000;
		// Admission covers parser metadata and expansion copies as well as the input.
		// This is a content budget, not an accounting of allocator overhead.
		constexpr std::uint64_t kDecodeAllowancePerByte = 1024;

		std::string DocumentPath(std::string_view reference, SceneDocumentKind kind)
		{
			const std::string_view suffix = kind == SceneDocumentKind::Scene ? ".scene.toml" : ".prefab.toml";
			std::string path;
			if (reference.starts_with("uid://") || reference.starts_with("project://"))
			{
				path = io::FileSystem::ResolveAssetPath(reference);
			}
			else if (IsValidAssetName(reference))
			{
				path = std::string(kind == SceneDocumentKind::Scene ? "project://scenes/" : "project://assets/prefabs/") + std::string(reference) + std::string(suffix);
			}
			if (!path.starts_with("project://") || !path.ends_with(suffix) || path.find("..") != std::string::npos || path.find('\\') != std::string::npos)
			{
				return {};
			}
			return path;
		}

		std::uint64_t NodeHash(std::string_view region, std::string_view identity)
		{
			std::uint64_t hash = 14695981039346656037ull;
			for (const unsigned char c: region)
			{
				hash ^= c;
				hash *= 1099511628211ull;
			}
			hash ^= 0xff;
			hash *= 1099511628211ull;
			for (const unsigned char c: identity)
			{
				hash ^= c;
				hash *= 1099511628211ull;
			}
			return hash ? hash : 1;
		}

		std::expected<void, std::string> NamespaceNodes(SceneDescription& scene, std::string_view region)
		{
			if (auto result = EnsureStreamingSceneNodeIds(scene); !result)
			{
				return result;
			}
			std::unordered_set<std::uint64_t> used;
			for (auto& entity: scene.entities)
			{
				entity.nodeId = StreamingNodeId(region, entity.nodeId);
				if (!used.insert(entity.nodeId).second)
				{
					return std::unexpected("Ambiguous qualified region node identity");
				}
			}
			return {};
		}

		std::expected<void, std::string> ConvertLights(SceneDescription& scene)
		{
			if (scene.entities.size() > kMaxEntities || scene.lights.size() > kMaxEntities - scene.entities.size())
			{
				return std::unexpected("Scene light count exceeds entity limit");
			}
			for (const auto& light: scene.lights)
			{
				if (light.isSpot && glm::dot(light.direction, light.direction) < 1e-12f)
				{
					return std::unexpected("Spot light has zero direction");
				}
				EntityRecord record;
				record.name = light.isSpot ? "Spot Light" : "Point Light";
				// Legacy lights have no authored node ID; position and kind are
				// their migration key. Coincident lights fail the collision check.
				record.nodeId = NodeHash("legacy-light", record.name + ":" + std::to_string(light.position.x) + ":" + std::to_string(light.position.y) + ":" + std::to_string(light.position.z));
				record.hasTransform = true;
				if (light.isSpot)
				{
					DecomposeTRS(ecs::LightAimMatrix(light.position, light.direction), record.position, record.eulerDeg, record.scale);
				}
				else
				{
					record.position = light.position;
				}
				GenericComponent component;
				component.type = record.name;
				auto color = reflect::MakeValue(light.color);
				color.type = reflect::FieldType::Color3;
				component.fields.emplace_back("color", std::move(color));
				component.fields.emplace_back("intensity", reflect::MakeValue(light.intensity));
				component.fields.emplace_back("radius", reflect::MakeValue(light.radius));
				component.fields.emplace_back("shadow", reflect::MakeValue(light.castsShadow));
				if (light.isSpot)
				{
					component.fields.emplace_back("inner_angle_deg", reflect::MakeValue(light.innerAngleRad));
					component.fields.emplace_back("outer_angle_deg", reflect::MakeValue(light.outerAngleRad));
				}
				record.reflected.push_back(std::move(component));
				scene.entities.push_back(std::move(record));
			}
			scene.lights.clear();
			return {};
		}
	} // namespace

	std::string ResolveNativeSceneStreamingDependency(std::string_view reference)
	{
		const auto path = io::FileSystem::ResolveAssetPath(reference);
		if (path.empty() || (!path.starts_with("project://") && !path.starts_with("engine://")))
		{
			return {};
		}
		return io::FileSystem::AssetReference(path);
	}

	NativePreparedRegion::Reservation::~Reservation()
	{
		if (ledger && bytes)
		{
			ledger->Release(streaming::MemoryClass::CpuPending, bytes);
		}
	}

	std::optional<NativePreparedRegion> NativeStreamingPreparation::Job::Take()
	{
		std::lock_guard lock(mutex);
		auto taken = std::move(result);
		result.reset();
		return taken;
	}

	std::shared_ptr<NativeStreamingPreparation::Job> NativeStreamingPreparation::Submit(
	        std::string reference, std::string regionId, streaming::RequestId request, io::IOPriority priority, std::shared_ptr<std::atomic_bool> cancelled, streaming::BudgetLedger& ledger)
	{
		std::shared_ptr<Job> job;
		{
			std::lock_guard lock(m_mutex);
			std::erase_if(m_jobs, [](const auto& item) { return item.expired(); });
			if (m_jobs.size() >= ledger.Limits().maxPendingRequests)
			{
				return {};
			}
			job = std::make_shared<Job>();
			m_jobs.push_back(job);
		}
		io::FileSystem::Submit(priority,
		        [job, reference = std::move(reference), regionId = std::move(regionId), request, cancelled = std::move(cancelled), &ledger]() mutable
		        {
			        NativePreparedRegion result;
			        result.request = request;
			        result.reservation = std::make_shared<NativePreparedRegion::Reservation>();
			        result.reservation->ledger = &ledger;
			        try
			        {
				        std::size_t documents = 0;
				        auto isCancelled = [&]()
				        {
					        return cancelled && cancelled->load(std::memory_order_relaxed);
				        };
				        auto reader = [&](std::string_view ref, SceneDocumentKind kind) -> std::expected<SceneDescription, std::string>
				        {
					        if (isCancelled())
					        {
						        return std::unexpected("Cancelled");
					        }
					        if (++documents > kMaxDocuments)
					        {
						        return std::unexpected("Scene document count limit exceeded");
					        }
					        auto path = DocumentPath(ref, kind);
					        if (path.empty())
					        {
						        return std::unexpected("Invalid scene document reference");
					        }
					        if (!io::FileSystem::Exists(path))
					        {
						        path.replace(path.size() - 4, 4, "bin");
					        }
					        auto bytes = io::FileSystem::ReadFileReserved(path,
					                [&](std::size_t size)
					                {
						                if (isCancelled() || size > kMaxDocumentBytes || size > std::numeric_limits<std::uint64_t>::max() / (kDecodeAllowancePerByte + 1))
						                {
							                return false;
						                }
						                const auto charge = static_cast<std::uint64_t>(size) * (kDecodeAllowancePerByte + 1);
						                if (!ledger.TryReserve(streaming::MemoryClass::CpuPending, charge))
						                {
							                return false;
						                }
						                result.reservation->bytes += charge;
						                return true;
					                });
					        if (!bytes)
					        {
						        return std::unexpected("Cannot read or reserve scene document: " + path);
					        }
					        std::optional<SceneDescription> document;
					        if (path.ends_with(".bin"))
					        {
						        auto decoded = ReadSceneBinaryChecked(bytes->data(), bytes->size());
						        if (!decoded)
						        {
							        return std::unexpected("Malformed binary scene document " + path + ": " + decoded.error().message);
						        }
						        document = std::move(*decoded);
					        }
					        else
					        {
						        document = ParseToml(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
					        }
					        if (!document)
					        {
						        return std::unexpected("Malformed scene document: " + path);
					        }
					        if (document->version < 1 || document->version > kSceneFormatVersion || !document->streamingRegions.empty())
					        {
						        return std::unexpected("Unsupported document version or nested streaming world: " + path);
					        }
					        if (document->entities.size() > kMaxEntities)
					        {
						        return std::unexpected("Scene entity count limit exceeded");
					        }
					        return std::move(*document);
				        };
				        auto root = reader(reference, SceneDocumentKind::Scene);
				        if (!root)
				        {
					        result.error = root.error();
				        }
				        else
				        {
					        root->name = reference;
					        auto expanded = ExpandSceneDocument(*root, reader, {.maxDepth = 32, .maxEntities = kMaxEntities, .preserveExternalNodeReferences = true});
					        if (!expanded)
					        {
						        result.error = expanded.error();
					        }
					        else if (isCancelled())
					        {
						        result.error = "Cancelled";
					        }
					        else if (auto converted = ConvertLights(*expanded); !converted)
					        {
						        result.error = converted.error();
					        }
					        else if (auto deps = CollectSceneStreamingDependencies(*expanded, result.dependencies, ResolveNativeSceneStreamingDependency); !deps)
					        {
						        result.error = deps.error();
					        }
					        else if (auto namespaced = NamespaceNodes(*expanded, regionId); !namespaced)
					        {
						        result.error = namespaced.error();
					        }
					        else
					        {
						        if (result.error.empty() && !isCancelled())
						        {
							        result.scene = std::make_shared<const SceneDescription>(std::move(*expanded));
						        }
						        else if (result.error.empty())
						        {
							        result.error = "Cancelled";
						        }
					        }
				        }
			        }
			        catch (const std::exception& e)
			        {
				        result.error = e.what();
			        }
			        catch (...)
			        {
				        result.error = "Scene preparation failed";
			        }
			        if (!result.scene)
			        {
				        result.dependencies.clear();
				        result.reservation.reset();
			        }
			        std::lock_guard lock(job->mutex);
			        job->result.emplace(std::move(result));
		        });
		return job;
	}
} // namespace aether::app::scene
