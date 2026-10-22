"""Cross-build boundaries: native executable validation and binary Wasm imports."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess


def validate_host_binary(path):
    path = Path(path)
    if not path.is_file():
        raise ValueError("Host executable is missing: " + str(path))
    with path.open("rb") as stream:
        magic = stream.read(4)
    if magic == b"\0asm":
        raise ValueError("Wasm binary cannot execute as a host build tool: " + str(path))
    if not (magic[:2] == b"MZ" or magic == b"\x7fELF" or magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe")):
        raise ValueError("Host tool must be a native executable, not JS/a cross-build output: " + str(path))


def wasm_imports(data):
    if data[:8] != b"\0asm\x01\0\0\0":
        raise ValueError("Not a version-1 Wasm module")
    cursor = 8

    def integer(limit):
        nonlocal cursor
        value = 0
        for shift in range(0, 35, 7):
            if cursor >= limit:
                raise ValueError("Truncated Wasm integer")
            byte = data[cursor]
            cursor += 1
            value |= (byte & 127) << shift
            if not byte & 128:
                return value
        raise ValueError("Oversized Wasm integer")

    def name(limit):
        nonlocal cursor
        size = integer(limit)
        if cursor + size > limit:
            raise ValueError("Truncated Wasm name")
        value = data[cursor:cursor + size].decode("utf-8")
        cursor += size
        return value

    def limits(end):
        flags = integer(end)
        if flags & 2:
            raise ValueError("Shared Wasm memory is forbidden in the first browser profile")
        integer(end)
        if flags & 1:
            integer(end)

    imports = []
    while cursor < len(data):
        section = data[cursor]
        cursor += 1
        length = integer(len(data))
        end = cursor + length
        if end > len(data):
            raise ValueError("Truncated Wasm section")
        if section == 2:
            for _ in range(integer(end)):
                imports.append((name(end), name(end)))
                kind = integer(end)
                if kind == 0:
                    integer(end)
                elif kind == 1:
                    integer(end)
                    limits(end)
                elif kind == 2:
                    limits(end)
                elif kind == 3:
                    integer(end)
                    integer(end)
                elif kind == 4:
                    integer(end)
                    integer(end)
                else:
                    raise ValueError("Unknown Wasm import descriptor")
        elif section == 5:
            for _ in range(integer(end)):
                limits(end)
        cursor = end
    return imports


def audit_imports(imports):
    forbidden = ("hostfxr", "nethost", "coreclr", "vulkan", "vkcreate", "enet_", "socket", "connect", "listen", "pthread_create", "thrd_create", "emscripten_thread_init")
    for module, name in imports:
        if any(token in (module + "." + name).lower() for token in forbidden):
            raise ValueError("Forbidden browser import: " + module + "." + name)


def audit_sources(commands):
    sources = []
    for entry in commands:
        source = entry["file"].replace("\\", "/")
        lower = source.lower()
        forbidden = ("/vulkan/", "/editor/", "/net/", "/enet", "/libplum", "iothread.cpp", "assetmanager.cpp", "/renderthread.cpp", "/logger.cpp", "dotnethost.cpp", "audio/audiosubsystem.cpp")
        command = entry.get("command", "") + " " + " ".join(entry.get("arguments", []))
        if any(token in lower for token in forbidden) or "-pthread" in command:
            raise ValueError("Forbidden browser source/worker: " + source)
        sources.append(source)
    if not any(source.endswith("scene/World.cpp") for source in sources):
        raise ValueError("Browser runtime must compile the real engine World")
    return sorted(set(sources))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    tool = sub.add_parser("tool")
    tool.add_argument("path", type=Path)
    tool.add_argument("--kind", choices=("assetpacker", "slang"), required=True)
    audit = sub.add_parser("wasm")
    audit.add_argument("path", type=Path)
    audit.add_argument("--report", type=Path, required=True)
    graph = sub.add_parser("sources")
    graph.add_argument("path", type=Path)
    graph.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.operation == "tool":
        validate_host_binary(args.path)
        command = [str(args.path), "-version" if args.kind == "slang" else "--help"]
        result = subprocess.run(command, capture_output=True, timeout=15)
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        pins = json.loads((Path(__file__).resolve().parents[2] / "tools/experiments/rust-webgpu/toolchain.json").read_text(encoding="utf-8"))
        expected = pins["slang"] if args.kind == "slang" else "Usage: AssetPacker"
        if expected not in output or result.returncode not in (0, 1):
            raise ValueError("Host tool identity/version check failed: " + output)
        print("Verified native host " + args.kind + ": " + str(args.path))
    elif args.operation == "wasm":
        imports = wasm_imports(args.path.read_bytes())
        audit_imports(imports)
        args.report.write_text(json.dumps(dict(status="passed", sharedMemory=False, imports=imports), indent=2), encoding="utf-8")
        print("Wasm native-import and shared-memory audit passed")
    else:
        sources = audit_sources(json.loads(args.path.read_text(encoding="utf-8")))
        args.report.write_text(json.dumps(dict(status="passed", sources=sources, sourceListSha256=hashlib.sha256("\n".join(sources).encode()).hexdigest()), indent=2), encoding="utf-8")
        print("Browser source/worker closure audit passed: " + str(len(sources)) + " sources")


if __name__ == "__main__":
    main()
