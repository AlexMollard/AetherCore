#include "assets/AssetStreaming.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/AssetManager.hpp"
#include "assets/StreamingDecode.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "audio/AudioSubsystem.hpp"
#include "ui/UiRenderer.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/LoadedModel.hpp"

namespace aether
{
	namespace
	{
		std::string AssetIdentity(std::string_view reference)
		{
			const auto path = io::FileSystem::ResolveAssetPath(reference);
			return path.empty() ? std::string{} : io::FileSystem::AssetReference(path);
		}
	} // namespace

	using streaming::MemoryClass;
	using streaming::State;

	struct AssetStreaming::Impl
	{
		struct Job
		{
			std::mutex mutex;
			std::atomic<bool> cancelled{false};
			bool done = false;
			bool reservationRejected = false;
			std::optional<Expected<assets::StreamingDecodeResult>> result;
			std::uint64_t reserved = 0;
		};

		struct ModelOwner
		{
			AssetManager* manager = nullptr;
			LoadedModel model;

			~ModelOwner()
			{
				if (manager)
				{
					manager->ReleaseModelTextures(model);
				}
			}
		};

		struct PendingTexture
		{
			std::string path;
			TextureColorSpace colorSpace;
			std::uint64_t gpuBytes = 0;
			std::uint64_t cpuBytes = 0;
			Texture::ChunkedUpload upload;
		};

		struct Asset
		{
			std::string path;
			std::string identity;
			State state = State::Queued;
			std::size_t claimCount = 0;
			std::size_t requiredCount = 0;
			std::array<std::size_t, 4> priorityCounts{};
			std::string error;
			std::shared_ptr<Job> job;
			std::optional<assets::StreamingDecodeResult> decoded;
			std::vector<PendingTexture> pendingTextures;
			std::vector<TextureHandle> textures;
			std::shared_ptr<ModelOwner> model;
			std::shared_ptr<AssetManager::PreparedModelUpload> modelUpload;
			std::optional<ui::UiRenderer::FontUpload> fontUpload;
			std::uint64_t fontGpu = 0;
			bool fontReady = false;
			std::shared_ptr<const void> documentLease = std::make_shared<int>(0);
			std::uint64_t cpu = 0, gpu = 0, staging = 0, ticket = 0;
			std::uint64_t retainedAudioCpu = 0;
			std::uint64_t modelStaging = 0;
			MemoryClass cpuClass = MemoryClass::CpuPending;
			MemoryClass gpuClass = MemoryClass::GpuPending;
			std::size_t nextTexture = 0;
			bool dependenciesScheduled = false;
			bool prefetchEvicted = false;
			std::vector<streaming::RequestId> dependencies;
			enum class Document
			{
				None,
				Atlas,
				Animation,
				TileSet,
				TileMap,
				Font,
				Audio,
				Material
			};
			Document document = Document::None;
			bool documentPublished = false;
		};

		struct Request
		{
			std::shared_ptr<Asset> asset;
			io::IOPriority priority = io::IOPriority::Normal;
			bool required = false;
			State terminal = State::Unknown;
		};

		explicit Impl(AssetManager& owner)
		      : manager(owner), budget(std::make_shared<streaming::BudgetLedger>())
		{
		}

		AssetManager& manager;
		SpriteAssetStore* sprites = nullptr;
		TileAssetStore* tiles = nullptr;
		ui::UiRenderer* uiRenderer = nullptr;
		audio::AudioSubsystem* audio = nullptr;
		std::shared_ptr<streaming::BudgetLedger> budget;
		std::unordered_map<std::string, std::shared_ptr<Asset>> assets;
		std::unordered_map<streaming::RequestId, Request> requests;
		streaming::RequestId nextId = 1;
		std::uint64_t uploadBytesThisFrame = 0;
		std::vector<std::shared_ptr<Asset>> queuedScratch;
		std::vector<std::shared_ptr<Asset>> dependencyScratch;
		bool shutdown = false;

		std::size_t Claims(const Asset& asset) const
		{
			return asset.claimCount;
		}

		bool Required(const Asset& asset) const
		{
			return asset.requiredCount != 0;
		}

		io::IOPriority Priority(const Asset& asset) const
		{
			for (std::size_t i = 0; i < asset.priorityCounts.size(); ++i)
			{
				if (asset.priorityCounts[i])
				{
					return static_cast<io::IOPriority>(i);
				}
			}
			return io::IOPriority::Background;
		}

		void AddClaim(Request& request)
		{
			auto& asset = *request.asset;
			const auto oldPriority = Priority(asset);
			const bool oldRequired = Required(asset);
			if (!asset.claimCount && asset.state == State::Loaded && asset.decoded && !asset.dependenciesScheduled)
			{
				asset.state = State::Uploading;
			}
			++asset.claimCount;
			if (request.required)
			{
				++asset.requiredCount;
			}
			++asset.priorityCounts[static_cast<std::size_t>(request.priority)];
			if (oldPriority != Priority(asset) || oldRequired != Required(asset))
			{
				PropagateDemand(asset);
			}
		}

		void ReleaseDependencies(Asset& asset)
		{
			auto dependencies = std::move(asset.dependencies);
			asset.dependencies.clear();
			asset.dependenciesScheduled = false;
			for (auto id: dependencies)
			{
				auto it = requests.find(id);
				if (it == requests.end())
				{
					continue;
				}
				if (it->second.terminal == State::Unknown)
				{
					DropClaim(it->second);
				}
				requests.erase(it);
			}
		}

		void DropClaim(Request& request)
		{
			auto& asset = *request.asset;
			const auto oldPriority = Priority(asset);
			const bool oldRequired = Required(asset);
			--asset.claimCount;
			if (request.required)
			{
				--asset.requiredCount;
			}
			--asset.priorityCounts[static_cast<std::size_t>(request.priority)];
			if (!asset.claimCount)
			{
				if (asset.job)
				{
					asset.job->cancelled.store(true, std::memory_order_release);
				}
				ReleaseDependencies(asset);
			}
			else if (oldPriority != Priority(asset) || oldRequired != Required(asset))
			{
				PropagateDemand(asset);
			}
		}

