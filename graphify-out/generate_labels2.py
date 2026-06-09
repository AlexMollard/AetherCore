import json
from pathlib import Path
from collections import Counter
import re

analysis = json.loads(Path(".graphify_analysis.json").read_text(encoding="utf-8"))
extract = json.loads(Path(".graphify_extract.json").read_text(encoding="utf-8"))

node_map = {n["id"]: n.get("label", n["id"]) for n in extract["nodes"]}

# Module prefix patterns for naming
MODULE_NAMES = {
    "ui_uicontext": "UI Context",
    "ui_uisystem": "UI System Core",
    "ui_uiwidgets": "UI Widgets",
    "ui_quadrenderer": "UI Quad Renderer",
    "ui_uirenderer": "UI Renderer",
    "ui_uilayout": "UI Layout",
    "ui_uitheme": "UI Theme",
    "rendering_renderqueue": "Render Queue",
    "rendering_rendergraph": "Render Graph",
    "rendering_renderpipelinecoordinator": "Render Pipeline Coordinator",
    "rendering_renderer": "Renderer Core",
    "rendering_lightingmanager": "Lighting Manager",
    "rendering_localshadowservice": "Local Shadow Service",
    "rendering_shadowservice": "Shadow Service",
    "rendering_shadowatlasmanager": "Shadow Atlas",
    "rendering_renderingsubsystem": "Rendering Subsystem",
    "rendering_rendertargetservice": "Render Target Service",
    "rendering_frameconstants": "Frame Constants",
    "rendering_graphicspipeline": "Graphics Pipeline",
    "rendering_commandrecorder": "Command Recorder",
    "physics_physicssystem": "Physics System",
    "physics_physicsdebugrenderer": "Physics Debug Renderer",
    "physics_physicscomponents": "Physics Components",
    "animation_animationdatabase": "Animation Database",
    "animation_animationik": "Animation IK",
    "animation_animationblend": "Animation Blend",
    "animation_rootmotion": "Root Motion",
    "camera_camera": "Camera System",
    "camera_cameramanager": "Camera Manager",
    "assets_assetmanager": "Asset Manager",
    "assets_gltfasset": "glTF Asset Loader",
    "mesh_mesh": "Mesh System",
    "material_materialbuffer": "Material Buffer",
    "material_texture": "Texture Loader",
    "material_material": "Material System",
    "scene_world": "Scene World",
    "scene_entity": "ECS Entity",
    "scene_scene": "Scene System",
    "utils_logger": "Logger",
    "utils_settings": "Engine Settings",
    "utils_profiler": "Profiler",
    "utils_servicecontainer": "Service Container",
    "utils_loadingmanager": "Loading Manager",
    "utils_framepacer": "Frame Pacer",
    "utils_expected": "Error Handling",
    "utils_allocator": "Allocator",
    "utils_textini": "INI/Config Parser",
    "coro_task": "Coroutine Task",
    "coro_channel": "Coroutine Channel",
    "coro_executor": "Coroutine Executor",
    "platform_window": "Platform Window",
    "platform_crashhandler": "Crash Handler",
    "platform_input": "Input System",
    "io_filesystem": "File System",
    "io_directorybackend": "Directory Backend",
    "io_pakbackend": "PAK Backend",
    "gpu_bindlessmanager": "Bindless Manager",
    "gpu_device": "GPU Device",
    "gpu_timestamppool": "GPU Timestamp Pool",
    "gpu_asynccompute": "Async Compute",
    "vulkan_uniqueimage": "Unique Image",
    "vulkan_uniquebuffer": "Unique Buffer",
    "vulkan_swapchain": "Swapchain",
    "vulkan_vulkancontext": "Vulkan Context",
    "vulkan_aftermathcontext": "Vulkan Aftermath",
    "vulkan_gpuheap": "GPU Heap",
    "vulkan_resourcepool": "Resource Pool",
    "modules_uimodule": "Script UI Module",
    "modules_animationmodule": "Script Animation Module",
    "modules_worldmodule": "Script World Module",
    "modules_inputmodule": "Script Input Module",
    "modules_physicsmodule": "Script Physics Module",
    "modules_renderermodule": "Script Renderer Module",
    "modules_systemsmodule": "Script Systems Module",
    "modules_datamodule": "Script Data Module",
    "modules_gamecomponentsmodule": "Script Game Components",
    "modules_tagslots": "Script Tag Slots",
    "systems_fishinggamesystem": "Fishing Game System",
    "systems_sandboxgamesystem": "Sandbox Game System",
    "systems_physicsgamesystem": "Physics Game System",
    "systems_daynightsystem": "Day/Night Cycle",
    "layers_debuglayer": "Debug Layer",
    "layers_fishinglayer": "Fishing Layer",
    "layers_physicslayer": "Physics Layer",
    "layers_sandboxlayer": "Sandbox Layer",
    "layers_inventorylayer": "Inventory Layer",
    "layers_loadinglayer": "Loading Layer",
    "layers_uisandboxlayer": "UI Sandbox Layer",
    "layers_scriptedscenelayer": "Scripted Scene Layer",
    "layers_layerstack": "Layer Stack",
    "scripting_scenecontext": "Script Scene Context",
    "scripting_scriptingsubsystem": "Scripting Subsystem",
    "app_application": "Application Core",
    "effects_effectmanager": "Effect Manager",
    "components_gamecomponents": "Game Components",
    "engine_aethercore": "Engine Core (AetherCore)",
    "text_fontatlas": "Font Atlas",
    "text_textrenderer": "Text Renderer",
    "assetpack_threadpool": "Asset Packer Thread Pool",
    "assetpack_meshprocessor": "Asset Packer Mesh Processor",
    "assetpack_paklog": "Asset Packer Logging",
    "assetpack_pipelineutils": "Asset Packer Pipeline Utils",
    "assetpack_assetprocessor": "Asset Packer Asset Processor",
    "assetpack_materialprocessor": "Asset Packer Material Processor",
    "assetpack_spirvprocessor": "Asset Packer SPIR-V Processor",
    "assetpack_textureprocessor": "Asset Packer Texture Processor",
    "passes_postprocessstack": "Post-Process Stack",
    "passes_cullpass": "GPU Culling Pass",
    "passes_forwardpass": "Forward Render Pass",
    "passes_skyboxpass": "Skybox Pass",
}

