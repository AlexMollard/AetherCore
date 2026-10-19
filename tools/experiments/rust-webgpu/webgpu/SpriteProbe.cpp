#include "SpriteProbe.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <emscripten.h>
#include <webgpu/webgpu.h>

#include "SpriteShader.inc"

namespace
{
	constexpr uint32_t kSize = 64;
	constexpr uint32_t kRowBytes = 256;

	struct alignas(16) FrameData
	{
		float clipScale[4];
	};

	struct alignas(16) SpriteData
	{
		float rect[4];
		float tint[4];
	};

	static_assert(sizeof(FrameData) == 16 && alignof(FrameData) == 16);
	static_assert(sizeof(SpriteData) == 32 && offsetof(SpriteData, tint) == 16);
	WGPUInstance s_instance;
	WGPUAdapter s_adapter;
	WGPUDevice s_device;
	WGPUBuffer s_readback;
	bool s_pixelsDone = false;
	bool s_scopeDone = false;
	bool s_shaderDone = false;
	bool s_failed = false;

	WGPUStringView Text(const char* value)
	{
		return {value, std::strlen(value)};
	}

	// clang-format off
	EM_JS(void, SetFailure, (const char* phase, const char* message, size_t length), {
		const result = globalThis.feasibilityResult;
		const failurePhase = UTF8ToString(phase);
		const failureCode = failurePhase === 'adapter' ? 'WEBGPU_UNAVAILABLE' : 'WEBGPU_' + failurePhase.toUpperCase().replaceAll('-', '_');
		if (result.status !== 'failed')
		{
			result.phase = failurePhase;
			result.code = failureCode;
		}
		result.status = 'failed';
		result.diagnostics.push({code : failureCode, phase : failurePhase, message : UTF8ToString(message, length)});
		result.elapsedMs = performance.now() - result.startedMs;
	});
	EM_JS(void, InitializeResult, (), {
		globalThis.feasibilityResult =
		        {status : 'running', phase : 'adapter', startedMs : performance.now(), diagnostics : [], browser : navigator.userAgent, secureContext : isSecureContext, layouts : {frameBytes : 16, spriteBytes : 32, tintOffset : 16}};
	});
	EM_JS(void, ReportPixels, (uint32_t mismatches, const uint8_t* bytes), {
		const result = globalThis.feasibilityResult;
		result.pixels = {width : 64, height : 64, tested : 4096, mismatches, format : 'rgba8unorm', bytesPerRow : 256};
		result.pixels.samples = [ [16,16], [48,16], [16,48], [48,48] ].map(([x,y]) => Array.from(HEAPU8.subarray(bytes + y*256 + x*4, bytes + y*256 + x*4 + 4)));
	});
	EM_JS(void, ReportShader, (uint32_t errors, uint32_t messages), { globalThis.feasibilityResult.shader = {compiler : 'Slang', target : 'wgsl', errors, messages}; });
	EM_JS(void,
	        ReportAdapter,
	        (const char* vendor, size_t vendorLength, const char* architecture, size_t architectureLength, const char* device, size_t deviceLength, const char* description, size_t descriptionLength, int adapterType, int backendType),
	        {
		        globalThis.feasibilityResult.adapter = {
			        vendor : UTF8ToString(vendor, vendorLength),
			        architecture : UTF8ToString(architecture, architectureLength),
			        device : UTF8ToString(device, deviceLength),
			        description : UTF8ToString(description, descriptionLength),
			        adapterType,
			        backendType,
			        isFallbackAdapter : adapterType === 3
		        };
	        });
	// clang-format on

	void Fail(const char* phase, WGPUStringView message)
	{
		s_failed = true;
		SetFailure(phase, message.data ? message.data : "", message.data ? message.length : 0);
	}

	void TryFinish()
	{
		if (!s_failed && s_pixelsDone && s_scopeDone && s_shaderDone)
		{
			EM_ASM({
				globalThis.feasibilityResult.status = 'passed';
				globalThis.feasibilityResult.phase = 'complete';
				globalThis.feasibilityResult.elapsedMs = performance.now() - globalThis.feasibilityResult.startedMs;
			});
		}
	}

	WGPUBuffer Buffer(WGPUBufferUsage usage, const void* data, size_t size)
	{
		WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
		descriptor.usage = usage | WGPUBufferUsage_CopyDst;
		descriptor.size = size;
		auto buffer = wgpuDeviceCreateBuffer(s_device, &descriptor);
		if (data)
		{
			wgpuQueueWriteBuffer(wgpuDeviceGetQueue(s_device), buffer, 0, data, size);
		}
		return buffer;
	}

