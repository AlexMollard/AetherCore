# ImGui Multi-Viewport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let any Dear ImGui editor panel — including the live 3D Viewport — be torn out of the main window into an independent OS window and re-docked, integrated into AetherCore's threaded renderer.

**Architecture:** GLFW platform-window management stays on the producer thread (`UpdatePlatformWindows()`); secondary viewports are snapshotted (extended `ImguiFrameData`) and rendered on the render thread by a custom `ImguiViewportRenderer` that owns per-viewport swapchains (public `ImGui_ImplVulkanH_Window` helpers) and a small ImGui pipeline built from ImGui's own shaders, binding the backend's shared texture descriptors. The stock Vulkan renderer viewport hooks are suppressed. A live `SettingsService` toggle (`graphics.imguiViewports`, default on) flips `ImGuiConfigFlags_ViewportsEnable`. Structural viewport destroy reuses the `RunExclusive` quiesce.

**Tech Stack:** C++20, Vulkan (volk + VkBootstrap + VMA), GLFW, Dear ImGui v1.92.8-docking (stock `imgui_impl_glfw` + `imgui_impl_vulkan`), doctest, CMake (ninja-clang + vs2022-msvc presets).

**Spec:** `docs/superpowers/specs/2026-07-06-imgui-multiviewport-design.md`

---

## Conventions used by every task

- **Build (ninja-clang):** `cmake --build build-ninja-clang --target Engine App EngineTests`
- **Build (msvc, cross-platform gate):** `cmake --build build-vs2022-msvc --config RelWithDebInfo --target Engine App EngineTests`
- **Tests:** `build-ninja-clang/EngineTests` (doctest). Filter: `EngineTests --test-case="*viewport*"`.
- **Run the app:** launch the built `App` **from the build-tree root** (e.g. `build-ninja-clang/`) so `shaders://` resolves (per project convention); otherwise it segfaults at pipeline creation.
- New engine `.cpp`/`.hpp` auto-register via `GLOB_RECURSE CONFIGURE_DEPENDS` — **no** `src/engine/CMakeLists.txt` edit. A **new test** file must be added to `tests/CMakeLists.txt`.
- Vulkan validation layers must stay clean; enable **sync validation** when testing torn-out live-texture panels.

---

## Task 1: Settings toggle `graphics.imguiViewports` (TDD)

**Files:**
- Modify: `src/engine/utils/EngineSettings.hpp`
- Modify: `src/engine/utils/SettingsService.cpp`
- Modify: `src/engine/AetherCore.hpp`, `src/engine/AetherCore.cpp`
- Test: `tests/utils/EngineSettingsTests.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/utils/EngineSettingsTests.cpp`:

```cpp
TEST_CASE("imguiViewports defaults on and round-trips through Apply") {
    EngineSettings s; // compiled-in defaults
    CHECK(s.graphics.imguiViewports == true);

    EngineSettingsIO::Apply("[graphics]\nimguiViewports = false\n", s);
    CHECK(s.graphics.imguiViewports == false);

    const std::string toml = EngineSettingsIO::Serialize(s);
    EngineSettings rebuilt;
    EngineSettingsIO::Apply(toml, rebuilt);
    CHECK(rebuilt.graphics.imguiViewports == false);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-ninja-clang --target EngineTests && build-ninja-clang/EngineTests --test-case="imguiViewports*"`
Expected: **compile error** — `imguiViewports` is not a member of `EngineSettings::Graphics`.

- [ ] **Step 3: Add the setting field and reflection line**

In `src/engine/utils/EngineSettings.hpp`, add to the `Graphics` struct (after `asyncCompute`):

```cpp
		struct Graphics
		{
			bool vsync = true;
			bool fxaa = false;
			bool asyncCompute = true;
			bool imguiViewports = true; // tool panels tear out into OS windows
		} graphics;
```

And add to `ForEachSettingField`, grouped with the other `graphics.` lines:

```cpp
		f("graphics.imguiViewports", settings.graphics.imguiViewports);
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-ninja-clang --target EngineTests && build-ninja-clang/EngineTests --test-case="imguiViewports*"`
Expected: **PASS** (also confirm the existing `SerializeOverrides`/round-trip cases still pass: `build-ninja-clang/EngineTests`).

- [ ] **Step 5: Add the live-apply dispatch + engine forwarder**

In `src/engine/AetherCore.hpp`, after `SetVsync` (around line 89):

```cpp
		// Toggles ImGui multi-viewport at runtime by flipping
		// ImGuiConfigFlags_ViewportsEnable. Must be called on the producer thread.
		void SetImguiViewportsEnabled(bool enabled);
```

In `src/engine/AetherCore.cpp`, implement it near `SetVsync` (include `imgui/ImguiSubsystem.hpp` is already present):

```cpp
	void AetherCore::SetImguiViewportsEnabled(bool enabled)
	{
		m_settings.graphics.imguiViewports = enabled;
		if (m_imgui)
		{
			m_imgui->SetViewportsEnabled(enabled);
		}
	}
```

In `src/engine/utils/SettingsService.cpp`, add a branch inside `ApplyLive` (after the `graphics.vsync` branch):

```cpp
		else if (key == "graphics.imguiViewports")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetImguiViewportsEnabled(m_values.graphics.imguiViewports);
			}
		}
```

> `ImguiSubsystem::SetViewportsEnabled` does not exist yet — it is added in Task 6. Until then this references a not-yet-declared method; build Task 1 through Step 4 (settings + test) first and commit, then wire `SetImguiViewportsEnabled`'s body in Task 6's build. To keep Task 1 self-contained and compiling, temporarily guard the `m_imgui->SetViewportsEnabled` call by declaring the method in Task 6 before building the SettingsService change. (Order the work: Task 1 Steps 1-4 → commit; Steps 5 lands together with Task 6.)

- [ ] **Step 6: Commit**

```bash
git add tests/utils/EngineSettingsTests.cpp src/engine/utils/EngineSettings.hpp
git commit -m "feat(settings): add graphics.imguiViewports toggle (default on)"
```

---

## Task 2: Extend `ImguiFrameData` to N viewports