def get_community_name(cid_str, nodes):
    nid_text = " ".join(nodes)
    sample_labels = [node_map.get(n, "") for n in nodes[:20]]
    label_text = " ".join(str(l) for l in sample_labels)

    # Check for known module prefixes in order of specificity
    best_match = None
    best_len = 0
    for prefix, name in MODULE_NAMES.items():
        if prefix in nid_text:
            # Prefer longer/more specific matches
            score = len(prefix)
            if score > best_len:
                best_match = name
                best_len = score

    if best_match:
        name = best_match
        # Add specificity for cpp vs hpp
        has_cpp = "cpp_" in nodes[0] if nodes else False
        has_hpp = "_hpp_" in nid_text
        if name == "Physics System" and "physicssystem_hpp" in nid_text:
            return "Physics System State"
        return name

    # Check passes
    if "forwardpass" in nid_text:
        return "Forward Render Pass"
    if "cullpass" in nid_text:
        return "GPU Culling Pass"
    if "skyboxpass" in nid_text:
        return "Skybox Pass"
    if "postprocess" in nid_text:
        return "Post-Processing"

    # Check for data files
    if "data_dialogues" in nid_text:
        return "Dialogue Data"
    if "data_npcs" in nid_text:
        return "NPC Data"

    # Check for binary format headers
    if "include_binaryformats" in nid_text:
        return "Binary Format Headers"
    if "include_pakformat" in nid_text:
        return "PAK Format Headers"

    # Check for app-level systems
    if "systems_" in nid_text:
        return "Game Systems"
    if "layers_" in nid_text:
        return "App Layer"
    if "modules_" in nid_text:
        return "Scripting Module"
    if "scripting_" in nid_text:
        return "Scripting System"

    # Check for tool asset packer
    if "assetpack_" in nid_text or "tools_assetpack_" in nid_text:
        return "Asset Packer"

    # Engine core
    if "engine_aethercore" in nid_text:
        return "Engine Core"

    # Config/build files
    if "cmakepresets" in nid_text:
        return "CMake Presets"
    if "opencode" in nid_text:
        return "OpenCode Config"

    # Shared rendering patterns
    if "rendering_" in nid_text:
        return "Rendering Subsystem"
    if "vulkan_" in nid_text:
        return "Vulkan Implementation"
    if "gpu_" in nid_text:
        return "GPU Abstraction"
    if "utils_" in nid_text:
        return "Utility System"

    # Fall back to most common source file
    sources = [n for n in nodes if n.startswith("src_") and (n.endswith("_cpp") or n.endswith("_hpp"))]
    if sources:
        src = sources[0]
        # Extract meaningful part
        parts = src.split("_")
        if len(parts) >= 4:
            area = parts[1]  # engine, app, tools
            subsystem = parts[2]  # e.g., ui, rendering, etc
            name = subsystem.capitalize()
            return f"{name} Subsystem"

    # Last resort: look at sample labels
    if sample_labels:
        first = str(sample_labels[0])
        if not first.endswith(".cpp") and not first.endswith(".hpp"):
            return first

    return f"Community {cid_str}"

# Process all communities
labels = {}
for cid_str, nodes in analysis["communities"].items():
    name = get_community_name(cid_str, nodes)
    labels[cid_str] = name

# Manual overrides for communities 0-39 based on verified inspection
overrides = {
    "0": "UI System Core",
    "1": "Render Queue",
    "2": "Script UI Bindings",
    "3": "Logger System",
    "4": "Fishing Game System",
    "5": "UI Renderer and Layout",
    "6": "Sandbox Game System",
    "7": "File System Backends",
    "8": "Physics System Interface",
    "9": "Render Graph",
    "10": "Physics Game System",
    "11": "Animation Database",
    "12": "Animation IK System",
    "13": "Asset Manager",
    "14": "UI Theme System",
    "15": "Render Pipeline Coordinator",
    "16": "Mesh System",
    "17": "Physics Debug Renderer",
    "18": "Camera System",
    "19": "Physics Components",
    "20": "Unique Image (Vulkan)",
    "21": "Dialogue Data",
    "22": "Font Atlas",
    "23": "Script Animation Module",
    "24": "Swapchain (Vulkan)",
    "25": "Text and UI Renderer",
    "26": "Script Scene Context",
    "27": "File System Core",
    "28": "Debug Layer",
    "29": "Crash Handler",
    "30": "Coroutine Task",
    "31": "Lighting Manager State",
    "32": "Lighting Compute Pipeline",
    "33": "Asset Manager State",
    "34": "Font Texture Renderer",
    "35": "Vulkan Aftermath",
    "36": "Input System",
    "37": "Quad Renderer",
    "38": "Physics System Update",
    "39": "Local Shadow Service",
}

labels.update(overrides)

Path(".graphify_labels.json").write_text(
    json.dumps(labels, indent=2, ensure_ascii=False), encoding="utf-8"
)
print(f"Generated {len(labels)} labels.")

# Print all for verification
for cid in sorted(labels.keys(), key=int):
    print(f"{cid}: {labels[cid]}")