	void PixelsReady(WGPUMapAsyncStatus status, WGPUStringView message, void*, void*)
	{
		if (status != WGPUMapAsyncStatus_Success)
		{
			Fail("readback", message);
			return;
		}
		const auto* bytes = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(s_readback, 0, kRowBytes * kSize));
		if (!bytes)
		{
			Fail("readback", Text("Mapped readback buffer returned no memory"));
			return;
		}
		// Texture rows are red/green, then blue/yellow. Sprite UV zero is canvas top-left.
		constexpr uint8_t expected[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
		uint32_t mismatches = 0;
		for (uint32_t y = 0; y < kSize; ++y)
		{
			for (uint32_t x = 0; x < kSize; ++x)
			{
				const auto quadrant = (y >= kSize / 2 ? 2 : 0) + (x >= kSize / 2 ? 1 : 0);
				if (std::memcmp(bytes + y * kRowBytes + x * 4, expected[quadrant], 4) != 0)
				{
					++mismatches;
				}
			}
		}
		ReportPixels(mismatches, bytes);
		wgpuBufferUnmap(s_readback);
		if (mismatches)
		{
			Fail("pixels", Text("Rendered pixels differ from all four expected texture quadrants"));
			return;
		}
		s_pixelsDone = true;
		TryFinish();
	}

	void Render()
	{
		wgpuDevicePushErrorScope(s_device, WGPUErrorFilter_Validation);
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = Text(kSpriteShader);
		if (EM_ASM_INT({ return new URLSearchParams(location.search).has('invalidShader'); }))
		{
			source.code = Text("deliberately invalid WGSL");
		}
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		auto shader = wgpuDeviceCreateShaderModule(s_device, &shaderDescriptor);
		WGPUCompilationInfoCallbackInfo compilationCallback = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
		compilationCallback.mode = WGPUCallbackMode_AllowSpontaneous;
		compilationCallback.callback = [](WGPUCompilationInfoRequestStatus status, const WGPUCompilationInfo* info, void*, void*)
		{
			if (status != WGPUCompilationInfoRequestStatus_Success || !info)
			{
				Fail("shader", Text("Could not obtain shader compilation diagnostics"));
				return;
			}
			uint32_t errors = 0;
			for (size_t i = 0; i < info->messageCount; ++i)
			{
				if (info->messages[i].type == WGPUCompilationMessageType_Error)
				{
					++errors;
					Fail("shader", info->messages[i].message);
				}
			}
			ReportShader(errors, info->messageCount);
			s_shaderDone = errors == 0;
			TryFinish();
		};
		wgpuShaderModuleGetCompilationInfo(shader, compilationCallback);

		WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = {};
		canvasSource.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
		canvasSource.selector = Text("#canvas");
		WGPUSurfaceDescriptor surfaceDescriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
		surfaceDescriptor.nextInChain = &canvasSource.chain;
		auto surface = wgpuInstanceCreateSurface(s_instance, &surfaceDescriptor);
		WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
		configuration.device = s_device;
		configuration.format = WGPUTextureFormat_RGBA8Unorm;
		configuration.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
		configuration.width = kSize;
		configuration.height = kSize;
		configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
		wgpuSurfaceConfigure(surface, &configuration);

		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
		target.format = configuration.format;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = shader;
		fragment.entryPoint = Text("fragmentMain");
		fragment.targetCount = 1;
		fragment.targets = &target;
		WGPURenderPipelineDescriptor pipelineDescriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		pipelineDescriptor.vertex.module = shader;
		pipelineDescriptor.vertex.entryPoint = Text("vertexMain");
		pipelineDescriptor.fragment = &fragment;
		pipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		auto pipeline = wgpuDeviceCreateRenderPipeline(s_device, &pipelineDescriptor);

		WGPUTextureDescriptor textureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
		textureDescriptor.dimension = WGPUTextureDimension_2D;
		textureDescriptor.size = {2, 2, 1};
		textureDescriptor.format = WGPUTextureFormat_RGBA8Unorm;
		textureDescriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		auto texture = wgpuDeviceCreateTexture(s_device, &textureDescriptor);
		constexpr uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = texture;
		WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		dataLayout.bytesPerRow = 8;
		dataLayout.rowsPerImage = 2;
		WGPUExtent3D textureSize = {2, 2, 1};
		wgpuQueueWriteTexture(wgpuDeviceGetQueue(s_device), &destination, texels, sizeof(texels), &dataLayout, &textureSize);
		WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
		samplerDescriptor.magFilter = WGPUFilterMode_Nearest;
		samplerDescriptor.minFilter = WGPUFilterMode_Nearest;
		auto sampler = wgpuDeviceCreateSampler(s_device, &samplerDescriptor);
		const FrameData frame = {{2, -2, -1, 1}};
		const SpriteData sprite = {{0, 0, 1, 1}, {1, 1, 1, 1}};
		auto frameBuffer = Buffer(WGPUBufferUsage_Uniform, &frame, sizeof(frame));
		auto spriteBuffer = Buffer(WGPUBufferUsage_Storage, &sprite, sizeof(sprite));
		WGPUBindGroupEntry entries[4] = {};
		entries[0].binding = 0;
		entries[0].buffer = frameBuffer;
		entries[0].size = sizeof(frame);
		entries[1].binding = 1;
		entries[1].buffer = spriteBuffer;
		entries[1].size = sizeof(sprite);
		entries[2].binding = 2;
		entries[2].textureView = wgpuTextureCreateView(texture, nullptr);
		entries[3].binding = 3;
		entries[3].sampler = sampler;
		WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindDescriptor.layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
		bindDescriptor.entryCount = 4;
		bindDescriptor.entries = entries;
		auto bindGroup = wgpuDeviceCreateBindGroup(s_device, &bindDescriptor);

		WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
		wgpuSurfaceGetCurrentTexture(surface, &current);
		if (!current.texture)
		{
			Fail("surface", Text("Canvas surface returned no texture"));
			return;
		}
		WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		color.view = wgpuTextureCreateView(current.texture, nullptr);
		color.loadOp = WGPULoadOp_Clear;
		color.storeOp = WGPUStoreOp_Store;
		color.clearValue = {0, 0, 0, 1};
		WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDescriptor.colorAttachmentCount = 1;
		passDescriptor.colorAttachments = &color;
		auto encoder = wgpuDeviceCreateCommandEncoder(s_device, nullptr);
		auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
		wgpuRenderPassEncoderSetPipeline(pass, pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
		wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
		wgpuRenderPassEncoderEnd(pass);
		s_readback = Buffer(WGPUBufferUsage_MapRead, nullptr, kRowBytes * kSize);
		WGPUTexelCopyTextureInfo renderedTexture = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		renderedTexture.texture = current.texture;
		WGPUTexelCopyBufferInfo readbackTarget = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		readbackTarget.buffer = s_readback;
		readbackTarget.layout.bytesPerRow = kRowBytes;
		readbackTarget.layout.rowsPerImage = kSize;
		WGPUExtent3D renderSize = {kSize, kSize, 1};
		wgpuCommandEncoderCopyTextureToBuffer(encoder, &renderedTexture, &readbackTarget, &renderSize);
		auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuQueueSubmit(wgpuDeviceGetQueue(s_device), 1, &commands);
		WGPUBufferMapCallbackInfo mapCallback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		mapCallback.mode = WGPUCallbackMode_AllowSpontaneous;
		mapCallback.callback = PixelsReady;
		wgpuBufferMapAsync(s_readback, WGPUMapMode_Read, 0, kRowBytes * kSize, mapCallback);
		WGPUPopErrorScopeCallbackInfo scopeCallback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
		scopeCallback.mode = WGPUCallbackMode_AllowSpontaneous;
		scopeCallback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void*, void*)
		{
			if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError)
			{
				Fail("validation", message);
				return;
			}
			s_scopeDone = true;
			TryFinish();
		};
		wgpuDevicePopErrorScope(s_device, scopeCallback);
	}
} // namespace

