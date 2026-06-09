import json
from pathlib import Path

analysis = json.loads(Path("C:/Users/alexm/source/repos/AetherCore/graphify-out/.graphify_analysis.json").read_text(encoding="utf-8"))
extract = json.loads(Path("C:/Users/alexm/source/repos/AetherCore/graphify-out/.graphify_extract.json").read_text(encoding="utf-8"))

node_map = {n["id"]: n.get("label", n["id"]) for n in extract["nodes"]}

def infer_label(cid, nodes):
    labels = [node_map.get(n, n) for n in nodes]
    ids = nodes

    # Count keyword frequencies
    keywords = {}
    for nid in ids:
        parts = nid.replace("_", " ").lower().split()
        for p in parts:
            keywords[p] = keywords.get(p, 0) + 1

    # Sort by frequency
    sorted_kw = sorted(keywords.items(), key=lambda x: -x[1])

    # Detect domain from node ID patterns
    nid_text = " ".join(ids)

    # Domain detection based on node ID patterns
    if "modules_" in nid_text and "das_" in nid_text:
        return "Script UI Bindings"

    if "modules_" in nid_text and ("animationmodule" in nid_text or "animation" in nid_text):
        return "Script Animation Module"

    if "modules_uimodule" in nid_text:
        return "Script UI Module"

    if "modules_" in nid_text and "scenecontext" in nid_text:
        return "Script Scene Context"

    if "modules_" in nid_text:
        return "Scripting Module"

    if "ui_uisystem" in nid_text or "ui_uiwidgets" in nid_text or "ui_uicontext" in nid_text or "uicontext" in nid_text:
        return "UI System Core"

    if "ui_quadrenderer" in nid_text or "ui_uirenderer" in nid_text:
        return "UI Renderer"

    if "ui_uilayout" in nid_text:
        return "UI Layout Engine"

    if "ui_uitheme" in nid_text:
        return "UI Theme System"

    if "quadrenderer" in nid_text and "m_" in nid_text:
        return "Quad Renderer Core"

    if "text_fontatlas" in nid_text:
        return "Font Atlas System"

    if "text_textrenderer" in nid_text:
        return "Text Renderer System"

    if "rendering_renderqueue" in nid_text:
        return "Render Queue System"

    if "rendering_rendergraph" in nid_text:
        return "Render Graph System"

    if "rendering_renderpipelinecoordinator" in nid_text:
        return "Render Pipeline Coordinator"

    if "rendering_renderer" in nid_text and "_renderer" in nid_text:
        return "Renderer Core"

    if "rendering_lightingmanager" in nid_text:
        return "Lighting Manager"

    if "rendering_localshadowservice" in nid_text:
        return "Local Shadow Service"

    if "rendering_shadowservice" in nid_text:
        return "Shadow Service"

    if "rendering_shadowatlas" in nid_text:
        return "Shadow Atlas"

    if "rendering_cullpass" in nid_text or "cullpass" in nid_text:
        return "GPU Culling Pass"

    if "rendering_skybox" in nid_text or "skyboxpass" in nid_text:
        return "Skybox Pass"

    if "rendering_forwardpass" in nid_text or "forwardpass" in nid_text:
        return "Forward Rendering Pass"

    if "rendering_postprocess" in nid_text:
        return "Post-Processing Stack"

    if "rendering_computecull" in nid_text:
        return "GPU Compute Culling"

    if "rendering_frametarget" in nid_text:
        return "Frame Target Service"

    if "rendering_renderqueue" in nid_text:
        return "Render Queue"

    if "vulkan_uniqueimage" in nid_text:
        return "Unique Image (Vulkan)"

    if "vulkan_uniquebuffer" in nid_text:
        return "Unique Buffer (Vulkan)"

    if "vulkan_swapchain" in nid_text:
        return "Swapchain (Vulkan)"

    if "vulkan_vulkancontext" in nid_text or "vulkan_context" in nid_text:
        return "Vulkan Context"

    if "vulkan_aftermath" in nid_text:
        return "Vulkan Aftermath"

    if "vulkan_gpuheap" in nid_text:
        return "GPU Heap Allocator"

    if "vulkan_asynccompute" in nid_text or "gpu_async" in nid_text:
        return "Async Compute Context"

    if "gpu_bindless" in nid_text or "bindlessmanager" in nid_text:
        return "Bindless Manager"

    if "gpu_device" in nid_text or "gpudevice" in nid_text:
        return "GPU Device"

    if "gpu_timestamppool" in nid_text or "timestamppool" in nid_text:
        return "GPU Timestamp Pool"

    if "physics_physicssystem" in nid_text and "physicssystem_" in nid_text:
        return "Physics System Core"

    if "physics_physicssystem" in nid_text:
        return "Physics System"

    if "physics_physicsdebugrenderer" in nid_text:
        return "Physics Debug Renderer"

    if "physics_physicscomponents" in nid_text:
        return "Physics Components"

    if "animation_animationdatabase" in nid_text:
        return "Animation Database"

    if "animation_animationik" in nid_text:
        return "Animation IK System"

    if "animation_animationblend" in nid_text or "blendsystem" in nid_text:
        return "Animation Blend System"

    if "animation_rootmotion" in nid_text:
        return "Root Motion System"

    if "animatorsamplejob" in nid_text or "samplejob" in nid_text:
        return "Animation Sampling Jobs"

    if "camera_camera" in nid_text:
        return "Camera System"

    if "camera_cameramanager" in nid_text:
        return "Camera Manager"

    if "assets_assetmanager" in nid_text:
        return "Asset Manager Core"

    if "assets_gltf" in nid_text:
        return "glTF Asset Loading"

    if "mesh_mesh_" in nid_text:
        return "Mesh System"

    if "material_material" in nid_text:
        return "Material System"

    if "material_texture" in nid_text:
        return "Texture System"

    if "scene_world" in nid_text or "scene_scene" in nid_text:
        return "Scene / World System"

    if "scene_ecs" in nid_text or "entity_components" in nid_text:
        return "ECS Components"

    if "utils_logger" in nid_text:
        return "Logger System"

    if "utils_settings" in nid_text:
        return "Engine Settings"

    if "utils_profiler" in nid_text or "tracy" in nid_text:
        return "Profiler / Tracy"

    if "utils_servicecontainer" in nid_text:
        return "Service Container"

    if "utils_loading" in nid_text or "loadingmanager" in nid_text:
        return "Loading Manager"

    if "utils_framepacer" in nid_text:
        return "Frame Pacer"

    if "utils_expected" in nid_text or "aetherror" in nid_text:
        return "Error Handling (Expected)"

    if "utils_allocator" in nid_text or "stlallocator" in nid_text:
        return "Custom Allocator"

    if "coro_task" in nid_text or "coroutine" in nid_text:
        return "Coroutine Task System"

    if "coro_channel" in nid_text:
        return "Coroutine Channel"

    if "coro_executor" in nid_text or "executor" in nid_text:
        return "Coroutine Executor"

    if "platform_window" in nid_text or "glfw" in nid_text:
        return "Platform Window (GLFW)"

    if "platform_crashhandler" in nid_text:
        return "Crash Handler"

    if "platform_input" in nid_text:
        return "Input System"

    if "io_filesystem" in nid_text:
        return "File System"

    if "io_directorybackend" in nid_text:
        return "Directory Backend"

    if "io_pakbackend" in nid_text:
        return "PAK Backend"

    if "io_virtualfilesystem" in nid_text or "vfs" in nid_text:
        return "Virtual File System"

    if "io_async" in nid_text or "readfileasync" in nid_text:
        return "Async I/O System"

    if "systems_fishinggamesystem" in nid_text:
        return "Fishing Game System"

    if "systems_sandboxgamesystem" in nid_text:
        return "Sandbox Game System"

    if "systems_physicsgamesystem" in nid_text:
        return "Physics Game System"

    if "systems_daynightsystem" in nid_text or "daynightsystem" in nid_text:
        return "Day/Night Cycle System"

    if "layers_debuglayer" in nid_text:
        return "Debug Layer (UI)"

    if "layers_fishinglayer" in nid_text:
        return "Fishing Layer"

    if "layers_physicslayer" in nid_text:
        return "Physics Layer"

    if "layers_sandboxlayer" in nid_text:
        return "Sandbox Layer"

    if "layers_inventorylayer" in nid_text:
        return "Inventory Layer"

    if "layers_loadinglayer" in nid_text:
        return "Loading Layer"

    if "layers_layerstack" in nid_text:
        return "Layer Stack"

    if "layers_scriptedscenelayer" in nid_text:
        return "Scripted Scene Layer"

    if "app_application" in nid_text:
        return "Application Core"

    if "effects_effectmanager" in nid_text:
        return "Effect Manager"

    if "components_gamecomponents" in nid_text:
        return "Game Components"

    if "data_dialogues" in nid_text:
        return "Dialogue Data"

    if "data_npcs" in nid_text:
        return "NPC Data"

    if "include_binaryformats" in nid_text:
        return "Binary Format Headers"

    if "include_pakformat" in nid_text:
        return "PAK Format Headers"

    if "cmakepresets" in nid_text:
        return "CMake Presets"

    if "opencode" in nid_text and "json" in nid_text:
        return "OpenCode Config"

    if "scripts_format" in nid_text or "scripts_run_" in nid_text or "scripts_export" in nid_text:
        return "Build Scripts"

    if "assetpack_" in nid_text:
        return "Asset Packer Tool"

    if "bgfx" in nid_text or "shader" in nid_text or "spirv" in nid_text:
        return "Shader System"

    if "aethercore" in nid_text and "engine" in nid_text:
        return "Engine Core"

    if "vulkan_volk" in nid_text:
        return "Volk Vulkan Loader"

    if "passes_" in nid_text:
        return "Render Passes"

    if "frameconstants" in nid_text:
        return "Frame Constants"

    if "commandrecorder" in nid_text:
        return "Command Recorder"

    if "frametarget" in nid_text:
        return "Frame Target"

    if "gpu_shared" in nid_text:
        return "GPU Shared Resources"

    if "vulkan_vma" in nid_text:
        return "VMA Allocator"

    if "tool_assetpack" in nid_text or "tools_assetpack" in nid_text:
        return "Asset Packer CLI"

    if "anim_" in nid_text and "sample" in nid_text:
        return "Animation Sampling"

    if "ik" in nid_text and ("solve" in nid_text or "ground" in nid_text):
        return "IK Solving"

    if "skinnedmesh" in nid_text or "skinning" in nid_text:
        return "GPU Skinning"

    if "framebuffer" in nid_text:
        return "Framebuffer"

    # Generic fallbacks based on common prefixes
    if "utils_" in nid_text:
        return "Utility System"

    if "gpu_" in nid_text:
        return "GPU Abstraction Layer"

    if "vulkan_" in nid_text:
        return "Vulkan Implementation"

    if "rendering_" in nid_text:
        return "Rendering Subsystem"

    if "animation_" in nid_text:
        return "Animation System"

    if "physics_" in nid_text:
        return "Physics System"

    if "scene_" in nid_text:
        return "Scene System"

    if "assets_" in nid_text:
        return "Asset System"

    if "camera_" in nid_text:
        return "Camera Subsystem"

    if "material_" in nid_text:
        return "Material Subsystem"

    if "mesh_" in nid_text:
        return "Mesh Subsystem"

    if "io_" in nid_text:
        return "I/O Subsystem"

    if "platform_" in nid_text:
        return "Platform Subsystem"

    if "ui_" in nid_text:
        return "UI Subsystem"

    if "text_" in nid_text:
        return "Text Subsystem"

    if "layers_" in nid_text:
        return "App Layer"

    if "systems_" in nid_text:
        return "Game Systems"

    if "app_" in nid_text:
        return "Application"

    # Try to find a good name from sample labels
    sample_labels = [node_map.get(n, "") for n in nodes[:10]]
    sample_labels = [s for s in sample_labels if s and s != nid and not s.endswith(".cpp") and not s.endswith(".hpp")]
    if sample_labels:
        return sample_labels[0]

    return f"Community {cid}"