**Files:**
- Modify: `src/engine/imgui/ImguiFrameData.hpp`
- Modify: `src/engine/imgui/ImguiFrameData.cpp`

No unit test: constructing `ImDrawData`/`ImDrawList` requires a live ImGui context; this is covered by the runtime verification in Task 7. Verify by build.

- [ ] **Step 1: Add the `CapturedViewport` type and secondary storage**

In `src/engine/imgui/ImguiFrameData.hpp`, inside the class add a public nested struct and a member. Full new header:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

namespace aether
{
	// Thread-transferable snapshot of an ImGui frame's draw data (main viewport +
	// every secondary/OS-window viewport when multi-viewport is enabled).
	class ImguiFrameData
	{
	public:
		// One torn-out OS-window viewport, deep-copied for the render thread.
		struct CapturedViewport
		{
			ImGuiID id = 0;
			ImDrawData draw{};
			std::vector<ImDrawList*> owned;
			ImVec2 pos{0.0f, 0.0f};
			ImVec2 size{0.0f, 0.0f};
			ImVec2 fbScale{1.0f, 1.0f};
			void* platformHandle = nullptr; // GLFWwindow*, created on the producer thread
		};

		ImguiFrameData() = default;
		~ImguiFrameData();

		ImguiFrameData(const ImguiFrameData&) = delete;
		ImguiFrameData& operator=(const ImguiFrameData&) = delete;
		ImguiFrameData(ImguiFrameData&& other) noexcept;
		ImguiFrameData& operator=(ImguiFrameData&& other) noexcept;

		void Clear();
		void Capture(const ImDrawData* source); // main viewport
		void CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle);

		[[nodiscard]] bool HasDrawData() const noexcept
		{
			return m_drawData.Valid && m_drawData.CmdListsCount > 0 && m_drawData.TotalVtxCount > 0;
		}

		[[nodiscard]] ImDrawData* GetDrawData() noexcept { return &m_drawData; }
		[[nodiscard]] const ImDrawData* GetDrawData() const noexcept { return &m_drawData; }

		[[nodiscard]] const std::vector<CapturedViewport>& SecondaryViewports() const noexcept
		{
			return m_secondary;
		}

	private:
		void RebuildCommandListView();
		static void CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& owned);
		static void RebuildView(ImDrawData& dst, std::vector<ImDrawList*>& owned);

		ImDrawData m_drawData;
		std::vector<ImDrawList*> m_ownedLists;
		std::vector<CapturedViewport> m_secondary;
	};
} // namespace aether
```

- [ ] **Step 2: Factor the clone helper and implement secondary capture + lifetime**

Rewrite `src/engine/imgui/ImguiFrameData.cpp`:

```cpp
#include "imgui/ImguiFrameData.hpp"

namespace aether
{
	ImguiFrameData::~ImguiFrameData()
	{
		Clear();
	}

	ImguiFrameData::ImguiFrameData(ImguiFrameData&& other) noexcept
	      : m_drawData(other.m_drawData), m_ownedLists(std::move(other.m_ownedLists)), m_secondary(std::move(other.m_secondary))
	{
		RebuildView(m_drawData, m_ownedLists);
		for (auto& vp: m_secondary)
		{
			RebuildView(vp.draw, vp.owned);
		}
		other.m_drawData.Clear();
		other.m_ownedLists.clear();
		other.m_secondary.clear();
	}

	ImguiFrameData& ImguiFrameData::operator=(ImguiFrameData&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}
		Clear();
		m_drawData = other.m_drawData;
		m_ownedLists = std::move(other.m_ownedLists);
		m_secondary = std::move(other.m_secondary);
		RebuildView(m_drawData, m_ownedLists);
		for (auto& vp: m_secondary)
		{
			RebuildView(vp.draw, vp.owned);
		}
		other.m_drawData.Clear();
		other.m_ownedLists.clear();
		other.m_secondary.clear();
		return *this;
	}

	void ImguiFrameData::Clear()
	{
		for (ImDrawList* list: m_ownedLists)
		{
			IM_DELETE(list);
		}
		m_ownedLists.clear();
		m_drawData.Clear();

		for (auto& vp: m_secondary)
		{
			for (ImDrawList* list: vp.owned)
			{
				IM_DELETE(list);
			}
		}
		m_secondary.clear();
	}

	void ImguiFrameData::CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& owned)
	{
		dst.Clear();
		owned.clear();
		if (source == nullptr || !source->Valid)
		{
			return;
		}

		dst.Valid = true;
		dst.DisplayPos = source->DisplayPos;
		dst.DisplaySize = source->DisplaySize;
		dst.FramebufferScale = source->FramebufferScale;
		dst.OwnerViewport = source->OwnerViewport;
		dst.Textures = source->Textures;

		owned.reserve(static_cast<std::size_t>(source->CmdListsCount));
		for (const ImDrawList* sourceList: source->CmdLists)
		{
			if (sourceList == nullptr)
			{
				continue;
			}
			ImDrawList* clone = sourceList->CloneOutput();
			dst.CmdLists.push_back(clone);
			owned.push_back(clone);
			dst.CmdListsCount = dst.CmdLists.Size;
			dst.TotalVtxCount += clone->VtxBuffer.Size;
			dst.TotalIdxCount += clone->IdxBuffer.Size;
		}
	}

	void ImguiFrameData::RebuildView(ImDrawData& dst, std::vector<ImDrawList*>& owned)
	{
		dst.CmdLists.resize(0);
		dst.CmdLists.reserve(static_cast<int>(owned.size()));
		dst.CmdListsCount = 0;
		for (ImDrawList* list: owned)
		{
			dst.CmdLists.push_back(list);
			dst.CmdListsCount = dst.CmdLists.Size;
		}
	}

	void ImguiFrameData::Capture(const ImDrawData* source)
	{
		Clear();
		CloneInto(source, m_drawData, m_ownedLists);
	}

	void ImguiFrameData::CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle)
	{
		if (source == nullptr || !source->Valid || source->CmdListsCount <= 0)
		{
			return;
		}
		CapturedViewport vp;
		vp.id = id;
		vp.pos = pos;
		vp.size = size;
		vp.fbScale = fbScale;
		vp.platformHandle = platformHandle;
		CloneInto(source, vp.draw, vp.owned);
		m_secondary.push_back(std::move(vp));
	}

	void ImguiFrameData::RebuildCommandListView()
	{
		RebuildView(m_drawData, m_ownedLists);
	}
} // namespace aether
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-ninja-clang --target Engine`
Expected: **success** (no callers of the new API yet).

- [ ] **Step 4: Commit**

```bash
git add src/engine/imgui/ImguiFrameData.hpp src/engine/imgui/ImguiFrameData.cpp
git commit -m "feat(imgui): ImguiFrameData captures N viewports (deep-copied)"
```

---

## Task 3: `ImguiViewportRenderer` — shared ImGui pipeline

**Files:**
- Create: `src/engine/imgui/ImguiViewportRenderer.hpp`
- Create: `src/engine/imgui/ImguiViewportRenderer.cpp`

Reuses ImGui's own shaders + descriptor layout so `AddTexture` descriptor sets bind. Verify by build.

- [ ] **Step 1: Header with the class skeleton**

Create `src/engine/imgui/ImguiViewportRenderer.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "vulkan/volk.hpp"