		void ChangeDemand(Request& request, io::IOPriority priority, bool required)
		{
			auto& asset = *request.asset;
			const auto oldPriority = Priority(asset);
			const bool oldRequired = Required(asset);
			if (request.priority != priority)
			{
				--asset.priorityCounts[static_cast<std::size_t>(request.priority)];
				++asset.priorityCounts[static_cast<std::size_t>(priority)];
				request.priority = priority;
			}
			if (request.required != required)
			{
				if (required)
				{
					++asset.requiredCount;
				}
				else
				{
					--asset.requiredCount;
				}
				request.required = required;
			}
			if (oldPriority != Priority(asset) || oldRequired != Required(asset))
			{
				PropagateDemand(asset);
			}
		}

		void PropagateDemand(Asset& asset)
		{
			const auto priority = Priority(asset);
			const bool required = Required(asset);
			for (auto id: asset.dependencies)
			{
				auto it = requests.find(id);
				if (it != requests.end() && it->second.terminal == State::Unknown)
				{
					ChangeDemand(it->second, priority, required);
				}
			}
		}

		void Start(const std::shared_ptr<Asset>& asset)
		{
			const auto path = io::FileSystem::ResolveAssetPath(asset->identity);
			if (path.empty())
			{
				asset->error = "Streaming asset reference no longer resolves: " + asset->identity;
				asset->state = State::Failed;
				return;
			}
			asset->path = path;
			auto job = std::make_shared<Job>();
			asset->job = job;
			asset->state = State::Loading;
			auto ledger = budget;
			// Capture the current path only after resolving the stable identity.
			io::FileSystem::Submit(Priority(*asset),
			        [job, ledger, path]()
			        {
				        try
				        {
					        if (job->cancelled.load(std::memory_order_acquire))
					        {
						        std::lock_guard lock(job->mutex);
						        job->result.emplace(std::unexpected(AetherError::Asset("Streaming decode cancelled")));
						        job->done = true;
						        return;
					        }
					        auto result = assets::DecodeStreamingAsset(
					                path,
					                [&](std::size_t bytes)
					                {
						                if (job->cancelled.load(std::memory_order_acquire))
						                {
							                return false;
						                }
						                if (!ledger->TryReserve(MemoryClass::CpuPending, bytes))
						                {
							                job->reservationRejected = true;
							                return false;
						                }
						                job->reserved += bytes;
						                return true;
					                },
					                [job]() { return job->cancelled.load(std::memory_order_acquire); });
					        std::lock_guard lock(job->mutex);
					        job->result.emplace(std::move(result));
					        job->done = true;
				        }
				        catch (const std::exception& e)
				        {
					        std::lock_guard lock(job->mutex);
					        job->result.emplace(std::unexpected(AetherError::Asset(e.what())));
					        job->done = true;
				        }
				        catch (...)
				        {
					        std::lock_guard lock(job->mutex);
					        job->result.emplace(std::unexpected(AetherError::Asset("Streaming worker failed")));
					        job->done = true;
				        }
			        });
		}

		void ReleaseGpuLater(std::uint64_t bytes)
		{
			if (!bytes)
			{
				return;
			}
			auto ledger = budget;
			gpu::ResourceRegistry::DeferDestructionCallback([ledger, bytes]() { ledger->Release(MemoryClass::GpuRetiring, bytes); });
		}

		void Dispose(Asset& asset)
		{
			ReleaseDependencies(asset);
			switch (asset.document)
			{
				case Asset::Document::Atlas:
				case Asset::Document::Animation:
					if (sprites)
					{
						sprites->Invalidate(asset.path);
					}
					break;
				case Asset::Document::TileSet:
				case Asset::Document::TileMap:
					if (tiles)
					{
						tiles->Invalidate(asset.path);
					}
					break;
				case Asset::Document::Font:
					if (uiRenderer)
					{
						const auto bytes = asset.fontGpu;
						auto ledger = budget;
						(void) ledger->Transfer(MemoryClass::GpuResident, MemoryClass::GpuRetiring, bytes);
						uiRenderer->RetireStreamedFont(asset.path, [ledger, bytes]() { ledger->Release(MemoryClass::GpuRetiring, bytes); });
						asset.fontGpu = 0;
					}
					break;
				case Asset::Document::Audio:
					if (audio)
					{
						audio->RetireStreamedClip(asset.path);
					}
					break;
				case Asset::Document::Material:
					manager.RetireStreamedMaterial(asset.identity);
					break;
				default:
					break;
			}
			if (asset.fontUpload && uiRenderer)
			{
				const auto bytes = asset.fontGpu;
				auto ledger = budget;
				uiRenderer->DiscardStreamedFontUpload(*asset.fontUpload, [ledger, bytes]() { ledger->Release(MemoryClass::GpuPending, bytes); });
				asset.fontUpload.reset();
				asset.fontGpu = 0;
			}
			auto& registry = manager.GetTextureRegistry();
			for (auto handle: asset.textures)
			{
				registry.Release(handle);
			}
			asset.textures.clear();
			if (asset.model && asset.model.use_count() == 1)
			{
				manager.ReleaseModelTextures(asset.model->model);
				asset.model->manager = nullptr;
			}
			std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
			asset.modelUpload.reset();
			asset.model.reset();
			asset.pendingTextures.clear();
			asset.decoded.reset();
			if (asset.cpu)
			{
				budget->Release(asset.cpuClass, asset.cpu);
				asset.cpu = 0;
			}
			asset.retainedAudioCpu = 0;
			if (asset.staging)
			{
				budget->Release(MemoryClass::Staging, asset.staging);
				asset.staging = 0;
			}
			if (asset.gpu)
			{
				(void) budget->Transfer(asset.gpuClass, MemoryClass::GpuRetiring, asset.gpu);
				ReleaseGpuLater(asset.gpu);
				asset.gpu = 0;
			}
		}

