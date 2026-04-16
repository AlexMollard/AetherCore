option(AETHERCORE_ENABLE_ASAN "Enable AddressSanitizer on all first-party targets" OFF)

# aethercore_target_defaults(<target>)
#
# Applies project-wide compiler flags to a first-party target.
# Call this on every Engine / App target after its sources are declared.
function(aethercore_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            # --- Warning level & conformance ---
            /W4                      # High warning level
            /permissive-             # Strict standards conformance
            /Zc:preprocessor         # Standards-conforming preprocessor (required for __VA_OPT__ etc.)
            /Zc:__cplusplus          # Report correct __cplusplus value; vk-bootstrap and others check it
            /Zc:inline               # Strip unreferenced functions/data at compile time; reduces link time
            /Zc:templateScope        # Fix template parameter shadowing conformance; hits bugs in generic wrappers

            # --- Promoted warnings → errors ---
            /we4062                  # Switch on enum: unhandled enumerator — critical for VkResult switches
            /we4063                  # Switch on enum: value not a valid enumerator
            /we4715                  # Not all control paths return a value

            # --- Code generation ---
            /MP                      # Multi-processor compilation
            /FS                      # Serialized PDB writes; avoids contention with /MP in VS Debug
            /fp:fast                 # Allow FMA fusion and reciprocal approximations; standard for renderers
            /Gy                      # Function-level linking (COMDAT); required for /OPT:REF,ICF at link time
            /jumptablerdata          # Place jump tables in .rdata instead of .text; free CFG hardening (VS 17.9+)

            # --- External header suppression ---
            /external:anglebrackets  # Treat angle-bracket includes as external/system headers
            /external:W0             # Suppress warnings from external headers (Vulkan, GLM, VMA, etc.)
        )

        # --- Per-config linker flags ---
        # Release: dead-strip unreferenced functions (/OPT:REF) and fold identical COMDATs (/OPT:ICF).
        # Hits real size/speed wins in template-heavy Vulkan dispatch code.
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF,ICF>
            # Full PDB with inlined frame info in RelWithDebInfo — default /DEBUG omits inlined
            # frames, making PIX / RenderDoc / Superluminal call stacks nearly useless.
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        )

        if(AETHERCORE_FAST_MSVC_DEBUG_INFO)
            # Faster local edit-build-run iterations in Debug:
            # - /Z7 stores debug info in .obj (reduces shared PDB contention during compile)
            # - /DEBUG:FASTLINK reduces link-time debug merge cost
            target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Z7>)
            target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:/DEBUG:FASTLINK>)
        endif()

        # Disable the buffer security cookie in Release — zero benefit in GPU-bound render code.
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/GS->)

        # Control Flow Guard — enforces valid indirect call targets (Vulkan function pointer tables).
        # /CETCOMPAT marks the binary as CET shadow-stack compatible (required for signed executables on Win11).
        target_compile_options(${target} PRIVATE /guard:cf)
        target_link_options(${target} PRIVATE /guard:cf /CETCOMPAT)

        # Windows.h guard: NOMINMAX prevents min/max macro pollution that silently
        # corrupts std::min/max and Vulkan structs; WIN32_LEAN_AND_MEAN cuts header bloat.
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -ffast-math              # Equivalent to /fp:fast on GCC/Clang
            -ffunction-sections      # Emit each function into its own section; enables --gc-sections
            -fdata-sections
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>      # Dead-strip unreferenced sections; equivalent to /OPT:REF
            $<$<CONFIG:Release>:-Wl,--icf=all>          # Identical code folding; equivalent to /OPT:ICF
        )

        if(AETHERCORE_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()
