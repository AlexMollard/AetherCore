#include "rendering/RenderGraph.hpp"

#include <algorithm>
#include <limits>

#include "utils/Logger.hpp"

namespace aether
{
	// -- PassBuilder ----------------------------------------------------------

	RenderGraph::PassBuilder::PassBuilder(RenderGraph& graph, std::size_t passIndex)
	      : m_graph(graph), m_passIndex(passIndex)
	{
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteColor(RGImage image, gpu::LoadOp loadOp, gpu::StoreOp storeOp, gpu::ClearValue clearValue)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].colorWrites.push_back(AttachmentRef{
		        .image = image,
		        .loadOp = loadOp,
		        .storeOp = storeOp,
		        .clearValue = clearValue,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteDepth(RGImage image, gpu::LoadOp loadOp, gpu::StoreOp storeOp, gpu::ClearValue clearValue)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].depthWrite = AttachmentRef{
		        .image = image,
		        .loadOp = loadOp,
		        .storeOp = storeOp,
		        .clearValue = clearValue,
		};
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadTexture(RGImage image)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::SampledRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadStorageImage(RGImage image)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteStorageImage(RGImage image)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::StorageWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadBuffer(RGBuffer buffer)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteBuffer(RGBuffer buffer)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadWriteBuffer(RGBuffer buffer)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::StorageReadWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadImageTransfer(RGImage image)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::TransferRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteImageTransfer(RGImage image)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].imageAccesses.push_back(ImageAccessRef{
		        .image = image,
		        .type = ImageAccessType::TransferWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ReadBufferTransfer(RGBuffer buffer)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::TransferRead,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::WriteBufferTransfer(RGBuffer buffer)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].bufferAccesses.push_back(BufferAccessRef{
		        .buffer = buffer,
		        .type = BufferAccessType::TransferWrite,
		});
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::Execute(std::function<void(PassContext&)> fn)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].execute = std::move(fn);
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ExecuteCompute(std::function<void(PassContext&)> fn)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].kind = PassKind::Compute;
		m_graph.m_passes[m_passIndex].execute = std::move(fn);
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::OnDebugDisabled(std::function<void(PassContext&)> fn)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].debugDisabledExecute = std::move(fn);
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::SetExtent(gpu::Extent2D extent)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].extentOverride = gpu::Extent2D{extent.width, extent.height};
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::SetQueueClass(QueueClass qc)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].queueClass = qc;
		m_graph.m_passes[m_passIndex].allowAsyncCompute = qc == QueueClass::AsyncCompute;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::DisableAsyncCompute()
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].allowAsyncCompute = false;
		m_graph.m_passes[m_passIndex].queueClass = QueueClass::Graphics;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::HasSideEffects(std::string reason)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		PassRecord& pass = m_graph.m_passes[m_passIndex];
		pass.hasSideEffects = true;
		pass.sideEffectReason = std::move(reason);
		m_graph.m_compileDirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::DependsOn(std::string passNamePrefix)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		m_graph.m_passes[m_passIndex].logicalDependencies.push_back(std::move(passNamePrefix));
		m_graph.m_compileDirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ProducesDrawList(PreparedDrawList drawList)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		if (!drawList.IsValid() || drawList.id >= m_graph.m_preparedDrawLists.size() || m_graph.m_preparedDrawLists[drawList.id].retired)
		{
			AE_WARN(LogCategory::Engine, "RenderGraph: pass '{}' tried to produce an invalid PreparedDrawList.", m_graph.m_passes[m_passIndex].name);
			return *this;
		}
		m_graph.m_passes[m_passIndex].producedDrawLists.push_back(drawList);
		PreparedDrawListRecord& record = m_graph.m_preparedDrawLists[drawList.id];
		if (record.producerPass != std::numeric_limits<std::size_t>::max() && record.producerPass != m_passIndex)
		{
			AE_WARN(LogCategory::Engine, "RenderGraph: PreparedDrawList '{}' already has producer '{}'; replacing with '{}'.", record.name, m_graph.m_passes[record.producerPass].name, m_graph.m_passes[m_passIndex].name);
		}
		record.producerPass = m_passIndex;
		m_graph.m_blackboard.MarkProduced<PreparedDrawList>(record.name, m_graph.m_passes[m_passIndex].name);
		m_graph.m_compileDirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ConsumesDrawList(PreparedDrawList drawList)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		if (!drawList.IsValid() || drawList.id >= m_graph.m_preparedDrawLists.size() || m_graph.m_preparedDrawLists[drawList.id].retired)
		{
			AE_WARN(LogCategory::Engine, "RenderGraph: pass '{}' tried to consume an invalid PreparedDrawList.", m_graph.m_passes[m_passIndex].name);
			return *this;
		}
		m_graph.m_passes[m_passIndex].consumedDrawLists.push_back(drawList);
		PreparedDrawListRecord& record = m_graph.m_preparedDrawLists[drawList.id];
		if (std::ranges::find(record.consumerPasses, m_passIndex) == record.consumerPasses.end())
		{
			record.consumerPasses.push_back(m_passIndex);
		}
		m_graph.m_blackboard.MarkConsumed<PreparedDrawList>(record.name, m_graph.m_passes[m_passIndex].name);
		m_graph.m_compileDirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ProducesProductRef(const FrameProductRef& product)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		PassRecord& pass = m_graph.m_passes[m_passIndex];
		pass.producedFrameProducts.push_back(product);
		m_graph.m_blackboard.MarkProduced(product.type, product.name, pass.name);
		m_graph.m_compileDirty = true;
		return *this;
	}

	RenderGraph::PassBuilder& RenderGraph::PassBuilder::ConsumesProductRef(const FrameProductRef& product)
	{
		std::scoped_lock lock(m_graph.m_debugStateMutex);
		PassRecord& pass = m_graph.m_passes[m_passIndex];
		pass.consumedFrameProducts.push_back(product);
		m_graph.m_blackboard.MarkConsumed(product.type, product.name, pass.name);
		m_graph.m_compileDirty = true;
		return *this;
	}

	// -- Pass management ------------------------------------------------------

	RenderGraph::PassBuilder RenderGraph::AddPass(std::string name, std::source_location loc)
	{
		std::scoped_lock lock(m_debugStateMutex);
		PassRecord rec{};
#ifndef NDEBUG
		rec.declaredAt = loc;
#else
		(void) loc;
#endif
		rec.name = std::move(name);
		m_passes.push_back(std::move(rec));
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	RenderGraph::PassBuilder RenderGraph::AddComputePass(std::string name, std::source_location loc)
	{
		std::scoped_lock lock(m_debugStateMutex);
		PassRecord rec{};
#ifndef NDEBUG
		rec.declaredAt = loc;
#else
		(void) loc;
#endif
		rec.name = std::move(name);
		rec.kind = PassKind::Compute;
		m_passes.push_back(std::move(rec));
		m_compileDirty = true;
		return PassBuilder{*this, m_passes.size() - 1};
	}

	RenderGraph::PassBuilder RenderGraph::AddFullscreenPass(FullscreenPassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddPass(std::move(desc.name), loc);
		pass.SetExtent(desc.extent);
		for (const FrameProductRef& product: desc.consumes)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.produces)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.color.IsValid())
		{
			pass.WriteColor(desc.color, desc.loadOp, desc.storeOp, desc.clearValue);
		}
		return pass;
	}

	RenderGraph::PassBuilder RenderGraph::AddDepthOnlyPass(DepthOnlyPassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddPass(std::move(desc.name), loc);
		pass.SetExtent(desc.extent);
		for (const FrameProductRef& product: desc.consumes)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.produces)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.draws.IsValid())
		{
			pass.ConsumesDrawList(desc.draws);
		}
		if (desc.depth.IsValid())
		{
			pass.WriteDepth(desc.depth, desc.loadOp, desc.storeOp, desc.clearValue);
		}
		return pass;
	}

	RenderGraph::PassBuilder RenderGraph::AddDrawQueuePass(DrawQueuePassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddPass(std::move(desc.name), loc);
		pass.SetExtent(desc.extent);
		for (const FrameProductRef& product: desc.consumes)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.produces)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.draws.IsValid())
		{
			pass.ConsumesDrawList(desc.draws);
		}
		if (desc.color.IsValid())
		{
			pass.WriteColor(desc.color, desc.colorLoadOp, desc.colorStoreOp);
		}
		if (desc.depth.IsValid())
		{
			pass.WriteDepth(desc.depth, desc.depthLoadOp, desc.depthStoreOp);
		}
		return pass;
	}

	RenderGraph::PassBuilder RenderGraph::AddQueuePreparePass(QueuePreparePassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddComputePass(std::move(desc.name), loc);
		for (const FrameProductRef& product: desc.consumesProducts)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.producesProducts)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.keepOnGraphicsQueue)
		{
			pass.DisableAsyncCompute();
		}
		pass.HasSideEffects(std::move(desc.sideEffectReason));
		if (desc.produces.IsValid())
		{
			pass.ProducesDrawList(desc.produces);
		}
		return pass;
	}

	RenderGraph::PassBuilder RenderGraph::AddComputeImagePass(ComputeImagePassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddComputePass(std::move(desc.name), loc);
		pass.SetExtent(desc.extent);
		for (const FrameProductRef& product: desc.consumes)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.produces)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.queueClass == QueueClass::AsyncCompute)
		{
			pass.SetAsyncCompute();
		}
		else
		{
			pass.DisableAsyncCompute();
		}
		if (desc.image.IsValid())
		{
			if (desc.readsStorageImage)
			{
				pass.ReadStorageImage(desc.image);
			}
			if (desc.writesStorageImage)
			{
				pass.WriteStorageImage(desc.image);
			}
		}
		return pass;
	}

	RenderGraph::PassBuilder RenderGraph::AddComputeBufferPass(ComputeBufferPassDesc desc, std::source_location loc)
	{
		PassBuilder pass = AddComputePass(std::move(desc.name), loc);
		pass.SetExtent(desc.extent);
		for (const FrameProductRef& product: desc.consumes)
		{
			pass.ConsumesProductRef(product);
		}
		for (const FrameProductRef& product: desc.produces)
		{
			pass.ProducesProductRef(product);
		}
		if (desc.queueClass == QueueClass::AsyncCompute)
		{
			pass.SetAsyncCompute();
		}
		else
		{
			pass.DisableAsyncCompute();
		}
		for (const RGBuffer buffer: desc.reads)
		{
			pass.ReadBuffer(buffer);
		}
		for (const RGBuffer buffer: desc.writes)
		{
			pass.WriteBuffer(buffer);
		}
		for (const RGBuffer buffer: desc.readWrites)
		{
			pass.ReadWriteBuffer(buffer);
		}
		return pass;
	}

	PreparedDrawList RenderGraph::CreatePreparedDrawList(std::string name)
	{
		std::scoped_lock lock(m_debugStateMutex);
		const auto id = static_cast<std::uint32_t>(m_preparedDrawLists.size());
		m_preparedDrawLists.push_back(PreparedDrawListRecord{.name = std::move(name)});
		PreparedDrawList drawList{id};
		(void) m_blackboard.Create<PreparedDrawList>(m_preparedDrawLists.back().name, drawList);
		m_compileDirty = true;
		return drawList;
	}

	void RenderGraph::RemovePreparedDrawList(PreparedDrawList drawList)
	{
		std::scoped_lock lock(m_debugStateMutex);
		if (!drawList.IsValid() || drawList.id >= m_preparedDrawLists.size())
		{
			return;
		}

		PreparedDrawListRecord& record = m_preparedDrawLists[drawList.id];
		m_blackboard.Remove<PreparedDrawList>(record.name);
		record.retired = true;
		record.producerPass = std::numeric_limits<std::size_t>::max();
		record.consumerPasses.clear();
		for (PassRecord& pass: m_passes)
		{
			std::erase_if(pass.producedDrawLists, [&](const PreparedDrawList candidate) { return candidate.id == drawList.id; });
			std::erase_if(pass.consumedDrawLists, [&](const PreparedDrawList candidate) { return candidate.id == drawList.id; });
		}
		m_compileDirty = true;
	}
} // namespace aether