		bool EvictPrefetch(const Asset& requester)
		{
			for (auto& [path, candidate]: assets)
			{
				if (candidate.get() == &requester || candidate->state != State::Loaded || Required(*candidate))
				{
					continue;
				}
				if (candidate->model && (candidate->model.use_count() != 1 || manager.HasLiveModelUsers(candidate->model->model)))
				{
					continue;
				}
				if (candidate->documentLease.use_count() != 1)
				{
					continue;
				}
				if (candidate->document == Asset::Document::TileMap && tiles && tiles->IsTileMapDirty(candidate->path))
				{
					continue;
				}
				Dispose(*candidate);
				candidate->document = Asset::Document::None;
				candidate->documentPublished = false;
				candidate->fontReady = false;
				candidate->dependenciesScheduled = false;
				candidate->nextTexture = 0;
				candidate->cpuClass = MemoryClass::CpuPending;
				candidate->gpuClass = MemoryClass::GpuPending;
				candidate->prefetchEvicted = true;
				candidate->state = State::Blocked;
				candidate->error = "Prefetch evicted under budget pressure";
				return true;
			}
			return false;
		}
	};

	AssetStreaming::AssetStreaming(AssetManager& manager)
	      : m_impl(std::make_unique<Impl>(manager))
	{
	}

	AssetStreaming::~AssetStreaming()
	{
		Shutdown();
	}

	void AssetStreaming::SetDocumentStores(SpriteAssetStore* sprites, TileAssetStore* tiles, ui::UiRenderer* ui, audio::AudioSubsystem* audio)
	{
		m_impl->sprites = sprites;
		m_impl->tiles = tiles;
		m_impl->uiRenderer = ui;
		m_impl->audio = audio;
	}

	streaming::RequestId AssetStreaming::Request(std::string_view reference, io::IOPriority priority, bool required)
	{
		auto& impl = *m_impl;
		if (impl.shutdown || reference.empty() || static_cast<unsigned>(priority) > 3)
		{
			return 0;
		}
		const auto path = io::FileSystem::ResolveAssetPath(reference);
		if (path.empty())
		{
			return 0;
		}
		const auto identity = io::FileSystem::AssetReference(path);
		auto it = impl.assets.find(identity);
		if (it == impl.assets.end() || it->second->state != State::Loaded)
		{
			std::size_t pending = 0;
			for (const auto& [id, claim]: impl.requests)
			{
				const auto& asset = *claim.asset;
				// A decoded parent waiting on children must leave admission room
				// for the rest of its dependency chain, even after scheduling them.
				const bool waitingForDependencies = asset.decoded && (!asset.dependenciesScheduled || std::any_of(asset.dependencies.begin(), asset.dependencies.end(),
				        [&](auto dependencyId)
				        {
					        const auto dependency = impl.requests.find(dependencyId);
					        return dependency != impl.requests.end() && dependency->second.asset->state != State::Loaded;
				        }));
				if (claim.terminal == State::Unknown && asset.state != State::Loaded && !waitingForDependencies)
				{
					++pending;
				}
			}
			if (pending >= impl.budget->Limits().maxPendingRequests)
			{
				return 0;
			}
		}
		if (it == impl.assets.end())
		{
			auto asset = std::make_shared<Impl::Asset>();
			asset->identity = identity;
			asset->path = path;
			it = impl.assets.emplace(identity, std::move(asset)).first;
		}
		auto id = impl.nextId++;
		if (!id)
		{
			id = impl.nextId++;
		}
		auto [requestIt, inserted] = impl.requests.emplace(id, Impl::Request{it->second, priority, required});
		if (!inserted)
		{
			return 0;
		}
		impl.AddClaim(requestIt->second);
		if (required && it->second->state == State::Blocked && !it->second->decoded)
		{
			it->second->state = State::Queued;
			it->second->error.clear();
		}
		return id;
	}