namespace aether
{
	class VulkanContext;
	class ImguiFrameData;

	// Render-thread-owned renderer for ImGui secondary (torn-out OS-window) viewports.
	// Owns one ImGui pipeline (built from ImGui's own shaders, layout-compatible with the
	// backend's AddTexture descriptor sets) and one ImGui_ImplVulkanH_Window per viewport.
	// All methods run on the render thread EXCEPT RetireViewports, which the producer calls
	// inside a RunExclusive quiesce (render thread parked, GPU idle).
	class ImguiViewportRenderer
	{
	public:
		ImguiViewportRenderer() = default;
		~ImguiViewportRenderer();

		ImguiViewportRenderer(const ImguiViewportRenderer&) = delete;
		ImguiViewportRenderer& operator=(const ImguiViewportRenderer&) = delete;

		// colorFormat MUST equal the main swapchain format so secondary swapchains share
		// this pipeline.
		void Init(VulkanContext& vk, VkFormat colorFormat);
		void Shutdown();

		// Render + present every secondary viewport in the snapshot (render thread).
		void Render(const ImguiFrameData& frame);

		// Destroy swapchains/surfaces for viewports whose ids are no longer present.
		// Producer thread, inside RunExclusive only.
		void RetireViewports(const std::vector<ImGuiID>& departedIds);

		[[nodiscard]] bool IsInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

	private:
		struct PerViewport
		{
			ImGui_ImplVulkanH_Window window{};
			bool created = false;
		};

		void CreatePipeline(VkFormat colorFormat);
		void EnsureWindow(PerViewport& vp, void* glfwWindow, int width, int height);
		void RenderOne(PerViewport& vp, const ImDrawData& draw);
		void DestroyOne(PerViewport& vp);

		VulkanContext* m_vk = nullptr;
		VkDevice m_device = VK_NULL_HANDLE;
		VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;

		VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkSampler m_fontSampler = VK_NULL_HANDLE; // matches backend; only for completeness

		std::unordered_map<ImGuiID, PerViewport> m_viewports;
	};
} // namespace aether
```

- [ ] **Step 2: Implement `Init` + `CreatePipeline` using ImGui's shaders**

Create `src/engine/imgui/ImguiViewportRenderer.cpp`. Copy the two static SPIR-V arrays `__glsl_shader_vert_spv` and `__glsl_shader_frag_spv` **verbatim** from `build-ninja-clang/_deps/imgui-src/backends/imgui_impl_vulkan.cpp` (the `// glsl_shader.vert` / `// glsl_shader.frag` blocks near the top of the file, ~lines 160-235) into an anonymous namespace here. Then:

```cpp
#include "imgui/ImguiViewportRenderer.hpp"

#include "imgui/ImguiFrameData.hpp"
#include "utils/Logger.hpp"
#include "vulkan/VulkanContext.hpp"

#include <GLFW/glfw3.h>

namespace aether
{
	namespace
	{
		// __glsl_shader_vert_spv[] and __glsl_shader_frag_spv[] copied verbatim from
		// imgui_impl_vulkan.cpp (ImGui's own shaders; layout matches ImDrawVert +
		// push-constant scale/translate + set0 combined image sampler).
		// <PASTE the two `static uint32_t __glsl_shader_*_spv[] = { ... };` arrays here>

		VkShaderModule MakeModule(VkDevice device, const uint32_t* code, std::size_t bytes)
		{
			VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = bytes;
			info.pCode = code;
			VkShaderModule module = VK_NULL_HANDLE;
			vkCreateShaderModule(device, &info, nullptr, &module);
			return module;
		}
	} // namespace

	ImguiViewportRenderer::~ImguiViewportRenderer()
	{
		Shutdown();
	}

	void ImguiViewportRenderer::Init(VulkanContext& vk, VkFormat colorFormat)
	{
		m_vk = &vk;
		m_device = vk.GetDevice().device;
		m_colorFormat = colorFormat;
		CreatePipeline(colorFormat);
	}

	void ImguiViewportRenderer::CreatePipeline(VkFormat colorFormat)
	{
		// Sampler: use the backend's font sampler by leaving descriptor sampler immutable
		// unset; AddTexture descriptor sets carry their own sampler. We still create a
		// sampler to declare the layout binding identically to the backend.
		VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.maxLod = 1000.0f;
		vkCreateSampler(m_device, &samplerInfo, nullptr, &m_fontSampler);

		// set0 binding0: combined image sampler, fragment stage — identical to the backend
		// so ImGui_ImplVulkan_AddTexture() descriptor sets bind with this layout.
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo dsl{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dsl.bindingCount = 1;
		dsl.pBindings = &binding;
		vkCreateDescriptorSetLayout(m_device, &dsl, nullptr, &m_descriptorSetLayout);

		VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
		VkPipelineLayoutCreateInfo plci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plci.setLayoutCount = 1;
		plci.pSetLayouts = &m_descriptorSetLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges = &pcRange;
		vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipelineLayout);

		VkShaderModule vert = MakeModule(m_device, __glsl_shader_vert_spv, sizeof(__glsl_shader_vert_spv));
		VkShaderModule frag = MakeModule(m_device, __glsl_shader_frag_spv, sizeof(__glsl_shader_frag_spv));

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		VkVertexInputBindingDescription vbind{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
		VkVertexInputAttributeDescription vattr[3]{};
		vattr[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, IM_OFFSETOF(ImDrawVert, pos)};
		vattr[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, IM_OFFSETOF(ImDrawVert, uv)};
		vattr[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, IM_OFFSETOF(ImDrawVert, col)};
		VkPipelineVertexInputStateCreateInfo vin{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		vin.vertexBindingDescriptionCount = 1;
		vin.pVertexBindingDescriptions = &vbind;
		vin.vertexAttributeDescriptionCount = 3;
		vin.pVertexAttributeDescriptions = vattr;

		VkPipelineInputAssemblyStateCreateInfo ia{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo cb{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &blend;
		VkDynamicState dyn[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynInfo.dynamicStateCount = 2;
		dynInfo.pDynamicStates = dyn;

		VkPipelineRenderingCreateInfoKHR rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachmentFormats = &colorFormat;

		VkGraphicsPipelineCreateInfo pci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pci.pNext = &rendering;
		pci.stageCount = 2;
		pci.pStages = stages;
		pci.pVertexInputState = &vin;
		pci.pInputAssemblyState = &ia;
		pci.pViewportState = &vp;
		pci.pRasterizationState = &rs;
		pci.pMultisampleState = &ms;
		pci.pColorBlendState = &cb;
		pci.pDynamicState = &dynInfo;
		pci.layout = m_pipelineLayout;
		vkCreateGraphicsPipelines(m_device, m_vk->GetPipelineCache(), 1, &pci, nullptr, &m_pipeline);

		vkDestroyShaderModule(m_device, vert, nullptr);
		vkDestroyShaderModule(m_device, frag, nullptr);
	}

	void ImguiViewportRenderer::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		for (auto& [id, vp]: m_viewports)
		{
			DestroyOne(vp);
		}
		m_viewports.clear();
		if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
		if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
		if (m_descriptorSetLayout) vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
		if (m_fontSampler) vkDestroySampler(m_device, m_fontSampler, nullptr);
		m_pipeline = VK_NULL_HANDLE;
		m_pipelineLayout = VK_NULL_HANDLE;
		m_descriptorSetLayout = VK_NULL_HANDLE;
		m_fontSampler = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
	}

	// EnsureWindow / RenderOne / DestroyOne / Render / RetireViewports — added in Tasks 4-5.
	void ImguiViewportRenderer::Render(const ImguiFrameData&) {}
	void ImguiViewportRenderer::RetireViewports(const std::vector<ImGuiID>&) {}
	void ImguiViewportRenderer::EnsureWindow(PerViewport&, void*, int, int) {}
	void ImguiViewportRenderer::RenderOne(PerViewport&, const ImDrawData&) {}
	void ImguiViewportRenderer::DestroyOne(PerViewport&) {}
} // namespace aether
```

> Confirm `VulkanContext` exposes `GetDevice()`, `GetPipelineCache()`, `GetInstance()`, `GetPhysicalDevice()`, `GetGraphicsQueueFamily()`, `GetGraphicsQueue()` (all used by `InitBackends` in `ImguiSubsystem.cpp` today — reuse the same accessors). Confirm the SPIR-V arrays' element type/`sizeof` matches `codeSize` in bytes.

