#include "WebGpuDevice.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>
#include "gpu/SpritePass.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		WGPUStringView Text(const char* text)
		{
			return {text, std::strlen(text)};
		}

		// clang-format off
  EM_JS(void, Diagnostic, (const char* code, const char* text, size_t length), {
   const item={code:UTF8ToString(code),message:UTF8ToString(text,length)};
   globalThis.aetherRuntime.diagnostics.push(item);
   globalThis.aetherRuntime.status='failed';
   if(globalThis.aetherRuntime.state!=='stopped') { globalThis.aetherRuntime.state=item.code==='WEBGPU_DEVICE_LOST'?'device-lost':'failed'; }
  });
  EM_JS(void, AdapterReport, (const char* vendor, size_t vendorSize, const char* architecture, size_t architectureSize, const char* text, size_t size, int type), {
   globalThis.aetherRuntime.adapter={vendor:UTF8ToString(vendor,vendorSize),architecture:UTF8ToString(architecture,architectureSize),description:UTF8ToString(text,size),adapterType:type,isFallbackAdapter:type===3};
  });
  EM_JS(void, Pixels, (const unsigned char* bytes, unsigned width, unsigned height, unsigned row), {
   const pixels=[];
   for(let y=0;y<height;++y) { pixels.push(...HEAPU8.subarray(bytes+y*row,bytes+y*row+width*4)); }
   globalThis.aetherRuntime.capture={width,height,pixels};
  });
		// clang-format on
	} // namespace

	struct WebGpuDevice::Impl
	{
		gpu::AsyncRenderLifecycle lifecycle;
		WGPUInstance instance = nullptr;
		WGPUAdapter adapter = nullptr;
		WGPUDevice device = nullptr;
		WGPUQueue queue = nullptr;
		WGPUSurface surface = nullptr;
		std::array<WGPURenderPipeline, 4> pipelines{};
		WGPUSampler sampler = nullptr;
		std::vector<WGPUTexture> textures;
		unsigned width = 0, height = 0;
		bool released = false;
		unsigned callbacks = 0;
		unsigned pendingFrames = 0;

		void Fail(const char* code, WGPUStringView message, bool lost = false)
		{
			lifecycle.Fail(lost);
			Diagnostic(code, message.data ? message.data : "", message.data ? message.length : 0);
		}

		void Release()
		{
			if (released || callbacks || pendingFrames || lifecycle.State() != gpu::RenderState::Stopped)
			{
				return;
			}
			released = true;
			for (auto texture: textures)
			{
				if (texture)
				{
					wgpuTextureDestroy(texture);
					wgpuTextureRelease(texture);
				}
			}
			textures.clear();
			for (auto pipeline: pipelines)
			{
				if (pipeline)
				{
					wgpuRenderPipelineRelease(pipeline);
				}
			}
			if (sampler)
			{
				wgpuSamplerRelease(sampler);
			}
			if (surface)
			{
				wgpuSurfaceUnconfigure(surface);
				wgpuSurfaceRelease(surface);
			}
			if (queue)
			{
				wgpuQueueRelease(queue);
			}
			if (device)
			{
				wgpuDeviceDestroy(device);
				wgpuDeviceRelease(device);
			}
			if (adapter)
			{
				wgpuAdapterRelease(adapter);
			}
			if (instance)
			{
				wgpuInstanceRelease(instance);
			}
			EM_ASM({ globalThis.aetherRuntime.state = 'stopped'; });
		}

		void Configure()
		{
			if (!surface || !width || !height)
			{
				return;
			}
			emscripten_set_canvas_element_size("#canvas", width, height);
			WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
			config.device = device;
			config.format = WGPUTextureFormat_RGBA8Unorm;
			config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
			config.width = width;
			config.height = height;
			config.alphaMode = WGPUCompositeAlphaMode_Opaque;
			wgpuSurfaceConfigure(surface, &config);
		}

		void CreateResources()
		{
			queue = wgpuDeviceGetQueue(device);
			WGPULimits limits = WGPU_LIMITS_INIT;
			wgpuDeviceGetLimits(device, &limits);
			if (limits.maxStorageBufferBindingSize < 128 * 65536 || limits.maxSampledTexturesPerShaderStage < 1 || limits.maxBindingsPerBindGroup < 4 || EM_ASM_INT({ return new URLSearchParams(location.search).has('forceLimits'); }))
			{
				Fail("WEBGPU_LIMITS", Text("Device lacks the supported sprite pass limits"));
				return;
			}
			std::ifstream input("/assets/sprite.wgsl");
			const std::string code(std::istreambuf_iterator<char>(input), {});
			if (code.empty())
			{
				Fail("WEBGPU_SHADER", Text("Missing sprite WGSL"));
				return;
			}
			wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
			WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
			source.code = {code.data(), code.size()};
			WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
			shaderDesc.nextInChain = &source.chain;
			auto shader = wgpuDeviceCreateShaderModule(device, &shaderDesc);
			for (unsigned blend = 0; blend < 4; ++blend)
			{
				WGPUBlendState blending = WGPU_BLEND_STATE_INIT;
				blending.color.operation = WGPUBlendOperation_Add;
				blending.alpha.operation = WGPUBlendOperation_Add;
				blending.color.srcFactor = blend == 2 ? WGPUBlendFactor_Dst : WGPUBlendFactor_SrcAlpha;
				blending.color.dstFactor = blend == 1 ? WGPUBlendFactor_One : blend == 2 ? WGPUBlendFactor_Zero : WGPUBlendFactor_OneMinusSrcAlpha;
				blending.alpha.srcFactor = WGPUBlendFactor_One;
				blending.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
				WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
				target.format = WGPUTextureFormat_RGBA8Unorm;
				target.blend = blend == 3 ? nullptr : &blending;
				WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
				fragment.module = shader;
				fragment.entryPoint = Text("fragmentMain");
				fragment.targetCount = 1;
				fragment.targets = &target;
				WGPURenderPipelineDescriptor pipeline = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
				pipeline.vertex.module = shader;
				pipeline.vertex.entryPoint = Text("vertexMain");
				pipeline.fragment = &fragment;
				pipeline.primitive.topology = WGPUPrimitiveTopology_TriangleList;
				pipelines[blend] = wgpuDeviceCreateRenderPipeline(device, &pipeline);
			}
			wgpuShaderModuleRelease(shader);
			WGPUSamplerDescriptor sample = WGPU_SAMPLER_DESCRIPTOR_INIT;
			sample.magFilter = WGPUFilterMode_Linear;
			sample.minFilter = WGPUFilterMode_Linear;
			sampler = wgpuDeviceCreateSampler(device, &sample);
			WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
			canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
			canvas.selector = Text("#canvas");
			WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
			descriptor.nextInChain = &canvas.chain;
			surface = wgpuInstanceCreateSurface(instance, &descriptor);
			Configure();
			++callbacks;
			WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
			callback.mode = WGPUCallbackMode_AllowSpontaneous;
			callback.userdata1 = this;
			callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* data, void*)
			{
				auto& self = *static_cast<Impl*>(data);
				--self.callbacks;
				if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError)
				{
					self.Fail("WEBGPU_VALIDATION", message);
				}
				else if (self.lifecycle.Ready())
				{
					EM_ASM({ globalThis.aetherRuntime.state = 'ready'; });
				}
				self.Release();
			};
			wgpuDevicePopErrorScope(device, callback);
		}

		WGPUBuffer Buffer(WGPUBufferUsage usage, const void* data, size_t bytes)
		{
			WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
			desc.size = bytes;
			desc.usage = usage | WGPUBufferUsage_CopyDst;
			auto buffer = wgpuDeviceCreateBuffer(device, &desc);
			if (data)
			{
				wgpuQueueWriteBuffer(queue, buffer, 0, data, bytes);
			}
			return buffer;
		}

		struct Frame
		{
			Impl* owner;
			WGPUBuffer uniform = nullptr, instances = nullptr, readback = nullptr;
			std::vector<WGPUBindGroup> bindings;
			unsigned width, height, row;

			void Finish()
			{
				for (auto binding: bindings)
				{
					wgpuBindGroupRelease(binding);
				}
				for (auto buffer: {uniform, instances, readback})
				{
					if (buffer)
					{
						wgpuBufferDestroy(buffer);
						wgpuBufferRelease(buffer);
					}
				}
				--owner->pendingFrames;
				EM_ASM(
				        {
					        aetherRuntime.inFlight = $0;
					        aetherRuntime.retired = (aetherRuntime.retired || 0) + 1;
				        },
				        owner->pendingFrames);
				owner->lifecycle.Complete();
				owner->Release();
				delete this;
			}
		};
	};

	WebGpuDevice::WebGpuDevice()
	      : m_impl(std::make_unique<Impl>())
	{
	}

	WebGpuDevice::~WebGpuDevice() = default;

	gpu::RenderState WebGpuDevice::State() const
	{
		return m_impl->lifecycle.State();
	}

	void WebGpuDevice::Initialize()
	{
		auto& self = *m_impl;
		if (!EM_ASM_INT({ return !!navigator.gpu && !new URLSearchParams(location.search).has('forceNoAdapter'); }))
		{
			self.Fail("WEBGPU_UNAVAILABLE", Text("WebGPU requires a supported adapter and secure context"));
			return;
		}
		self.instance = wgpuCreateInstance(nullptr);
		++self.callbacks;
		WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
		options.powerPreference = WGPUPowerPreference_HighPerformance;
		WGPURequestAdapterCallbackInfo callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
		callback.mode = WGPUCallbackMode_AllowSpontaneous;
		callback.userdata1 = &self;
		callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* data, void*)
		{
			auto& self = *static_cast<Impl*>(data);
			--self.callbacks;
			if (self.lifecycle.State() != gpu::RenderState::Initializing)
			{
				if (adapter)
				{
					wgpuAdapterRelease(adapter);
				}
				self.Release();
				return;
			}
			if (status != WGPURequestAdapterStatus_Success || !adapter)
			{
				self.Fail("WEBGPU_UNAVAILABLE", message);
				return;
			}
			self.adapter = adapter;
			WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
			wgpuAdapterGetInfo(adapter, &info);
			AdapterReport(info.vendor.data, info.vendor.length, info.architecture.data, info.architecture.length, info.description.data, info.description.length, info.adapterType);
			wgpuAdapterInfoFreeMembers(info);
			WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
			descriptor.uncapturedErrorCallbackInfo.userdata1 = &self;
			descriptor.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType, WGPUStringView message, void* data, void*)
			{
				static_cast<Impl*>(data)->Fail("WEBGPU_VALIDATION", message);
			};
			descriptor.deviceLostCallbackInfo.userdata1 = &self;
			descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
			descriptor.deviceLostCallbackInfo.callback = [](const WGPUDevice*, WGPUDeviceLostReason, WGPUStringView message, void* data, void*)
			{
				auto& self = *static_cast<Impl*>(data);
				if (self.lifecycle.State() != gpu::RenderState::Stopped && self.lifecycle.State() != gpu::RenderState::Stopping)
				{
					self.Fail("WEBGPU_DEVICE_LOST", message, true);
					EM_ASM({ globalThis.aetherRuntime.state = 'device-lost'; });
				}
			};
			++self.callbacks;
			WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
			deviceCallback.mode = WGPUCallbackMode_AllowSpontaneous;
			deviceCallback.userdata1 = &self;
			deviceCallback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* data, void*)
			{
				auto& self = *static_cast<Impl*>(data);
				--self.callbacks;
				if (self.lifecycle.State() != gpu::RenderState::Initializing)
				{
					if (device)
					{
						wgpuDeviceDestroy(device);
						wgpuDeviceRelease(device);
					}
					self.Release();
					return;
				}
				if (status != WGPURequestDeviceStatus_Success || !device)
				{
					self.Fail("WEBGPU_DEVICE", message);
					return;
				}
				self.device = device;
				self.CreateResources();
			};
			wgpuAdapterRequestDevice(adapter, &descriptor, deviceCallback);
		};
		wgpuInstanceRequestAdapter(self.instance, &options, callback);
	}

	bool WebGpuDevice::UploadTexture(unsigned slot, unsigned width, unsigned height, std::span<const std::uint8_t> rgba)
	{
		auto& self = *m_impl;
		if (State() != gpu::RenderState::Ready || slot > 255 || !width || !height || width > 4096 || height > 4096 || rgba.size() != std::uint64_t(width) * height * 4)
		{
			return false;
		}
		if (self.textures.size() <= slot)
		{
			self.textures.resize(slot + 1);
		}
		if (self.textures[slot])
		{
			return false;
		} // Immutable slots: replacement must wait for retirement.
		WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
		descriptor.dimension = WGPUTextureDimension_2D;
		descriptor.size = {width, height, 1};
		descriptor.format = WGPUTextureFormat_RGBA8Unorm;
		descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		auto texture = wgpuDeviceCreateTexture(self.device, &descriptor);
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = texture;
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = width * 4;
		layout.rowsPerImage = height;
		wgpuQueueWriteTexture(self.queue, &destination, rgba.data(), rgba.size(), &layout, &descriptor.size);
		self.textures[slot] = texture;
		return true;
	}

	void WebGpuDevice::Resize(unsigned width, unsigned height)
	{
		auto& self = *m_impl;
		if (width > 4096 || height > 4096)
		{
			self.Fail("WEBGPU_EXTENT", Text("Canvas exceeds supported 4096 extent"));
			return;
		}
		if (self.width != width || self.height != height)
		{
			self.width = width;
			self.height = height;
			if (State() == gpu::RenderState::Ready)
			{
				self.Configure();
			}
		}
	}

	bool WebGpuDevice::Draw(const RenderFramePacket& packet, bool capture)
	{
		auto& self = *m_impl;
		if (State() != gpu::RenderState::Ready || !self.width || !self.height || self.pendingFrames >= 3)
		{
			return false;
		}
		auto plan = gpu::PlanSpritePass(packet.render2D, 1024, 65536);
		if (!plan)
		{
			self.Fail("WEBGPU_SPRITES", Text(plan.error().c_str()));
			return false;
		}
		for (const auto& batch: plan->batches)
		{
			if (batch.texture >= self.textures.size() || !self.textures[batch.texture])
			{
				self.Fail("WEBGPU_TEXTURE", Text("Sprite references an unregistered texture"));
				return false;
			}
		}
		if (!self.lifecycle.Submit())
		{
			return false;
		}
		++self.pendingFrames;
		EM_ASM(
		        {
			        aetherRuntime.inFlight = $0;
			        aetherRuntime.peakInFlight = Math.max(aetherRuntime.peakInFlight || 0, $0);
			        aetherRuntime.draws = $1;
			        aetherRuntime.instances = $2;
		        },
		        self.pendingFrames,
		        plan->batches.size(),
		        plan->instances.size());
		auto* frame = new Impl::Frame{.owner = &self, .width = self.width, .height = self.height, .row = ((self.width * 4 + 255) / 256) * 256};
		const auto clip = glm::transpose(packet.proj * packet.view);
		const std::array<glm::vec4, 5> constants{clip[0], clip[1], clip[2], clip[3], glm::vec4(self.width, self.height, 0, 0)};
		frame->uniform = self.Buffer(WGPUBufferUsage_Uniform, constants.data(), sizeof(constants));
		frame->instances = self.Buffer(WGPUBufferUsage_Storage, plan->instances.empty() ? nullptr : plan->instances.data(), std::max(size_t(128), plan->instances.size() * 128));
		WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
		wgpuSurfaceGetCurrentTexture(self.surface, &current);
		if (!current.texture)
		{
			self.Fail("WEBGPU_SURFACE", Text("No canvas texture available"));
			frame->Finish();
			return false;
		}
		auto view = wgpuTextureCreateView(current.texture, nullptr);
		WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		color.view = view;
		color.loadOp = WGPULoadOp_Clear;
		color.storeOp = WGPUStoreOp_Store;
		color.clearValue = {0, 0, 0, 1};
		WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &color;
		auto encoder = wgpuDeviceCreateCommandEncoder(self.device, nullptr);
		auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
		for (const auto& batch: plan->batches)
		{
			auto pipeline = self.pipelines[batch.blend];
			auto textureView = wgpuTextureCreateView(self.textures[batch.texture], nullptr);
			WGPUBindGroupEntry entries[4] = {};
			entries[0].binding = 0;
			entries[0].buffer = frame->uniform;
			entries[0].size = sizeof(constants);
			entries[1].binding = 1;
			entries[1].buffer = frame->instances;
			entries[1].size = std::max(size_t(128), plan->instances.size() * 128);
			entries[2].binding = 2;
			entries[2].textureView = textureView;
			entries[3].binding = 3;
			entries[3].sampler = self.sampler;
			auto layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
			WGPUBindGroupDescriptor bind = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			bind.layout = layout;
			bind.entryCount = 4;
			bind.entries = entries;
			auto group = wgpuDeviceCreateBindGroup(self.device, &bind);
			frame->bindings.push_back(group);
			wgpuTextureViewRelease(textureView);
			wgpuBindGroupLayoutRelease(layout);
			wgpuRenderPassEncoderSetPipeline(pass, pipeline);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 6, batch.count, 0, batch.first);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		if (capture)
		{
			frame->readback = self.Buffer(WGPUBufferUsage_MapRead, nullptr, frame->row * frame->height);
			WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			source.texture = current.texture;
			WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
			destination.buffer = frame->readback;
			destination.layout.bytesPerRow = frame->row;
			destination.layout.rowsPerImage = frame->height;
			WGPUExtent3D extent = {frame->width, frame->height, 1};
			wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
		}
		auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuQueueSubmit(self.queue, 1, &commands);
		wgpuCommandBufferRelease(commands);
		wgpuCommandEncoderRelease(encoder);
		wgpuTextureViewRelease(view);
		wgpuTextureRelease(current.texture);
		WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
		done.mode = WGPUCallbackMode_AllowSpontaneous;
		done.userdata1 = frame;
		done.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView message, void* data, void*)
		{
			auto* frame = static_cast<Impl::Frame*>(data);
			if (status != WGPUQueueWorkDoneStatus_Success)
			{
				frame->owner->Fail("WEBGPU_QUEUE", message);
				frame->Finish();
				return;
			}
			if (!frame->readback)
			{
				frame->Finish();
				return;
			}
			WGPUBufferMapCallbackInfo map = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
			map.mode = WGPUCallbackMode_AllowSpontaneous;
			map.userdata1 = frame;
			map.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* data, void*)
			{
				auto* frame = static_cast<Impl::Frame*>(data);
				if (status != WGPUMapAsyncStatus_Success)
				{
					frame->owner->Fail("WEBGPU_READBACK", message);
				}
				else
				{
					const auto* bytes = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(frame->readback, 0, frame->row * frame->height));
					Pixels(bytes, frame->width, frame->height, frame->row);
					wgpuBufferUnmap(frame->readback);
				}
				frame->Finish();
			};
			wgpuBufferMapAsync(frame->readback, WGPUMapMode_Read, 0, frame->row * frame->height, map);
		};
		wgpuQueueOnSubmittedWorkDone(self.queue, done);
		return true;
	}

	void WebGpuDevice::Stop()
	{
		m_impl->lifecycle.Stop();
		m_impl->Release();
	}

	void WebGpuDevice::LoseDeviceForTest()
	{
		if (m_impl->device)
		{
			wgpuDeviceDestroy(m_impl->device);
		}
	}
} // namespace aether::webgpu
