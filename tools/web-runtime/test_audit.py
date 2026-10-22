import tempfile
from pathlib import Path
import unittest

from audit import validate_host_binary, wasm_imports, audit_imports, audit_sources


class CrossBuildBoundaryTests(unittest.TestCase):
    def test_browser_does_not_silently_override_dependency_pins(self):
        source = (Path(__file__).resolve().parents[2] / "CMake/WebRuntime.cmake").read_text(encoding="utf-8")
        self.assertNotIn("set(CPM_${_name}_SOURCE", source)

    def test_thread_flag_is_rejected_in_arguments_compilation_database(self):
        with self.assertRaises(ValueError):
            audit_sources([dict(file="src/engine/scene/World.cpp", arguments=["em++", "-pthread", "-c"])])

    def test_actual_compile_graph_rejects_native_worker_sources(self):
        world = dict(file="src/engine/scene/World.cpp", command="em++ -c")
        self.assertEqual(audit_sources([world]), [world["file"]])
        for source in ("src/engine/io/IOThread.cpp", "src/engine/vulkan/VulkanContext.cpp", "src/engine/net/Network.cpp"):
            with self.assertRaises(ValueError):
                audit_sources([world, dict(file=source)])
        with self.assertRaises(ValueError):
            audit_sources([])

    def test_jolt_and_physics_are_allowed_but_still_single_threaded(self):
        world = dict(file="src/engine/scene/World.cpp", command="em++ -c")
        jolt = dict(file="build/_deps/joltphysics-src/Jolt/Physics/PhysicsSystem.cpp", command="em++ -c")
        physics = dict(file="src/engine/physics/PhysicsSystem.cpp", command="em++ -c")
        self.assertEqual(len(audit_sources([world, jolt, physics])), 3)
        with self.assertRaises(ValueError):
            audit_sources([world, dict(file=jolt["file"], command="em++ -pthread -c")])
    def test_wasm_cannot_be_disguised_as_host_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "AssetPacker.exe"
            path.write_bytes(b"\0asm\x01\0\0\0")
            with self.assertRaisesRegex(ValueError, "Wasm"):
                validate_host_binary(path)

    def test_js_and_missing_tools_fail_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "slangc"
            path.write_text("var Module = {};", encoding="utf-8")
            with self.assertRaises(ValueError):
                validate_host_binary(path)
            with self.assertRaises(ValueError):
                validate_host_binary(path.parent / "missing")

    def test_native_elf_header_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "AssetPacker"
            path.write_bytes(b"\x7fELF" + bytes(60))
            validate_host_binary(path)

    def test_import_audit_rejects_native_network_and_thread_imports(self):
        for name in ("vkCreateInstance", "hostfxr_initialize", "socket", "enet_host_create", "pthread_create"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit_imports([("env", name)])
        audit_imports([("wasi_snapshot_preview1", "fd_write")])

    def test_actual_wasm_import_section_is_parsed_and_truncation_rejected(self):
        # One imported function env.socket with type index zero.
        payload = b"\x01\x03env\x06socket\x00\x00"
        module = b"\0asm\x01\0\0\0\x02" + bytes([len(payload)]) + payload
        self.assertEqual(wasm_imports(module), [("env", "socket")])
        with self.assertRaises(ValueError):
            wasm_imports(module[:-1])

    def test_shared_memory_is_rejected_even_without_thread_function_imports(self):
        module = b"\0asm\x01\0\0\0\x05\x04\x01\x03\x01\x02"
        with self.assertRaisesRegex(ValueError, "Shared"):
            wasm_imports(module)


if __name__ == "__main__":
    unittest.main()