# Process all communities
labels = {}
for cid_str, nodes in analysis["communities"].items():
    labels[cid_str] = infer_label(int(cid_str), nodes)

# Manual overrides for known communities from the _show_communities.py output
labels["0"] = "UI System Core"
labels["1"] = "Render Queue Core"
labels["2"] = "Script UI Bindings"
labels["3"] = "Logger System"
labels["4"] = "Fishing Game System"
labels["5"] = "UI Renderer / Layout"
labels["6"] = "Sandbox Game System"
labels["7"] = "File System Backends"
labels["8"] = "Physics System Interface"
labels["9"] = "Render Graph Core"
labels["10"] = "Physics Game System"
labels["11"] = "Animation Database"
labels["12"] = "Animation IK System"
labels["13"] = "Asset Manager"
labels["14"] = "UI Theme System"
labels["15"] = "Pipeline Coordinator"
labels["16"] = "Mesh System"
labels["17"] = "Physics Debug Renderer"
labels["18"] = "Camera System"
labels["19"] = "Physics Components"
labels["20"] = "Unique Image (Vulkan)"
labels["21"] = "Dialogue Data"
labels["22"] = "Font Atlas System"
labels["23"] = "Script Animation Module"
labels["24"] = "Swapchain (Vulkan)"
labels["25"] = "Text / UI Renderer"
labels["26"] = "Script Scene Context"
labels["27"] = "File System Core"
labels["28"] = "Debug Layer (UI)"
labels["29"] = "Crash Handler"
labels["30"] = "Coroutine Task System"
labels["31"] = "Lighting Manager Core"
labels["32"] = "Lighting Compute Pipeline"
labels["33"] = "Asset Manager State"
labels["34"] = "Font / Text Renderer"
labels["35"] = "Vulkan Aftermath"
labels["36"] = "Input System"
labels["37"] = "Quad Renderer Core"
labels["38"] = "Physics System Update"
labels["39"] = "Local Shadow Service"

Path("C:/Users/alexm/source/repos/AetherCore/graphify-out/.graphify_labels.json").write_text(
    json.dumps(labels, indent=2, ensure_ascii=False), encoding="utf-8"
)
print(f"Generated {len(labels)} labels.")