	bool AssetStreaming::Cancel(streaming::RequestId id)
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end() || it->second.terminal != State::Unknown)
		{
			return false;
		}
		it->second.terminal = State::Cancelled;
		m_impl->DropClaim(it->second);
		return true;
	}

	bool AssetStreaming::Release(streaming::RequestId id)
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end())
		{
			return false;
		}
		if (it->second.terminal == State::Unknown)
		{
			m_impl->DropClaim(it->second);
		}
		m_impl->requests.erase(it);
		return true;
	}

	bool AssetStreaming::SetRequired(streaming::RequestId id, bool required)
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end() || it->second.terminal != State::Unknown)
		{
			return false;
		}
		m_impl->ChangeDemand(it->second, it->second.priority, required);
		if (required && it->second.asset->state == State::Blocked)
		{
			it->second.asset->state = it->second.asset->decoded ? State::Uploading : State::Queued;
			it->second.asset->error.clear();
		}
		return true;
	}

	bool AssetStreaming::SetPriority(streaming::RequestId id, io::IOPriority priority)
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end() || it->second.terminal != State::Unknown || static_cast<unsigned>(priority) > 3)
		{
			return false;
		}
		m_impl->ChangeDemand(it->second, priority, it->second.required);
		return true;
	}

	State AssetStreaming::GetRequestState(streaming::RequestId id) const
	{
		auto it = m_impl->requests.find(id);
		if (it == m_impl->requests.end())
		{
			return State::Unknown;
		}
		return it->second.terminal != State::Unknown ? it->second.terminal : it->second.asset->state;
	}

	State AssetStreaming::GetAssetState(std::string_view reference) const
	{
		const auto identity = AssetIdentity(reference);
		auto it = m_impl->assets.find(identity);
		return it == m_impl->assets.end() ? State::Unknown : it->second->state;
	}

	LoadedModel* AssetStreaming::FindModel(std::string_view reference) const
	{
		auto it = m_impl->assets.find(AssetIdentity(reference));
		if (it == m_impl->assets.end() || it->second->state != State::Loaded || !it->second->model)
		{
			return nullptr;
		}
		return &it->second->model->model;
	}

	std::shared_ptr<const void> AssetStreaming::AcquireMeshLease(const Mesh* mesh) const
	{
		if (!mesh)
		{
			return {};
		}
		for (const auto& [path, asset]: m_impl->assets)
		{
			if (asset->model)
			{
				for (const auto& primitive: asset->model->model.primitives)
				{
					if (&primitive.mesh == mesh)
					{
						return asset->model;
					}
				}
			}
		}
		return {};
	}

	std::shared_ptr<const void> AssetStreaming::AcquireAnimationLease(const AnimationDatabase* animation) const
	{
		if (!animation)
		{
			return {};
		}
		for (const auto& [path, asset]: m_impl->assets)
		{
			if (asset->model && &asset->model->model.animationDb == animation)
			{
				return asset->model;
			}
		}
		return {};
	}

	std::shared_ptr<const void> AssetStreaming::AcquireDocumentLease(std::string_view reference) const
	{
		auto it = m_impl->assets.find(AssetIdentity(reference));
		if (it == m_impl->assets.end() || it->second->state != State::Loaded)
		{
			return {};
		}
		return it->second->documentLease;
	}

	std::shared_ptr<const void> AssetStreaming::AcquireFontLeaseBySlot(std::uint32_t slot) const
	{
		if (!m_impl->uiRenderer)
		{
			return {};
		}
		for (const auto& [path, asset]: m_impl->assets)
		{
			if (asset->state == State::Loaded && asset->document == Impl::Asset::Document::Font && m_impl->uiRenderer->StreamedFontSlot(path) == slot)
			{
				return asset->documentLease;
			}
		}
		return {};
	}

	Expected<void> AssetStreaming::SetBudgets(const streaming::Budgets& budgets)
	{
		return m_impl->budget->Configure(budgets);
	}

	streaming::BudgetLedger& AssetStreaming::GetBudget()
	{
		return *m_impl->budget;
	}

	void AssetStreaming::Update()
	{
		auto& impl = *m_impl;
		if (impl.shutdown)
		{
			return;
		}
		auto& textures = impl.manager.GetTextureRegistry();
		auto& uploads = impl.manager.GetUploadContext();
		uploads.CollectAsyncUploads();
		auto& queued = impl.queuedScratch;
		queued.clear();
		for (auto& [path, asset]: impl.assets)
		{
			if (asset->state == State::Queued && impl.Claims(*asset))
			{
				queued.push_back(asset);
			}
		}
		std::sort(queued.begin(), queued.end(), [&](const auto& a, const auto& b) { return impl.Priority(*a) < impl.Priority(*b); });
		for (auto& asset: queued)
		{
			impl.Start(asset);
		}

		auto& scheduleDependencies = impl.dependencyScratch;
		scheduleDependencies.clear();
		auto& frameBytes = impl.uploadBytesThisFrame;
		frameBytes = 0;
		for (auto it = impl.assets.begin(); it != impl.assets.end();)
		{
			auto& asset = *it->second;
			if (asset.state == State::Blocked && impl.Claims(asset) && (!asset.prefetchEvicted || impl.Required(asset)))
			{
				asset.prefetchEvicted = false;
				asset.state = asset.decoded ? State::Uploading : State::Queued;
				asset.error.clear();
			}
			if (asset.job)
			{
				auto job = asset.job;
				std::lock_guard lock(job->mutex);
				if (job->done)
				{
					asset.cpu = job->reserved;
					if (job->cancelled.load(std::memory_order_acquire))
					{
						job->result.reset();
						impl.budget->Release(MemoryClass::CpuPending, asset.cpu);
						asset.cpu = 0;
						asset.state = impl.Claims(asset) ? State::Queued : State::Cancelled;
					}
					else if (*job->result)
					{
						asset.decoded.emplace(std::move(**job->result));
						asset.state = State::Uploading;
					}
					else
					{
						asset.error = job->result->error().ToString();
						if (job->reservationRejected)
						{
							impl.budget->Release(MemoryClass::CpuPending, asset.cpu);
							asset.cpu = 0;
							if (impl.Required(asset))
							{
								impl.EvictPrefetch(asset);
							}
							asset.state = State::Blocked;
						}
						else
						{
							impl.budget->Release(MemoryClass::CpuPending, asset.cpu);
							asset.cpu = 0;
							asset.state = State::Failed;
						}
					}
					asset.job.reset();
				}
			}
			if (asset.state == State::Uploading && asset.decoded && !asset.dependenciesScheduled && impl.Claims(asset))
			{
				scheduleDependencies.push_back(it->second);
			}
			bool dependenciesReady = asset.dependenciesScheduled;
			for (auto id: asset.dependencies)
			{
				auto dependency = impl.requests.find(id);
				if (dependency == impl.requests.end())
				{
					dependenciesReady = false;
					asset.error = "Dependency lease lost";
					asset.state = State::Failed;
					break;
				}
				impl.ChangeDemand(dependency->second, impl.Priority(asset), impl.Required(asset));
				const auto state = dependency->second.asset->state;
				if (state == State::Failed || state == State::Blocked)
				{
					asset.error = "Dependency " + dependency->second.asset->path + ": " + dependency->second.asset->error;
					asset.state = state;
					dependenciesReady = false;
					break;
				}
				if (state != State::Loaded)
				{
					dependenciesReady = false;
				}
			}
			for (auto p = asset.pendingTextures.begin(); p != asset.pendingTextures.end();)
			{
				auto ledger = impl.budget;
				const auto gpuBytes = p->gpuBytes;
				const bool wanted = impl.Claims(asset) && asset.state == State::Uploading;
				if (wanted && !p->upload.complete)
				{
					if (!impl.budget->Limits().uploadBytesPerFrame)
					{
						asset.error = "Texture upload blocked by frame budget";
						asset.state = State::Blocked;
					}
					while (frameBytes < impl.budget->Limits().uploadBytesPerFrame)
					{
						auto bytes = textures.PumpChunkedUpload(
						        p->upload,
						        impl.budget->Limits().uploadBytesPerFrame - frameBytes,
						        [ledger](std::uint64_t n) { return ledger->TryReserve(MemoryClass::Staging, n); },
						        [ledger](std::uint64_t n) { ledger->Release(MemoryClass::Staging, n); });
						if (!bytes)
						{
							asset.error = bytes.error().ToString();
							asset.state = State::Failed;
							break;
						}
						if (!*bytes)
						{
							asset.error = "Texture upload blocked by staging or frame budget";
							asset.state = State::Blocked;
							break;
						}
						frameBytes += *bytes;
					}
				}
				const bool publish = impl.Claims(asset) && asset.state == State::Uploading && p->upload.complete;
				if (publish ? !textures.IsChunkedUploadComplete(p->upload) : !textures.IsChunkedUploadIdle(p->upload))
				{
					++p;
					continue;
				}
				if (!publish && impl.Claims(asset) && (asset.state == State::Uploading || asset.state == State::Blocked))
				{
					++p;
					continue;
				}
				if (publish)
				{
					(void) ledger->Transfer(MemoryClass::GpuPending, MemoryClass::GpuResident, gpuBytes);
					auto handle = textures.PublishPrepared(
					        p->path,
					        p->colorSpace,
					        TextureResource{std::move(p->upload.texture)},
					        [ledger, gpuBytes]() { ledger->Release(MemoryClass::GpuRetiring, gpuBytes); },
					        [ledger, gpuBytes]() { (void) ledger->Transfer(MemoryClass::GpuResident, MemoryClass::GpuRetiring, gpuBytes); });
					if (!handle.IsValid() || handle == TextureHandle::Broken())
					{
						asset.error = "Texture registry publication failed";
						asset.state = State::Failed;
					}
					else
					{
						asset.textures.push_back(handle);
					}
				}
				else
				{
					textures.RetirePrepared(
					        TextureResource{std::move(p->upload.texture)},
					        [ledger, gpuBytes]() { ledger->Release(MemoryClass::GpuRetiring, gpuBytes); },
					        [ledger, gpuBytes]() { (void) ledger->Transfer(MemoryClass::GpuPending, MemoryClass::GpuRetiring, gpuBytes); });
				}
				const auto cpuBytes = p->cpuBytes;
				p = asset.pendingTextures.erase(p);
				ledger->Release(MemoryClass::CpuPending, cpuBytes);
			}
			if (asset.state == State::Uploading && asset.decoded && impl.Claims(asset) && dependenciesReady)
			{
				while (asset.pendingTextures.empty() && asset.nextTexture < asset.decoded->textures.size())
				{
					auto& prepared = asset.decoded->textures[asset.nextTexture];
					auto handle = textures.FindAndAcquire(prepared.path, prepared.colorSpace);
					if (handle.IsValid())
					{
						asset.textures.push_back(handle);
						++asset.nextTexture;
						continue;
					}
					auto sizes = textures.GetPreparedUploadSizes(prepared.texture, prepared.colorSpace);
					auto cpu = textures.GetPreparedUploadBytes(prepared.texture, prepared.colorSpace);
					if (!sizes || !cpu)
					{
						asset.error = !sizes ? sizes.error().ToString() : cpu.error().ToString();
						asset.state = State::Failed;
						break;
					}
					if (!impl.budget->TryReserve(MemoryClass::CpuPending, *cpu))
					{
						if (impl.Required(asset))
						{
							impl.EvictPrefetch(asset);
						}
						asset.error = "Texture mip CPU budget exceeded";
						asset.state = State::Blocked;
						break;
					}
					if (!impl.budget->TryReserve(MemoryClass::GpuPending, sizes->gpuBytes))
					{
						impl.budget->Release(MemoryClass::CpuPending, *cpu);
						if (impl.Required(asset))
						{
							impl.EvictPrefetch(asset);
						}
						asset.error = "GPU streaming budget exceeded";
						asset.state = State::Blocked;
						break;
					}
					auto ledger = impl.budget;
					auto pending = textures.BeginChunkedUpload(
					        prepared.texture,
					        prepared.path,
					        prepared.colorSpace,
					        [ledger, bytes = sizes->gpuBytes]() { ledger->Release(MemoryClass::GpuRetiring, bytes); },
					        [ledger, bytes = sizes->gpuBytes]() { (void) ledger->Transfer(MemoryClass::GpuPending, MemoryClass::GpuRetiring, bytes); });
					if (!pending)
					{
						ledger->Release(MemoryClass::CpuPending, *cpu);
						asset.error = pending.error().ToString();
						asset.state = State::Failed;
						break;
					}
					asset.pendingTextures.push_back({prepared.path, prepared.colorSpace, sizes->gpuBytes, *cpu, std::move(*pending)});
					++asset.nextTexture;
				}
				if (asset.state == State::Uploading && asset.nextTexture == asset.decoded->textures.size() && asset.pendingTextures.empty())
				{
					if (asset.decoded->model && !asset.model)
					{
						if (!asset.modelUpload)
						{
							const auto sizes = AssetManager::PreparedModelUploadSizes(*asset.decoded->model);
							if (!sizes)
							{
								asset.error = sizes.error().ToString();
								asset.state = State::Failed;
							}
							else if (!impl.budget->TryReserve(MemoryClass::GpuPending, sizes->gpuBytes))
							{
								if (impl.Required(asset))
								{
									impl.EvictPrefetch(asset);
								}
								asset.error = "Model exceeds available GPU streaming budget";
								asset.state = State::Blocked;
							}
							else
							{
								asset.gpu += sizes->gpuBytes;
								auto upload = impl.manager.BeginPreparedModel(*asset.decoded->model, asset.path);
								if (!upload)
								{
									asset.error = upload.error().ToString();
									asset.state = State::Failed;
								}
								else
								{
									asset.modelUpload = std::move(*upload);
								}
							}
						}
						if (asset.state == State::Uploading && asset.modelUpload && !asset.modelStaging)
						{
							auto allowance = impl.budget->Limits().uploadBytesPerFrame - frameBytes;
							auto requirement = impl.manager.PreparedModelNextStagingRequirement(*asset.modelUpload, allowance);
							if (!requirement)
							{
								asset.error = requirement.error().ToString();
								asset.state = State::Failed;
							}
							else
							{
								bool reserved = !*requirement;
								while (!reserved && requirement)
								{
									reserved = impl.budget->TryReserve(MemoryClass::Staging, *requirement);
									if (reserved || allowance < 8)
									{
										break;
									}
									allowance = (allowance / 2) & ~std::uint64_t{3};
									requirement = impl.manager.PreparedModelNextStagingRequirement(*asset.modelUpload, allowance);
								}
								if (!requirement)
								{
									asset.error = requirement.error().ToString();
									asset.state = State::Failed;
								}
								else if (!reserved)
								{
									asset.error = "Model staging budget exceeded";
									asset.state = State::Blocked;
								}
								else
								{
									asset.modelStaging = *requirement;
									auto ledger = impl.budget;
									auto progress = impl.manager.PumpPreparedModel(*asset.modelUpload, allowance, [ledger, bytes = *requirement]() { ledger->Release(MemoryClass::Staging, bytes); });
									asset.ticket = impl.manager.PreparedModelLatestTicket(*asset.modelUpload);
									if (!progress)
									{
										asset.error = progress.error().ToString();
										asset.state = State::Failed;
									}
									else
									{
										frameBytes += progress->bytes;
										if (progress->complete && (!asset.ticket || uploads.IsUploadComplete(asset.ticket)))
										{
											asset.model = std::make_shared<Impl::ModelOwner>();
											asset.model->manager = &impl.manager;
											asset.model->model = impl.manager.TakePreparedModel(*asset.modelUpload);
											asset.modelUpload.reset();
										}
									}
								}
							}
						}
					}
					else if (!asset.decoded->model && !asset.documentPublished)
					{
						auto& decoded = *asset.decoded;
						bool owned = false;
						if (decoded.atlas && impl.sprites)
						{
							owned = impl.sprites->PublishAtlas(asset.path, std::move(*decoded.atlas));
							if (owned)
							{
								asset.document = Impl::Asset::Document::Atlas;
							}
						}
						else if (decoded.animation && impl.sprites)
						{
							owned = impl.sprites->PublishAnimation(asset.path, std::move(*decoded.animation));
							if (owned)
							{
								asset.document = Impl::Asset::Document::Animation;
							}
						}
						else if (decoded.tileSet && impl.tiles)
						{
							owned = impl.tiles->PublishTileSet(asset.path, std::move(*decoded.tileSet));
							if (owned)
							{
								asset.document = Impl::Asset::Document::TileSet;
							}
						}
						else if (decoded.tileMap && impl.tiles)
						{
							owned = impl.tiles->PublishTileMap(asset.path, std::move(*decoded.tileMap));
							if (owned)
							{
								asset.document = Impl::Asset::Document::TileMap;
							}
						}
						else if (decoded.material)
						{
							auto result = impl.manager.PublishStreamedMaterial(asset.identity, *decoded.material);
							if (!result)
							{
								asset.error = result.error().ToString();
								asset.state = State::Failed;
							}
							else if (*result)
							{
								asset.document = Impl::Asset::Document::Material;
							}
						}
						else if (decoded.font && impl.uiRenderer)
						{
							if (!asset.fontUpload)
							{
								const auto bytes = impl.uiRenderer->FontUploadBytes(*decoded.font);
								if (!bytes)
								{
									asset.error = "Invalid font GPU allocation";
									asset.state = State::Failed;
								}
								else if (!impl.budget->TryReserve(MemoryClass::GpuPending, bytes))
								{
									if (impl.Required(asset))
									{
										impl.EvictPrefetch(asset);
									}
									asset.error = "Font exceeds GPU streaming budget";
									asset.state = State::Blocked;
								}
								else
								{
									auto upload = impl.uiRenderer->BeginStreamedFontUpload(*decoded.font);
									if (!upload)
									{
										impl.budget->Release(MemoryClass::GpuPending, bytes);
										asset.error = upload.error().ToString();
										asset.state = State::Failed;
									}
									else
									{
										asset.fontGpu = bytes;
										asset.fontUpload.emplace(std::move(*upload));
									}
								}
							}
							if (asset.fontUpload && asset.state == State::Uploading)
							{
								auto ledger = impl.budget;
								if (!impl.budget->Limits().uploadBytesPerFrame && !asset.fontUpload->complete)
								{
									asset.error = "Font upload blocked by frame budget";
									asset.state = State::Blocked;
								}
								while (!asset.fontUpload->complete && frameBytes < impl.budget->Limits().uploadBytesPerFrame)
								{
									auto bytes = impl.uiRenderer->PumpStreamedFontUpload(
									        *asset.fontUpload,
									        *decoded.font,
									        impl.budget->Limits().uploadBytesPerFrame - frameBytes,
									        [ledger](std::uint64_t n) { return ledger->TryReserve(MemoryClass::Staging, n); },
									        [ledger](std::uint64_t n) { ledger->Release(MemoryClass::Staging, n); });
									if (!bytes)
									{
										asset.error = bytes.error().ToString();
										asset.state = State::Failed;
										break;
									}
									if (!*bytes)
									{
										asset.error = "Font upload blocked by staging or frame budget";
										asset.state = State::Blocked;
										break;
									}
									frameBytes += *bytes;
								}
								if (asset.fontUpload->complete && uploads.IsUploadComplete(asset.fontUpload->ticket))
								{
									auto result = impl.uiRenderer->FinishStreamedFontUpload(asset.path, std::move(*decoded.font), *asset.fontUpload);
									if (!result)
									{
										asset.error = result.error().ToString();
										asset.state = State::Failed;
									}
									else if (*result)
									{
										(void) ledger->Transfer(MemoryClass::GpuPending, MemoryClass::GpuResident, asset.fontGpu);
										asset.document = Impl::Asset::Document::Font;
										asset.fontUpload.reset();
										asset.fontReady = true;
									}
									else
									{
										const auto bytes = asset.fontGpu;
										impl.uiRenderer->DiscardStreamedFontUpload(*asset.fontUpload, [ledger, bytes]() { ledger->Release(MemoryClass::GpuPending, bytes); });
										asset.fontUpload.reset();
										asset.fontGpu = 0;
										asset.fontReady = true;
									}
								}
							}
						}
						else if (!decoded.bytes.empty() && impl.audio)
						{
							const auto bytes = decoded.bytes.size();
							auto ledger = impl.budget;
							auto result = impl.audio->PublishStreamedClip(asset.path, std::move(decoded.bytes), [ledger, bytes]() { ledger->Release(MemoryClass::CpuResident, bytes); });
							if (!result)
							{
								asset.error = result.error().ToString();
								asset.state = State::Failed;
							}
							else if (*result)
							{
								(void) ledger->Transfer(asset.cpuClass, MemoryClass::CpuResident, bytes);
								asset.cpu -= bytes;
								asset.retainedAudioCpu = bytes;
								asset.document = Impl::Asset::Document::Audio;
							}
						}
						else if (decoded.atlas || decoded.animation || decoded.tileSet || decoded.tileMap || decoded.font || !decoded.bytes.empty())
						{
							asset.error = "Canonical document store is unavailable";
							asset.state = State::Failed;
						}
						asset.documentPublished = asset.state == State::Uploading && (!decoded.font || asset.fontReady);
					}
					if (asset.state == State::Uploading && (asset.decoded->model ? static_cast<bool>(asset.model) : asset.documentPublished) && (!asset.ticket || uploads.IsUploadComplete(asset.ticket)))
					{
						(void) impl.budget->Transfer(asset.cpuClass, MemoryClass::CpuResident, asset.cpu);
						asset.cpuClass = MemoryClass::CpuResident;
						(void) impl.budget->Transfer(asset.gpuClass, MemoryClass::GpuResident, asset.gpu);
						asset.gpuClass = MemoryClass::GpuResident;
						asset.state = State::Loaded;
					}
				}
			}
			if (asset.modelStaging && (!asset.ticket || uploads.IsUploadComplete(asset.ticket)))
			{
				asset.modelStaging = 0;
			}
			if (!impl.Claims(asset) && !asset.job && asset.pendingTextures.empty() && (!asset.ticket || uploads.IsUploadComplete(asset.ticket)) && (!asset.fontUpload || !asset.fontUpload->ticket || uploads.IsUploadComplete(asset.fontUpload->ticket))
			        && asset.documentLease.use_count() == 1 && (!asset.model || (!impl.manager.HasLiveModelUsers(asset.model->model) && asset.model.use_count() == 1))
			        && !(asset.document == Impl::Asset::Document::TileMap && impl.tiles && impl.tiles->IsTileMapDirty(asset.path)))
			{
				impl.Dispose(asset);
				it = impl.assets.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (const auto& asset: scheduleDependencies)
		{
			if (!asset->decoded || !impl.Claims(*asset))
			{
				continue;
			}
			for (const auto& reference: asset->decoded->dependencies)
			{
				const auto path = io::FileSystem::ResolveAssetPath(reference);
				const auto identity = path.empty() ? std::string{} : io::FileSystem::AssetReference(path);
				bool texture = false;
				for (const auto& prepared: asset->decoded->textures)
				{
					if (AssetIdentity(prepared.path) == identity)
					{
						texture = true;
						break;
					}
				}
				if (texture)
				{
					continue;
				}
				if (identity.empty() || identity == asset->identity)
				{
					asset->error = "Invalid or cyclic dependency: " + reference;
					asset->state = State::Failed;
					break;
				}
				bool duplicate = false;
				for (auto id: asset->dependencies)
				{
					if (impl.requests.at(id).asset->identity == identity)
					{
						duplicate = true;
						break;
					}
				}
				if (duplicate)
				{
					continue;
				}
				auto child = impl.assets.find(identity);
				if (child != impl.assets.end())
				{
					std::unordered_set<const Impl::Asset*> visited;
					std::function<bool(const Impl::Asset&)> reaches = [&](const Impl::Asset& node)
					{
						if (&node == asset.get())
						{
							return true;
						}
						if (!visited.insert(&node).second)
						{
							return false;
						}
						for (auto id: node.dependencies)
						{
							auto request = impl.requests.find(id);
							if (request != impl.requests.end() && reaches(*request->second.asset))
							{
								return true;
							}
						}
						return false;
					};
					if (reaches(*child->second))
					{
						asset->error = "Cyclic asset dependency: " + path;
						asset->state = State::Failed;
						break;
					}
				}
				const auto id = Request(identity, impl.Priority(*asset), impl.Required(*asset));
				if (!id)
				{
					asset->error = "Dependency admission failed: " + path;
					asset->state = State::Blocked;
					break;
				}
				asset->dependencies.push_back(id);
			}
			if (asset->state == State::Uploading)
			{
				asset->dependenciesScheduled = true;
			}
		}
	}

	void AssetStreaming::Shutdown()
	{
		if (!m_impl || m_impl->shutdown)
		{
			return;
		}
		for (auto& [path, asset]: m_impl->assets)
		{
			if (asset->job)
			{
				asset->job->cancelled.store(true, std::memory_order_release);
			}
		}
		io::FileSystem::Flush();
		m_impl->requests.clear();
		for (auto& [path, asset]: m_impl->assets)
		{
			if (asset->job)
			{
				auto job = asset->job;
				std::lock_guard lock(job->mutex);
				asset->cpu = job->reserved;
				asset->job.reset();
			}
			std::uint64_t pendingCpu = 0;
			for (auto& pending: asset->pendingTextures)
			{
				m_impl->manager.GetTextureRegistry().WaitChunkedUpload(pending.upload);
				auto ledger = m_impl->budget;
				const auto bytes = pending.gpuBytes;
				TextureRegistry::RetirePrepared(
				        TextureResource{std::move(pending.upload.texture)},
				        [ledger, bytes]() { ledger->Release(MemoryClass::GpuRetiring, bytes); },
				        [ledger, bytes]() { (void) ledger->Transfer(MemoryClass::GpuPending, MemoryClass::GpuRetiring, bytes); });
				pendingCpu += pending.cpuBytes;
			}
			asset->pendingTextures.clear();
			m_impl->budget->Release(MemoryClass::CpuPending, pendingCpu);
			if (asset->ticket)
			{
				m_impl->manager.GetUploadContext().WaitForUpload(asset->ticket);
			}
			if (asset->fontUpload && asset->fontUpload->ticket)
			{
				m_impl->manager.GetUploadContext().WaitForUpload(asset->fontUpload->ticket);
			}
			m_impl->Dispose(*asset);
		}
		m_impl->assets.clear();
		m_impl->shutdown = true;
	}

	std::string AssetStreaming::GetDiagnosticsJson() const
	{
		nlohmann::json result;
		const auto limits = m_impl->budget->Limits();
		result["cpuBudgetBytes"] = limits.cpuBytes;
		result["gpuBudgetBytes"] = limits.gpuBytes;
		result["cpuResidentBytes"] = m_impl->budget->Bytes(MemoryClass::CpuResident);
		result["cpuPendingBytes"] = m_impl->budget->Bytes(MemoryClass::CpuPending);
		result["cpuRetiringBytes"] = m_impl->budget->Bytes(MemoryClass::CpuRetiring);
		result["gpuResidentBytes"] = m_impl->budget->Bytes(MemoryClass::GpuResident);
		result["gpuPendingBytes"] = m_impl->budget->Bytes(MemoryClass::GpuPending);
		result["gpuRetiringBytes"] = m_impl->budget->Bytes(MemoryClass::GpuRetiring);
		result["stagingBytes"] = m_impl->budget->Bytes(MemoryClass::Staging);
		result["uploadBytesThisFrame"] = m_impl->uploadBytesThisFrame;
		result["uploadBudgetBytesPerFrame"] = limits.uploadBytesPerFrame;
		result["cpuResidentAccounting"] = "conservative reserved payload and decode workspace, not measured live heap";
		result["gpuAssetAccounting"] = "texture VMA allocations from referenced registry handles plus model/font reservations; shared asset rows are not additive";
		result["accountingScope"] = "streaming-owned allocations; renderer baseline and driver overhead excluded";
		result["assets"] = nlohmann::json::array();
		std::unordered_map<std::uint32_t, std::uint64_t> textureBytes;
		for (const auto& texture: gpu::ResourceRegistry::ListDebugTextures())
		{
			if (texture.hasBindlessSampled)
			{
				textureBytes.emplace(texture.bindlessSampledSlot, texture.allocationBytes);
			}
		}
		const auto& textureRegistry = m_impl->manager.GetTextureRegistry();
		for (const auto& [path, asset]: m_impl->assets)
		{
			const char* type = "unknown";
			if (asset->decoded)
			{
				const auto& decoded = *asset->decoded;
				if (decoded.model)
				{
					type = "model";
				}
				else if (decoded.material)
				{
					type = "material";
				}
				else if (decoded.atlas)
				{
					type = "atlas";
				}
				else if (decoded.animation)
				{
					type = "animation";
				}
				else if (decoded.tileSet)
				{
					type = "tileset";
				}
				else if (decoded.tileMap)
				{
					type = "tilemap";
				}
				else if (decoded.font)
				{
					type = "font";
				}
				else if (!decoded.textures.empty())
				{
					type = "texture";
				}
				else if (!decoded.bytes.empty())
				{
					type = "audio";
				}
			}
			if (asset->document == Impl::Asset::Document::Audio)
			{
				type = "audio";
			}
			std::uint64_t gpuBytes = asset->gpu + asset->fontGpu;
			std::uint64_t cpuBytes = asset->cpu + asset->retainedAudioCpu;
			for (const auto& pending: asset->pendingTextures)
			{
				gpuBytes += pending.gpuBytes;
				cpuBytes += pending.cpuBytes;
			}
			std::unordered_set<std::uint32_t> seenSlots;
			bool gpuBytesKnown = true;
			for (auto handle: asset->textures)
			{
				const auto slot = textureRegistry.ResolveSlot(handle);
				if (!seenSlots.insert(slot).second)
				{
					continue;
				}
				const auto entry = textureBytes.find(slot);
				if (entry == textureBytes.end())
				{
					gpuBytesKnown = false;
				}
				else
				{
					gpuBytes += entry->second;
				}
			}
			nlohmann::json row = {{"reference", asset->identity},
			        {"resolvedPath", io::FileSystem::ResolveAssetPath(asset->identity)},
			        {"type", type},
			        {"state", static_cast<unsigned>(asset->state)},
			        {"priority", static_cast<unsigned>(m_impl->Priority(*asset))},
			        {"claims", m_impl->Claims(*asset)},
			        {"required", m_impl->Required(*asset)},
			        {"dependencies", asset->decoded ? asset->decoded->dependencies : std::vector<std::string>{}},
			        {"cpuBytes", cpuBytes},
			        {"gpuBytesKnown", gpuBytesKnown},
			        {"gpuBytesKnownSubtotal", gpuBytes},
			        {"reason", asset->error}};
			row["gpuBytes"] = gpuBytesKnown ? nlohmann::json(gpuBytes) : nlohmann::json(nullptr);
			result["assets"].push_back(std::move(row));
		}
		return result.dump();
	}
} // namespace aether