int main()
{
	InitializeResult();
	if (!EM_ASM_INT({ return !!navigator.gpu; }))
	{
		Fail("adapter", Text("WebGPU unavailable in this browser/context"));
		return 0;
	}
	// Deliberate null-adapter injection happens at the request boundary, without changing browser security.
	if (EM_ASM_INT({ return new URLSearchParams(location.search).has('forceNoAdapter'); }))
	{
		Fail("adapter", Text("Injected unavailable adapter"));
		return 0;
	}
	s_instance = wgpuCreateInstance(nullptr);
	WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	options.powerPreference = WGPUPowerPreference_HighPerformance;
	WGPURequestAdapterCallbackInfo adapterCallback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	adapterCallback.mode = WGPUCallbackMode_AllowSpontaneous;
	adapterCallback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void*, void*)
	{
		if (status != WGPURequestAdapterStatus_Success || !adapter)
		{
			Fail("adapter", message);
			return;
		}
		s_adapter = adapter;
		WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
		wgpuAdapterGetInfo(adapter, &info);
		ReportAdapter(info.vendor.data, info.vendor.length, info.architecture.data, info.architecture.length, info.device.data, info.device.length, info.description.data, info.description.length, info.adapterType, info.backendType);
		wgpuAdapterInfoFreeMembers(info);
		WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
		descriptor.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType, WGPUStringView message, void*, void*)
		{
			Fail("uncaptured-error", message);
		};
		descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
		descriptor.deviceLostCallbackInfo.callback = [](const WGPUDevice*, WGPUDeviceLostReason, WGPUStringView message, void*, void*)
		{
			Fail("device-lost", message);
		};
		WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
		deviceCallback.mode = WGPUCallbackMode_AllowSpontaneous;
		deviceCallback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void*, void*)
		{
			if (status != WGPURequestDeviceStatus_Success || !device)
			{
				Fail("device", message);
				return;
			}
			s_device = device;
			Render();
		};
		wgpuAdapterRequestDevice(adapter, &descriptor, deviceCallback);
	};
	wgpuInstanceRequestAdapter(s_instance, &options, adapterCallback);
	return 0;
}
