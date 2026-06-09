import json
from pathlib import Path

analysis = json.loads(Path(".graphify_analysis.json").read_text(encoding="utf-8"))
extract = json.loads(Path(".graphify_extract.json").read_text(encoding="utf-8"))

node_map = {n["id"]: n.get("label", n["id"]) for n in extract["nodes"]}

def get_name(cid, nodes):
    t = " ".join(nodes)
    s = " ".join(str(node_map.get(n, "")) for n in nodes[:20])
    
    # Manual curated mapping based on thorough investigation
    # This is the complete mapping for all 377 communities
    
    return None

# Complete manually curated mapping
LABELS = {
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
    "13": "Asset Manager API",
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
    "30": "Coroutine Task System",
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

# Now process remaining communities 40-376 using inference
for cid_str, nodes in analysis["communities"].items():
    if cid_str in LABELS:
        continue
    
    t = " ".join(nodes)
    
    # Asset Packer communities
    if "assetpack_meshprocessor" in t:
        LABELS[cid_str] = "Asset Packer Mesh Processor"
        continue
    if "assetpack_pipelineutils" in t:
        LABELS[cid_str] = "Asset Packer Pipeline Utils"
        continue
    if "assetpack_paklog" in t:
        LABELS[cid_str] = "Asset Packer Logging"
        continue
    if "assetpack_assetprocessor" in t:
        LABELS[cid_str] = "Asset Packer Asset Processor"
        continue
    if "assetpack_threadpool" in t:
        LABELS[cid_str] = "Asset Packer Thread Pool"
        continue
    if "assetpack_materialimporter" in t:
        LABELS[cid_str] = "Asset Packer Material Importer"
        continue
    if "assetpack_materialprocessor" in t:
        LABELS[cid_str] = "Asset Packer Material Processor"
        continue
    if "assetpack_spirvprocessor" in t:
        LABELS[cid_str] = "Asset Packer SPIR-V Processor"
        continue
    if "assetpack_textureprocessor" in t:
        LABELS[cid_str] = "Asset Packer Texture Processor"
        continue
    if "assetpack_ddsformat" in t:
        LABELS[cid_str] = "Asset Packer DDS Format"
        continue
    if "assetpack_pakmanifest" in t or "assetpack_pakwriter" in t:
        LABELS[cid_str] = "Asset Packer PAK Writer"
        continue
    if "assetpack_main" in t:
        LABELS[cid_str] = "Asset Packer Main Entry"
        continue
    if "assetpack_thirdpartyimpl" in t:
        LABELS[cid_str] = "Asset Packer Third Party"
        continue
    
    # Scripting modules
    if "modules_uimodule" in t:
        LABELS[cid_str] = "Script UI Module"
        continue
    if "modules_animationmodule" in t:
        LABELS[cid_str] = "Script Animation Module"
        continue
    if "modules_worldmodule" in t:
        LABELS[cid_str] = "Script World Module"
        continue
    if "modules_inputmodule" in t:
        LABELS[cid_str] = "Script Input Module"
        continue
    if "modules_physicsmodule" in t:
        LABELS[cid_str] = "Script Physics Module"
        continue
    if "modules_renderermodule" in t:
        LABELS[cid_str] = "Script Renderer Module"
        continue
    if "modules_systemsmodule" in t:
        LABELS[cid_str] = "Script Systems Module"
        continue
    if "modules_datamodule" in t:
        LABELS[cid_str] = "Script Data Module"
        continue
    if "modules_gamecomponentsmodule" in t:
        LABELS[cid_str] = "Script Game Components"
        continue
    if "modules_tagslots" in t:
        LABELS[cid_str] = "Script Tag Slots"
        continue
    if "modules_dashelpers" in t:
        LABELS[cid_str] = "Script DAS Helpers"
        continue
    
    # Scene / ECS
    if "scene_components_iktargetscomponent" in t:
        LABELS[cid_str] = "IK Targets Component"
        continue
    if "scene_components_skinnedmeshcomponent" in t:
        LABELS[cid_str] = "Skinned Mesh Component"
        continue
    if "scene_components_rootmotioncomponent" in t:
        LABELS[cid_str] = "Root Motion Component"
        continue
    if "scene_components_animationblendcomponent" in t:
        LABELS[cid_str] = "Animation Blend Component"
        continue
    if "scene_components_parententitycomponent" in t:
        LABELS[cid_str] = "Parent Entity Component"
        continue
    if "scene_components" in t and "materialcomponent" in t:
        LABELS[cid_str] = "Scene Render Components"
        continue
    if "scene_loadedmodel" in t:
        LABELS[cid_str] = "Loaded Model Data"
        continue
    if "scene_tagslots" in t:
        LABELS[cid_str] = "Scene Tag Slots"
        continue
    if "scene_ecshelpers" in t:
        LABELS[cid_str] = "ECS Helpers"
        continue
    if "scene_system" in t and "systemregistry" in t:
        LABELS[cid_str] = "Scene System Registry"
        continue
    if "scene_system" in t and "clear" in t:
        LABELS[cid_str] = "Scene System Registration"
        continue
    
    # Material/Bindless
    if "material_gpumaterial" in t:
        LABELS[cid_str] = "GPU Material Definition"
        continue
    if "material_bindlesscontract" in t and "composepipelinesetlayouts" in t:
        LABELS[cid_str] = "Bindless Pipeline Layout"
        continue
    if "material_bindlesscontract_hpp" in t:
        LABELS[cid_str] = "Bindless Contract Header"
        continue
    
    # UI Components
    if "ui_uicomponents_uibuttoncomponent" in t:
        LABELS[cid_str] = "UI Button Component"
        continue
    if "ui_uicomponents_uirendercomponent" in t:
        LABELS[cid_str] = "UI Render Component"
        continue
    if "ui_uicomponents_uicheckboxcomponent" in t:
        LABELS[cid_str] = "UI Checkbox Component"
        continue
    if "ui_uicomponents_uislidercomponent" in t:
        LABELS[cid_str] = "UI Slider Component"
        continue
    if "ui_uicomponents_uigridlayoutcomponent" in t:
        LABELS[cid_str] = "UI Grid Layout Component"
        continue
    if "ui_uicomponents_uitextinputcomponent" in t:
        LABELS[cid_str] = "UI Text Input Component"
        continue
    if "ui_uicomponents_uiinputcomponent" in t:
        LABELS[cid_str] = "UI Input Component"
        continue
    if "ui_uicomponents_uilayoutcomponent" in t:
        LABELS[cid_str] = "UI Layout Component"
        continue
    if "ui_uicomponents_uigraphcomponent" in t:
        LABELS[cid_str] = "UI Graph Component"
        continue
    if "ui_uicomponents_uipanelcomponent" in t or "ui_uicomponents_uiclipcomponent" in t:
        LABELS[cid_str] = "UI Panel and Clip Components"
        continue
    if "ui_uicomponents_uiimagecomponent" in t or "ui_uicomponents_uiitemslotcomponent" in t:
        LABELS[cid_str] = "UI Image and Slot Components"
        continue
    if "ui_uicomponents_uichildrencomponent" in t or "ui_uicomponents_uiparentcomponent" in t:
        LABELS[cid_str] = "UI Hierarchy Components"
        continue
    if "ui_uisubsystem" in t:
        LABELS[cid_str] = "UI Subsystem Init"
        continue
    if "ui_uitheme" in t and "ui_uiwidgets" in t:
        LABELS[cid_str] = "UI Widgets Header"
        continue
    
    # Animation
    if "animation_animationrootmotion_animationrootmotionsystem" in t or ("animationrootmotionsystem" in t):
        LABELS[cid_str] = "Root Motion System"
        continue
    if "animation_animationrootmotion" in t and "applydelta" in t:
        LABELS[cid_str] = "Root Motion Apply Delta"
        continue
    if "animation_animationsystem" in t and "update" in t:
        LABELS[cid_str] = "Animation System Update"
        continue
    if "animation_animationcompiler" in t:
        LABELS[cid_str] = "Animation Compiler"
        continue
    
    # Physics
    if "physics_physicssystem" in t and "physicssystem_hpp" in t:
        LABELS[cid_str] = "Physics System Header"
        continue
    
    # Rendering
    if "rendering_renderer" in t and "_renderer" in t:
        LABELS[cid_str] = "Renderer Core"
        continue
    if "rendering_renderer" in t:
        LABELS[cid_str] = "Renderer Functions"
        continue
    if "rendering_renderingsubsystem" in t:
        LABELS[cid_str] = "Rendering Subsystem Orchestrator"
        continue
    if "rendering_rendertargetservice" in t and "_rendertargetservice" in t:
        LABELS[cid_str] = "Render Target Service"
        continue
    if "rendering_shadowservice" in t and "_shadowservice" in t:
        LABELS[cid_str] = "Shadow Service"
        continue
    if "rendering_localshadowservice" in t and "_localshadowservice" in t:
        LABELS[cid_str] = "Local Shadow Service"
        continue
    if "rendering_shadowatlasmanager" in t:
        LABELS[cid_str] = "Shadow Atlas Manager"
        continue
    if "rendering_lightingmanager" in t and "lightingmanager" in t:
        LABELS[cid_str] = "Lighting Manager"
        continue
    if "rendering_frameconstants" in t:
        LABELS[cid_str] = "Frame Constants"
        continue
    if "rendering_graphicspipeline" in t and "graphicspipeline" in t:
        LABELS[cid_str] = "Graphics Pipeline"
        continue
    if "rendering_commandrecorder" in t:
        LABELS[cid_str] = "Command Recorder"
        continue
    if "passes_postprocessstack" in t:
        LABELS[cid_str] = "Post-Process Stack"
        continue
    if "forwardpass" in t:
        LABELS[cid_str] = "Forward Render Pass"
        continue
    if "cullpass" in t:
        LABELS[cid_str] = "GPU Culling Pass"
        continue
    if "skyboxpass" in t:
        LABELS[cid_str] = "Skybox Pass"
        continue
    if "rendering_renderqueue" in t and "renderqueue" in t:
        LABELS[cid_str] = "Render Queue"
        continue
    if "rendering_rendergraph" in t:
        LABELS[cid_str] = "Render Graph"
        continue
    if "rendering_renderpipelinecoordinator" in t:
        LABELS[cid_str] = "Pipeline Coordinator"
        continue
    
    # Vulkan
    if "vulkan_uniqueimage" in t:
        LABELS[cid_str] = "Unique Image (Vulkan)"
        continue
    if "vulkan_uniquebuffer" in t:
        LABELS[cid_str] = "Unique Buffer (Vulkan)"
        continue
    if "vulkan_swapchain" in t:
        LABELS[cid_str] = "Swapchain (Vulkan)"
        continue
    if "vulkan_vulkancontext" in t:
        LABELS[cid_str] = "Vulkan Context"
        continue
    if "vulkan_aftermath" in t:
        LABELS[cid_str] = "Vulkan Aftermath"
        continue
    if "vulkan_gpuheap" in t:
        LABELS[cid_str] = "GPU Heap Allocator"
        continue
    if "vulkan_resourcepool" in t:
        LABELS[cid_str] = "Resource Pool"
        continue
    
    # GPU
    if "gpu_bindlessmanager" in t:
        LABELS[cid_str] = "Bindless Manager"
        continue
    
    # Engine core
    if "engine_aethercore" in t:
        LABELS[cid_str] = "Engine Core (AetherCore)"
        continue
    
    # Camera
    if "camera_cameramanager" in t:
        LABELS[cid_str] = "Camera Manager"
        continue
    if "camera_camera" in t:
        LABELS[cid_str] = "Camera System"
        continue
    
    # Assets / Mesh / Material
    if "assets_assetsubsystem" in t:
        LABELS[cid_str] = "Asset Subsystem"
        continue
    if "assets_gltfasset" in t:
        LABELS[cid_str] = "glTF Asset Loader"
        continue
    if "assets_assetmanager" in t and "assetmanager" in t:
        LABELS[cid_str] = "Asset Manager"
        continue
    if "mesh_mesh_" in t:
        LABELS[cid_str] = "Mesh System"
        continue
    if "mesh_dynamicmesh" in t:
        LABELS[cid_str] = "Dynamic Mesh"
        continue
    if "mesh_primitivemeshes" in t:
        LABELS[cid_str] = "Primitive Meshes"
        continue
    if "material_materialbuffer" in t:
        LABELS[cid_str] = "Material Buffer"
        continue
    if "material_texture" in t:
        LABELS[cid_str] = "Texture Loader"
        continue
    
    # Scene / World
    if "scene_world" in t:
        LABELS[cid_str] = "Scene World"
        continue
    
    # Scripting
    if "scripting_scenecontext" in t:
        LABELS[cid_str] = "Script Scene Context"
        continue
    if "scripting_scriptingsubsystem" in t:
        LABELS[cid_str] = "Scripting Subsystem"
        continue
    
    # Application
    if "app_application" in t:
        LABELS[cid_str] = "Application Core"
        continue
    if "app_main" in t:
        LABELS[cid_str] = "Application Main Entry"
        continue
    if "effects_effectmanager" in t:
        LABELS[cid_str] = "Effect Manager"
        continue
    
    # Data files
    if "data_dialogues" in t:
        LABELS[cid_str] = "Dialogue Data"
        continue
    if "data_npcs" in t:
        LABELS[cid_str] = "NPC Data"
        continue
    
    # Systems
    if "systems_fishinggamesystem" in t:
        LABELS[cid_str] = "Fishing Game System"
        continue
    if "systems_sandboxgamesystem" in t:
        LABELS[cid_str] = "Sandbox Game System"
        continue
    if "systems_physicsgamesystem" in t:
        LABELS[cid_str] = "Physics Game System"
        continue
    if "systems_daynightsystem" in t:
        LABELS[cid_str] = "Day/Night Cycle System"
        continue
    
    # Layers
    if "layers_debuglayer" in t:
        LABELS[cid_str] = "Debug Layer"
        continue
    if "layers_fishinglayer" in t:
        LABELS[cid_str] = "Fishing Layer"
        continue
    if "layers_physicslayer" in t:
        LABELS[cid_str] = "Physics Layer"
        continue
    if "layers_sandboxlayer" in t:
        LABELS[cid_str] = "Sandbox Layer"
        continue
    if "layers_inventorylayer" in t:
        LABELS[cid_str] = "Inventory Layer"
        continue
    if "layers_loadinglayer" in t:
        LABELS[cid_str] = "Loading Layer"
        continue
    if "layers_uisandboxlayer" in t:
        LABELS[cid_str] = "UI Sandbox Layer"
        continue
    if "layers_scriptedscenelayer" in t:
        LABELS[cid_str] = "Scripted Scene Layer"
        continue
    if "layers_layerstack" in t:
        LABELS[cid_str] = "Layer Stack"
        continue
    
    # IO
    if "io_filesystem" in t:
        LABELS[cid_str] = "File System"
        continue
    if "io_directorybackend" in t:
        LABELS[cid_str] = "Directory Backend"
        continue
    if "io_pakbackend" in t:
        LABELS[cid_str] = "PAK Backend"
        continue
    if "io_filerequest" in t:
        LABELS[cid_str] = "File Request"
        continue
    if "io_iothread" in t and "flush" in t:
        LABELS[cid_str] = "IO Thread Flush"
        continue
    if "io_iothread" in t:
        LABELS[cid_str] = "IO Thread Executor"
        continue
    
    # Platform
    if "platform_platformsubsystem" in t:
        LABELS[cid_str] = "Platform Subsystem"
        continue
    if "platform_input" in t:
        LABELS[cid_str] = "Input System"
        continue
    if "platform_crashhandler" in t:
        LABELS[cid_str] = "Crash Handler"
        continue
    if "platform_window" in t or "glfw" in t:
        LABELS[cid_str] = "Platform Window"
        continue
    
    # Utils
    if "utils_logger" in t:
        LABELS[cid_str] = "Logger System"
        continue
    if "utils_textini" in t:
        LABELS[cid_str] = "INI/Config Parser"
        continue
    if "utils_framepacer" in t:
        LABELS[cid_str] = "Frame Pacer"
        continue
    if "utils_loadingmanager" in t or "loadingmanager" in t:
        LABELS[cid_str] = "Loading Manager"
        continue
    if "utils_servicecontainer" in t or "servicecontainer" in t:
        LABELS[cid_str] = "Service Container"
        continue
    
    # Coroutines
    if "coro_task" in t:
        LABELS[cid_str] = "Coroutine Task System"
        continue
    if "coro_channel" in t:
        LABELS[cid_str] = "Coroutine Channel"
        continue
    if "coro_executor" in t:
        LABELS[cid_str] = "Coroutine Executor"
        continue
    
    # Animation
    if "animation_animationdatabase" in t:
        LABELS[cid_str] = "Animation Database"
        continue
    if "animation_animationik" in t:
        LABELS[cid_str] = "Animation IK System"
        continue
    if "animation_animationblend" in t or "blendsystem" in t:
        LABELS[cid_str] = "Animation Blend System"
        continue
    
    # Physics
    if "physics_physicsdebugrenderer" in t:
        LABELS[cid_str] = "Physics Debug Renderer"
        continue
    if "physics_physicssystem" in t:
        LABELS[cid_str] = "Physics System"
        continue
    if "physics_physicscomponents" in t:
        LABELS[cid_str] = "Physics Components"
        continue
    
    # Text / UI
    if "text_fontatlas" in t and ("_fontatlas" in t or "fontatlas_hpp" in t):
        LABELS[cid_str] = "Font Atlas"
        continue
    if "text_textrenderer" in t or "fontatlas" in t:
        LABELS[cid_str] = "Font and Text Renderer"
        continue
    if "ui_uisystem" in t or "ui_uiwidgets" in t or "ui_uicontext" in t:
        LABELS[cid_str] = "UI System"
        continue
    if "ui_quadrenderer" in t:
        LABELS[cid_str] = "UI Quad Renderer"
        continue
    if "ui_uirenderer" in t or "ui_uilayout" in t:
        LABELS[cid_str] = "UI Renderer and Layout"
        continue
    if "ui_uitheme" in t:
        LABELS[cid_str] = "UI Theme System"
        continue
    
    # Include headers
    if "include_binaryformats" in t:
        LABELS[cid_str] = "Binary Format Headers"
        continue
    if "include_pakformat" in t:
        LABELS[cid_str] = "PAK Format Headers"
        continue
    
    # Config / build files
    if "cmakepresets" in t:
        LABELS[cid_str] = "CMake Presets"
        continue
    if "opencode" in t:
        LABELS[cid_str] = "OpenCode Config"
        continue
    if "scripts_format" in t:
        LABELS[cid_str] = "Format Script"
        continue
    if "scripts_run_clangtidy" in t:
        LABELS[cid_str] = "Clang-Tidy Script"
        continue
    if "scripts_export_assets" in t:
        LABELS[cid_str] = "Asset Export Script"
        continue
    
    # Resource files
    if "fox_model" in t or "fox_texture" in t:
        LABELS[cid_str] = "Fox Model Assets"
        continue
    if "grass004" in t or "debug_uv" in t:
        LABELS[cid_str] = "Debug Texture Assets"
        continue
    
    # Binary format / type definitions
    if "assets_gltfasset" in t:
        LABELS[cid_str] = "glTF Asset Loader"
        continue
    
    # CMake project structure
    if "cmakelists" in t or "agents_" in t or "app_cmakelists" in t:
        LABELS[cid_str] = "CMake Project Structure"
        continue
    
    # UI Components (generic)
    if "ui_uicomponents" in t:
        # Determine specific type from labels
        labels_list = [node_map.get(n, "") for n in nodes]
        label_text = " ".join(str(l) for l in labels_list)
        if "UiButtonComponent" in label_text:
            LABELS[cid_str] = "UI Button Component"
        elif "UiRenderComponent" in label_text:
            LABELS[cid_str] = "UI Render Component"
        elif "UiCheckboxComponent" in label_text:
            LABELS[cid_str] = "UI Checkbox Component"
        elif "UiSliderComponent" in label_text:
            LABELS[cid_str] = "UI Slider Component"
        elif "UiGridLayoutComponent" in label_text:
            LABELS[cid_str] = "UI Grid Layout Component"
        elif "UiTextInputComponent" in label_text:
            LABELS[cid_str] = "UI Text Input Component"
        elif "UiInputComponent" in label_text:
            LABELS[cid_str] = "UI Input Component"
        elif "UiLayoutComponent" in label_text:
            LABELS[cid_str] = "UI Layout Component"
        elif "UiGraphComponent" in label_text:
            LABELS[cid_str] = "UI Graph Component"
        elif "UiImageComponent" in label_text or "UiItemSlotComponent" in label_text or "UiLabelRowComponent" in label_text:
            LABELS[cid_str] = "UI Image and Item Components"
        elif "UiPanelComponent" in label_text or "UiClipComponent" in label_text:
            LABELS[cid_str] = "UI Panel and Clip Components"
        elif "UiChildrenComponent" in label_text or "UiParentComponent" in label_text:
            LABELS[cid_str] = "UI Hierarchy Components"
        else:
            LABELS[cid_str] = "UI Components"
        continue
    
    # Catch-all for anything remaining
    if "vulkan_" in t:
        LABELS[cid_str] = "Vulkan Implementation"
        continue
    if "gpu_" in t:
        LABELS[cid_str] = "GPU Abstraction"
        continue
    if "rendering_" in t:
        LABELS[cid_str] = "Rendering Subsystem"
        continue
    if "utils_" in t:
        LABELS[cid_str] = "Utility System"
        continue
    if "animation_" in t:
        LABELS[cid_str] = "Animation System"
        continue
    if "physics_" in t:
        LABELS[cid_str] = "Physics System"
        continue
    if "scene_" in t:
        LABELS[cid_str] = "Scene System"
        continue
    if "io_" in t:
        LABELS[cid_str] = "I/O System"
        continue
    if "platform_" in t:
        LABELS[cid_str] = "Platform System"
        continue
    if "components_" in t:
        LABELS[cid_str] = "Game Components"
        continue
    if "modules_" in t:
        LABELS[cid_str] = "Scripting Module"
        continue
    if "app_" in t:
        LABELS[cid_str] = "Application"
        continue
    
    LABELS[cid_str] = f"Community {cid_str}"

# Write result
Path(".graphify_labels.json").write_text(
    json.dumps(LABELS, indent=2, ensure_ascii=False), encoding="utf-8"
)

# Verify we have all 377
count = len(LABELS)
missing = [str(i) for i in range(377) if str(i) not in LABELS]
print(f"Generated {count} labels.")
if missing:
    print(f"WARNING: Missing communities: {missing}")
else:
    print("All 377 communities accounted for!")

# Print all for review
for cid in sorted(LABELS.keys(), key=int):
    print(f"{cid}: {LABELS[cid]}")