> **CORRECTION (applied during implementation):** ImGui v1.92 uses a SEPARATED image+sampler descriptor model, not a single combined-image-sampler. The committed code uses TWO set layouts — set 0 = `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` (identical to the backend's `DescriptorSetLayoutTexture`, so `AddTexture` sets bind) and set 1 = `VK_DESCRIPTOR_TYPE_SAMPLER` — a pipeline layout over both, plus a single shared sampler descriptor set (`m_samplerDS`) written with a linear sampler. Refs: imgui_impl_vulkan.cpp:1105-1212 (layouts + pipeline layout) and :528-564 (SetupRenderState binds set 1). The `offsetof` fallback (not `IM_OFFSETOF`) is also in the committed code.

- [ ] **Step 3: Build**

Run: `cmake --build build-ninja-clang --target Engine`
Expected: **success**.

- [ ] **Step 4: Commit**

```bash
git add src/engine/imgui/ImguiViewportRenderer.hpp src/engine/imgui/ImguiViewportRenderer.cpp
git commit -m "feat(imgui): ImguiViewportRenderer pipeline (ImGui shaders, shared descriptor layout)"
```

---

## Task 4: Per-viewport swapchain create / resize / retire

**Files:**
- Modify: `src/engine/imgui/ImguiViewportRenderer.cpp`

- [ ] **Step 1: Implement `EnsureWindow`, `DestroyOne`, `RetireViewports`**

Replace the stub `EnsureWindow`/`DestroyOne`/`RetireViewports` with:

```cpp
	void ImguiViewportRenderer::EnsureWindow(PerViewport& vp, void* glfwWindow, int width, int height)
	{
		ImGui_ImplVulkanH_Window& wd = vp.window;
		const uint32_t minImageCount = 2;
		if (!vp.created)
		{
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			const VkResult err = glfwCreateWindowSurface(m_vk->GetInstance().instance, static_cast<GLFWwindow*>(glfwWindow), nullptr, &surface);
			if (err != VK_SUCCESS || surface == VK_NULL_HANDLE)
			{
				AE_WARN(LogCategory::UI, "ImGui viewport surface creation failed ({})", static_cast<int>(err));
				return;
			}
			wd.Surface = surface;
			wd.UseDynamicRendering = true;
			// Force the main-swapchain color format so this viewport shares our pipeline.
			const VkFormat requested[] = {m_colorFormat};
			wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(m_vk->GetPhysicalDevice(), surface, requested, 1, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
			const VkPresentModeKHR presentModes[] = {VK_PRESENT_MODE_FIFO_KHR};
			wd.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(m_vk->GetPhysicalDevice(), surface, presentModes, 1);
			ImGui_ImplVulkanH_CreateOrResizeWindow(m_vk->GetInstance().instance, m_vk->GetPhysicalDevice(), m_device, &wd, m_vk->GetGraphicsQueueFamily(), nullptr, width, height, minImageCount, 0);
			vp.created = true;
			return;
		}
		if (wd.Width != width || wd.Height != height)
		{
			ImGui_ImplVulkanH_CreateOrResizeWindow(m_vk->GetInstance().instance, m_vk->GetPhysicalDevice(), m_device, &wd, m_vk->GetGraphicsQueueFamily(), nullptr, width, height, minImageCount, 0);
		}
	}

	void ImguiViewportRenderer::DestroyOne(PerViewport& vp)
	{
		if (!vp.created)
		{
			return;
		}
		const VkSurfaceKHR surface = vp.window.Surface;
		ImGui_ImplVulkanH_DestroyWindow(m_vk->GetInstance().instance, m_device, &vp.window, nullptr);
		if (surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_vk->GetInstance().instance, surface, nullptr);
		}
		vp.created = false;
		vp.window = ImGui_ImplVulkanH_Window{};
	}

	void ImguiViewportRenderer::RetireViewports(const std::vector<ImGuiID>& departedIds)
	{
		for (const ImGuiID id: departedIds)
		{
			auto it = m_viewports.find(id);
			if (it != m_viewports.end())
			{
				DestroyOne(it->second);
				m_viewports.erase(it);
			}
		}
	}
```

> Since 2025-09-26 `ImGui_ImplVulkanH_DestroyWindow` no longer destroys the surface (the caller created it) — hence the explicit `vkDestroySurfaceKHR`. Confirm against the vendored header comment (imgui_impl_vulkan.h) during implementation.

- [ ] **Step 2: Build**

Run: `cmake --build build-ninja-clang --target Engine`
Expected: **success**.

- [ ] **Step 3: Commit**

```bash
git add src/engine/imgui/ImguiViewportRenderer.cpp
git commit -m "feat(imgui): per-viewport swapchain lifecycle (create/resize/retire)"
```

---

## Task 5: Render + present a secondary viewport

**Files:**
- Modify: `src/engine/imgui/ImguiViewportRenderer.cpp`

Adapt the stock `ImGui_ImplVulkan_RenderWindow` (imgui_impl_vulkan.cpp:2162-2309, the `UseDynamicRendering` path) and `ImGui_ImplVulkan_RenderDrawData` body (imgui_impl_vulkan.cpp:573-720): acquire → barrier to `COLOR_ATTACHMENT_OPTIMAL` → `vkCmdBeginRenderingKHR` → upload our snapshot's vtx/idx into a per-viewport host-visible buffer → bind pipeline + push scale/translate → per-cmd scissor + bind `pcmd->GetTexID()` descriptor set + `vkCmdDrawIndexed` → `vkCmdEndRenderingKHR` → barrier to `PRESENT_SRC` → submit → `vkQueuePresentKHR`. All on the render thread's graphics queue (`m_vk->GetGraphicsQueue()`), sequential after the main present — no extra locking.

- [ ] **Step 1: Add a per-viewport vertex/index buffer ring**

Extend `PerViewport` in the header with a small ring (mirror `ImGui_ImplVulkan_FrameRenderBuffers` — see imgui_impl_vulkan.cpp:256-272 for the exact fields):

```cpp
		struct FrameBuffers
		{
			VkDeviceMemory vtxMem = VK_NULL_HANDLE, idxMem = VK_NULL_HANDLE;
			VkBuffer vtx = VK_NULL_HANDLE, idx = VK_NULL_HANDLE;
			VkDeviceSize vtxSize = 0, idxSize = 0;
		};
		struct PerViewport
		{
			ImGui_ImplVulkanH_Window window{};
			bool created = false;
			std::vector<FrameBuffers> ring; // sized to window.ImageCount
			uint32_t ringIndex = 0;
		};
```

Add a helper `CreateOrResizeBuffer(...)` — copy verbatim from imgui_impl_vulkan.cpp (the `CreateOrResizeBuffer` static function, ~line 500-528) into the anonymous namespace; it needs `m_vk->GetPhysicalDevice()` for the memory-type lookup (adapt its `bd->VulkanInitInfo` references to `m_vk` accessors). Destroy ring buffers in `DestroyOne` before `ImGui_ImplVulkanH_DestroyWindow`.

- [ ] **Step 2: Implement `Render` and `RenderOne`**

```cpp
	void ImguiViewportRenderer::Render(const ImguiFrameData& frame)
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		for (const auto& captured: frame.SecondaryViewports())
		{
			if (captured.platformHandle == nullptr || captured.draw.CmdListsCount <= 0)
			{
				continue;
			}
			PerViewport& vp = m_viewports[captured.id];
			const int w = static_cast<int>(captured.size.x * captured.fbScale.x);
			const int h = static_cast<int>(captured.size.y * captured.fbScale.y);
			if (w <= 0 || h <= 0)
			{
				continue;
			}
			EnsureWindow(vp, captured.platformHandle, w, h);
			if (!vp.created)
			{
				continue;
			}
			RenderOne(vp, captured.draw);
		}
	}
```

`RenderOne` — structure (fill each block from the stock reference cited above; the draw loop matches imgui_impl_vulkan.cpp:660-716 but binds `(VkDescriptorSet)pcmd->GetTexID()` and uses this class's `m_pipeline`/`m_pipelineLayout`):

```cpp
	void ImguiViewportRenderer::RenderOne(PerViewport& vp, const ImDrawData& draw)
	{
		ImGui_ImplVulkanH_Window& wd = vp.window;
		VkQueue queue = m_vk->GetGraphicsQueue();

		ImGui_ImplVulkanH_FrameSemaphores& fsd = wd.FrameSemaphores[wd.SemaphoreIndex];
		uint32_t imageIndex = 0;
		VkResult err = vkAcquireNextImageKHR(m_device, wd.Swapchain, UINT64_MAX, fsd.ImageAcquiredSemaphore, VK_NULL_HANDLE, &imageIndex);
		if (err == VK_ERROR_OUT_OF_DATE_KHR)
		{
			ImGui_ImplVulkanH_CreateOrResizeWindow(m_vk->GetInstance().instance, m_vk->GetPhysicalDevice(), m_device, &wd, m_vk->GetGraphicsQueueFamily(), nullptr, wd.Width, wd.Height, 2, 0);
			return; // present next frame
		}
		wd.FrameIndex = imageIndex;
		ImGui_ImplVulkanH_Frame& fd = wd.Frames[imageIndex];

		vkWaitForFences(m_device, 1, &fd.Fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m_device, 1, &fd.Fence);
		vkResetCommandPool(m_device, fd.CommandPool, 0);
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(fd.CommandBuffer, &begin);

		// (a) Barrier Backbuffer UNDEFINED/PRESENT_SRC -> COLOR_ATTACHMENT_OPTIMAL
		//     (copy the barrier from imgui_impl_vulkan.cpp:2218-2227).
		// (b) vkCmdBeginRenderingKHR with fd.BackbufferView, clear to (0,0,0,1)
		//     (imgui_impl_vulkan.cpp:2229-2247).
		// (c) Grow vp.ring to wd.ImageCount; pick rb = ring[imageIndex]; upload
		//     draw.CmdLists vtx/idx into rb (imgui_impl_vulkan.cpp:608-643).
		// (d) Bind m_pipeline; set viewport (0,0,fbW,fbH); push scale/translate from
		//     draw.DisplayPos/DisplaySize (imgui_impl_vulkan.cpp SetupRenderState 528-570).
		//     Bind the shared sampler set ONCE at set 1: vkCmdBindDescriptorSets(cmd,
		//     GRAPHICS, m_pipelineLayout, 1, 1, &m_samplerDS, ...) (matches backend :564).
		// (e) Draw loop over draw.CmdLists/CmdBuffer: project+clamp scissor with
		//     clip_off=draw.DisplayPos, clip_scale=draw.FramebufferScale; vkCmdSetScissor;
		//     bind (VkDescriptorSet)pcmd->GetTexID() at set 0 (SAMPLED_IMAGE, from
		//     AddTexture); vkCmdDrawIndexed (imgui_impl_vulkan.cpp:660-716). Skip
		//     UserCallback cmds (tool UI has none).
		// (f) vkCmdEndRenderingKHR; barrier -> PRESENT_SRC (imgui_impl_vulkan.cpp:2270-2282).

		vkEndCommandBuffer(fd.CommandBuffer);
		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &fsd.ImageAcquiredSemaphore;
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &fd.CommandBuffer;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &fsd.RenderCompleteSemaphore;
		vkQueueSubmit(queue, 1, &submit, fd.Fence);

		VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &fsd.RenderCompleteSemaphore;
		present.swapchainCount = 1;
		present.pSwapchains = &wd.Swapchain;
		present.pImageIndices = &imageIndex;
		vkQueuePresentKHR(queue, &present);
		wd.SemaphoreIndex = (wd.SemaphoreIndex + 1) % wd.SemaphoreCount;
	}
```

> The lettered blocks (a)-(f) are direct transcriptions from the cited stock lines with two substitutions: draw data = our snapshot (`draw`), pipeline/layout = `m_pipeline`/`m_pipelineLayout`. Keep `IMGUI_IMPL_VULKAN_USE_VOLK` dynamic-rendering entry points (`vkCmdBeginRenderingKHR` etc.) consistent with how the engine calls dynamic rendering elsewhere (`gpu::CommandList`/`ToVk`).

- [ ] **Step 3: Build**

Run: `cmake --build build-ninja-clang --target Engine`
Expected: **success**. (Not exercised until Task 7 wires it in.)

- [ ] **Step 4: Commit**

```bash
git add src/engine/imgui/ImguiViewportRenderer.hpp src/engine/imgui/ImguiViewportRenderer.cpp
git commit -m "feat(imgui): render + present secondary viewports on the render thread"
```

---

## Task 6: `ImguiSubsystem` — hooks, config flag, split capture, RenderViewports

**Files:**
- Modify: `src/engine/imgui/ImguiSubsystem.hpp`
- Modify: `src/engine/imgui/ImguiSubsystem.cpp`

- [ ] **Step 1: Header — new methods + owned renderer**

In `ImguiSubsystem.hpp`: add `#include <memory>` and a forward decl `class ImguiViewportRenderer;` and `class VulkanContext;`. Add public methods and replace the single `CaptureFrame` contract:

```cpp
		void BeginFrame(ServiceContainer& services, float deltaTimeSeconds);
		void Render();                                   // ImGui::Render() + refresh WantsInputCapture
		void UpdatePlatformWindows(std::uint64_t producerFrameIndex); // GLFW only; frame>=1 gate
		void SnapshotFrame(ImguiFrameData& outFrame);    // main + secondary viewports
		[[nodiscard]] std::vector<ImGuiID> SecondaryViewportIdsWithPendingDestroy() const;
		void RenderFrame(const ImguiFrameData& frame, gpu::CommandList& commands, const FrameTarget& target); // main (unchanged)
		void RenderViewports(const ImguiFrameData& frame);           // render thread -> ImguiViewportRenderer
		void SetViewportsEnabled(bool enabled);          // producer thread
```

Keep the old `CaptureFrame` removed (callers move to Render/UpdatePlatformWindows/SnapshotFrame in Task 7). Add member `std::unique_ptr<ImguiViewportRenderer> m_viewportRenderer;` and `bool m_viewportsEnabled = true;`.

- [ ] **Step 2: Init — install support, null renderer hooks, style, create renderer**

In `ImguiSubsystem.cpp`: `#include "imgui/ImguiViewportRenderer.hpp"` and `#include <imgui_internal.h>`. In `Init`, after setting `ImGuiConfigFlags_DockingEnable`, read the setting from `SettingsService` (via `services.TryGet<SettingsService>()`; default true if absent) into `m_viewportsEnabled` and set `io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;` **unconditionally at init** (so hooks install). In `ApplyTheme`, when viewports are enabled set:

```cpp
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
```

In `InitBackends`, after `ImGui_ImplVulkan_Init(&initInfo)` succeeds, suppress the stock renderer viewport hooks and create our renderer:

```cpp
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Renderer_CreateWindow = nullptr;
		platformIO.Renderer_DestroyWindow = nullptr;
		platformIO.Renderer_SetWindowSize = nullptr;
		platformIO.Renderer_RenderWindow = nullptr;
		platformIO.Renderer_SwapBuffers = nullptr;

		m_viewportRenderer = std::make_unique<ImguiViewportRenderer>();
		m_viewportRenderer->Init(vk, ToVk(swapchain.GetImageFormat()));

		// Reflect the persisted setting now that support is installed.
		if (!m_viewportsEnabled)
		{
			ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
		}
```

- [ ] **Step 3: Split capture + the three producer methods + render-thread method**

Replace the body of the old `CaptureFrame` with:

```cpp
	void ImguiSubsystem::Render()
	{
		if (!m_initialized)
		{
			return;
		}
		ImGui::Render();
		const ImGuiIO& io = ImGui::GetIO();
		m_wantsInputCapture = io.WantCaptureMouse || io.WantCaptureKeyboard;
	}

	void ImguiSubsystem::UpdatePlatformWindows(std::uint64_t producerFrameIndex)
	{
		if (!m_initialized || producerFrameIndex < 1)
		{
			return; // frame-0 gate (see spec)
		}
		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows(); // GLFW only (renderer hooks nulled)
		}
	}

	std::vector<ImGuiID> ImguiSubsystem::SecondaryViewportIdsWithPendingDestroy() const
	{
		std::vector<ImGuiID> departed;
		if (!m_initialized)
		{
			return departed;
		}
		const ImGuiContext& g = *ImGui::GetCurrentContext();
		for (ImGuiViewportP* vp: g.Viewports)
		{
			if (vp == static_cast<ImGuiViewportP*>(ImGui::GetMainViewport()))
			{
				continue;
			}
			if (vp->PlatformWindowCreated && vp->LastFrameActive < g.FrameCount)
			{
				departed.push_back(vp->ID);
			}
		}
		return departed;
	}

	void ImguiSubsystem::SnapshotFrame(ImguiFrameData& outFrame)
	{
		if (!m_initialized)
		{
			return;
		}
		std::lock_guard lock(m_mutex);
		outFrame.Capture(ImGui::GetDrawData()); // main viewport
		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			ImGuiViewport* main = ImGui::GetMainViewport();
			for (ImGuiViewport* vp: platformIO.Viewports)
			{
				if (vp == main || vp->DrawData == nullptr || vp->PlatformHandle == nullptr)
				{
					continue;
				}
				outFrame.CaptureSecondary(vp->DrawData, vp->ID, vp->Pos, vp->Size, ImGui::GetDrawData()->FramebufferScale, vp->PlatformHandle);
			}
		}
		m_gameThreadFrameLock.reset();
	}

	void ImguiSubsystem::RenderViewports(const ImguiFrameData& frame)
	{
		if (!m_initialized || m_viewportRenderer == nullptr)
		{
			return;
		}
		std::lock_guard lock(m_mutex);
		m_viewportRenderer->Render(frame);
	}

	void ImguiSubsystem::SetViewportsEnabled(bool enabled)
	{
		if (!m_initialized)
		{
			return;
		}
		m_viewportsEnabled = enabled;
		ImGuiIO& io = ImGui::GetIO();
		if (enabled)
		{
			io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		}
		else
		{
			io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
		}
	}
```

> `SnapshotFrame` still resets `m_gameThreadFrameLock` (the game-thread frame lock the old `CaptureFrame` released). Confirm `BeginFrame`'s `ImGui_ImplGlfw_NewFrame()` remains — it drives platform viewport input. `m_mutex` in `RenderViewports` serializes with `RenderFrame` (both render thread; the lock is defensive and matches the existing pattern).

- [ ] **Step 4: Shutdown ordering**

In `ShutdownBackends`, before `ImGui_ImplVulkan_Shutdown()`, destroy our renderer while the device is idle (the caller already idles the GPU in `Shutdown`/`RunExclusive`):

```cpp
		if (m_viewportRenderer)
		{
			m_viewportRenderer->Shutdown();
			m_viewportRenderer.reset();
		}
```

- [ ] **Step 5: Build**

Run: `cmake --build build-ninja-clang --target Engine`
Expected: **success**. Also confirm `Task 1 Step 5` (SettingsService + AetherCore forwarder) now compiles since `SetViewportsEnabled` exists.

- [ ] **Step 6: Commit**

```bash
git add src/engine/imgui/ImguiSubsystem.hpp src/engine/imgui/ImguiSubsystem.cpp src/engine/utils/SettingsService.cpp src/engine/AetherCore.hpp src/engine/AetherCore.cpp
git commit -m "feat(imgui): suppress stock renderer hooks; split capture; drive viewport renderer"
```

---

## Task 7: Wire the producer loop + EndFrame; first runtime test

**Files:**
- Modify: `src/engine/AetherCore.cpp`

- [ ] **Step 1: Replace the `CaptureFrame` block in `RunFrameLoop`**

In `RunFrameLoop` (around lines 314-330), replace the `m_imgui->CaptureFrame(imguiFrame)` section with the Render → structural-destroy check → UpdatePlatformWindows → snapshot sequence:

```cpp
			if (m_imgui)
			{
				m_imgui->BeginFrame(m_services, static_cast<float>(rawDt));
			}
			client.OnBuildUI(gameDt, m_producerFrameIndex);

			ImguiFrameData imguiFrame;
			if (m_imgui)
			{
				m_imgui->Render();

				const std::vector<ImGuiID> departed = m_imgui->SecondaryViewportIdsWithPendingDestroy();
				if (!departed.empty())
				{
					RunExclusive(QuiesceMode::Drain,
					        [this, &departed]()
					        {
						        m_imgui->RetireViewports(departed);           // render thread parked, GPU idle
						        m_imgui->UpdatePlatformWindows(m_producerFrameIndex); // then destroy GLFW windows
					        });
				}
				else
				{
					m_imgui->UpdatePlatformWindows(m_producerFrameIndex);
				}

				m_imgui->SnapshotFrame(imguiFrame);
			}

			RenderFramePacket packet = PrepareFrame(drawSlot, m_producerFrameIndex);
```

Add a thin `ImguiSubsystem::RetireViewports(const std::vector<ImGuiID>&)` that forwards to `m_viewportRenderer->RetireViewports(...)` (declare in the header next to `RenderViewports`). It runs on the producer inside `RunExclusive`, which is safe (render thread parked).

- [ ] **Step 2: Call `RenderViewports` in `EndFrame`**

In `EndFrame`, immediately after `m_imgui->RenderFrame(packet.imgui, m_currentCmdList, m_gpu->BuildFrameTarget());` and the subsequent `SubmitAndAdvance(frameIdx);`, add — after present so the swapchain image and final-color are done:

```cpp
		SubmitAndAdvance(frameIdx);
		m_imgui->RenderViewports(packet.imgui); // secondary OS windows, render thread
	}
```

> Verify ordering: `RenderViewports` must run after the render graph has produced the final-color image the torn-out Viewport samples. `SubmitAndAdvance` submits the main frame; the secondary submits wait on their own acquire semaphores and sample the final-color image which is already in `ShaderReadOnly` from the render graph. If sync validation flags the final-color read, add a `vkCmdPipelineBarrier` (fragment-shader-read) at the start of each secondary render pass, or move `RenderViewports` before `SubmitAndAdvance` so it shares the main command buffer's barriers — decide from validation output.

- [ ] **Step 3: Build both presets**

Run:
```
cmake --build build-ninja-clang --target Engine App EngineTests
cmake --build build-vs2022-msvc --config RelWithDebInfo --target Engine App
```
Expected: **both succeed**.

- [ ] **Step 4: Runtime test — tear out a pure-ImGui panel**

Launch `App` from `build-ninja-clang/`. With validation layers on:
1. Drag the **Inspector** (or **Settings**) panel out of the main window → it becomes its own OS window and renders correctly.
2. Drag it back → re-docks.
3. Resize the torn-out window; drag it across monitors.
4. Close the app with it torn out → clean shutdown, **no validation errors**.

Expected: all pass, validation clean. If a crash occurs on tear-out, check the frame>=1 gate and that `EnsureWindow` created a swapchain before `RenderOne`.

- [ ] **Step 5: Commit**

```bash
git add src/engine/AetherCore.cpp src/engine/imgui/ImguiSubsystem.hpp src/engine/imgui/ImguiSubsystem.cpp
git commit -m "feat(imgui): producer-loop integration + render-thread viewport present"
```

---

## Task 8: Live-texture tear-out, toggle, persistence, cross-platform verification

**Files:** none (verification + any barrier fix surfaced in Task 7 Step 2)

- [ ] **Step 1: Tear out the 3D Viewport (the correctness target)**

Launch with **sync validation** enabled. Drag the **Viewport** panel onto a second monitor.
Expected: the live 3D scene renders in the torn-out window with no tearing/garbage and **no sync-validation errors** on `post.GetFinalColorImageView()`. If sync validation flags a hazard, apply the barrier fix noted in Task 7 Step 2, rebuild, retest.

- [ ] **Step 2: Toggle the setting at runtime**

Open **Settings**, uncheck **imguiViewports** → all torn-out windows merge back into the main window (structural destroy path fires; validation clean). Re-check it → panels can be torn out again.

- [ ] **Step 3: Persistence across restart**

Tear a panel out, quit, relaunch. Expected: the panel spawns as an OS window at frame >= 1 (no frame-0 crash), positioned per `imgui.ini`.

- [ ] **Step 4: Tracy / performance sanity**

With Tracy attached, confirm the `RunExclusive` quiesce fires only when a viewport is **destroyed** (re-dock/close/toggle-off), not per frame during move/resize, and the producer stays up to 3 frames ahead in steady state.

- [ ] **Step 5: MSVC build + full test run**

Run:
```
cmake --build build-vs2022-msvc --config RelWithDebInfo --target Engine App EngineTests
build-ninja-clang/EngineTests
```
Expected: MSVC build clean; all doctest cases pass.

- [ ] **Step 6: Document the Wayland caveat**

Add a comment in `ImguiViewportRenderer::EnsureWindow` (or the class header) noting that on Linux/Wayland GLFW cannot position windows programmatically (X11 is full); viewports still function. Commit any barrier fix + the comment:

```bash
git add -A
git commit -m "docs+fix(imgui): live-texture tear-out verified; Wayland caveat; sync barrier"
```

---

## Self-review notes (author)

- **Spec coverage:** settings toggle (T1), N-viewport snapshot (T2), pipeline (T3), swapchain lifecycle (T4), render/present (T5), hook suppression + config flag + split capture + shutdown (T6), producer loop + structural quiesce + EndFrame (T7), full tear-out incl. live Viewport + toggle + persistence + Wayland + cross-platform (T8). All spec sections map to a task.
- **Type consistency:** `ImguiFrameData::CapturedViewport`, `SecondaryViewports()`, `CaptureSecondary(...)`; `ImguiViewportRenderer::{Init,Render,RetireViewports,Shutdown}`; `ImguiSubsystem::{Render,UpdatePlatformWindows,SnapshotFrame,SecondaryViewportIdsWithPendingDestroy,RenderViewports,RetireViewports,SetViewportsEnabled}`; `AetherCore::SetImguiViewportsEnabled`; `EngineSettings::Graphics::imguiViewports` — used consistently across tasks.
- **Known transcription points (not placeholders — exact source cited):** ImGui SPIR-V arrays + `CreateOrResizeBuffer` (imgui_impl_vulkan.cpp), and the RenderOne blocks (a)-(f) from imgui_impl_vulkan.cpp:2162-2309 / 573-716. These are verbatim reuse of ImGui's own code with two documented substitutions (our snapshot, our pipeline).
- **Ordering caveat:** Task 1 Step 5 (SettingsService/AetherCore forwarder) compiles only once `ImguiSubsystem::SetViewportsEnabled` exists (Task 6); commit T1 Steps 1-4 first, land Step 5 with T6.
