#!/usr/bin/env python3
"""
dead-strip-report.py - find first-party .cpp files that contribute no symbols
to a final executable, by parsing the linker map produced when
AETHERCORE_DEAD_STRIP_REPORT=ON.

The linker map survives only the symbols the link decided were reachable.
A first-party .cpp whose compiled .obj (or .o) does not appear in the map
contributed nothing to the final binary and is a candidate for removal.

Outputs (per map file found):
    <out>/<target>.kept-objects.txt    - .obj / .o basenames with surviving symbols
    <out>/<target>.used-cpp.txt        - first-party .cpp files that DID contribute
    <out>/<target>.unused-cpp.txt      - first-party .cpp files that DID NOT
    <out>/<target>.summary.txt         - per-target counts
    <out>/summary.md                   - aggregate overview

Usage:
    python scripts/dead-strip-report.py \
        --build-dir build/vs2022-msvc \
        --source-dir . \
        --out-dir build/vs2022-msvc/dead-strip-report
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# The MSVC /MAP "Publics by Value" / "static symbols" line shape is:
#   <section:offset>  <symbol...possibly-spaces>  <rva+base>  [<type>...]  <lib:obj | <linker-defined>>
#   - rva+base on x64 is exactly 16 hex digits
#   - Between rva+base and lib:obj there are 0..N type/section tokens ("f", "f i", etc.)
#   - The lib:obj is always the LAST whitespace-separated field on the line
# We anchor on the rva+base and the lib:obj at end-of-line so we tolerate the
# variable type markers.
_MSVC_LINE = re.compile(
    r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+"
    r"(?P<sym>.+?)\s+"
    r"(?P<rva>[0-9A-Fa-f]{16})"
    r"(?P<tail>\s+\S.*)$"
)
_LLVM_OBJ_REF = re.compile(r"(\S+\.o)(?::\([^)]+\))?\s*$")

# First-party source roots relative to the source dir.
_SOURCE_ROOTS = ("src", "tools")

# Subdirectory -> set of executable targets whose link this .cpp can reach.
# Anything not matched falls through to "all targets" so we never silently
# drop a candidate. Update this map when adding new top-level executables.
# Keys are matched as path segments (whole-directory), so an exact match like
# "src/app" is unambiguous: anything whose path contains a "src/app" segment.
_SOURCE_REACH = {
    "src/app":          frozenset({"App"}),
    "src/engine":       frozenset({"App", "AssetPacker"}),
    "tools/assetpack":  frozenset({"AssetPacker"}),
}


def targets_for_source(cpp: Path, all_targets: set[str]) -> frozenset[str]:
    """Which executables could plausibly link this .cpp.

    Determined by directory segment: src/app/* reaches only App, src/engine/*
    reaches both (it is compiled into the Engine static lib that both link),
    tools/assetpack/* reaches only AssetPacker. Anything outside the known
    segments is reported against all discovered targets so new code does
    not silently disappear from the report.
    """
    # Normalise to a leading-slash posix string so the substring check
    # works for both relative ("src/app/main.cpp") and absolute
    # ("C:/.../src/app/main.cpp") paths on every platform.
    needle = "/" + cpp.as_posix().lower()
    for key, reach in _SOURCE_REACH.items():
        if f"/{key}/" in needle:
            return reach & all_targets
    return frozenset(all_targets)


def find_map_files(build_dir: Path):
    """Yield every *.map under the build dir, skipping third-party _deps trees."""
    for p in sorted(build_dir.rglob("*.map")):
        if "_deps" in p.parts:
            continue
        # Skip the report directory itself if the user points --out-dir under the build dir.
        if "dead-strip-report" in p.parts:
            continue
        yield p


def detect_format(path: Path) -> str:
    """Return 'msvc', 'llvm', or 'unknown' based on the first few KB of the map."""
    try:
        head = path.read_text(encoding="utf-8", errors="replace")[:4096]
    except OSError:
        return "unknown"
    if "Timestamp is" in head and "Preferred load address" in head:
        return "msvc"
    if "Address" in head and "Symbol" in head and ".o" in head:
        return "llvm"
    # Fallback: look for .obj references anywhere.
    if ".obj" in head:
        return "msvc"
    if ".o" in head:
        return "llvm"
    return "unknown"


def parse_msvc_map(path: Path) -> set[str]:
    """Extract .obj basenames referenced in an MSVC /MAP file."""
    objs: set[str] = set()
    in_symbols = False
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"warning: cannot read {path}: {e}", file=sys.stderr)
        return objs
    for line in text.splitlines():
        if not in_symbols:
            if "Publics by Value" in line or "static symbols" in line:
                in_symbols = True
            continue
        # Section markers end the symbol table.
        if line.startswith(" ") is False and ":" not in line and line.strip():
            # Heuristic: the symbol table has leading whitespace; section headers do too,
            # so this branch rarely fires. Real terminator is "entry point at".
            if "entry point" in line:
                break
        m = _MSVC_LINE.match(line)
        if not m:
            continue
        # lib:obj is always the last whitespace-separated token in the tail.
        last = m.group("tail").split()[-1]
        if last == "<linker-defined>":
            continue
        if ":" in last:
            obj = last.rsplit(":", 1)[1]
        else:
            obj = last
        if obj.lower().endswith(".obj"):
            objs.add(obj)
    return objs


def parse_llvm_map(path: Path) -> set[str]:
    """Extract .o basenames referenced in an LLVM -Map file."""
    objs: set[str] = set()
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"warning: cannot read {path}: {e}", file=sys.stderr)
        return objs
    for line in text.splitlines():
        if ".o" not in line:
            continue
        m = _LLVM_OBJ_REF.search(line)
        if not m:
            continue
        objs.add(Path(m.group(1)).name)
    return objs


def parse_map(path: Path) -> set[str]:
    fmt = detect_format(path)
    if fmt == "msvc":
        return parse_msvc_map(path)
    if fmt == "llvm":
        return parse_llvm_map(path)
    return set()


def enumerate_project_sources(source_dir: Path) -> list[Path]:
    """All first-party .cpp files (skips build/_deps automatically by being source-only)."""
    out: list[Path] = []
    for root_name in _SOURCE_ROOTS:
        root = source_dir / root_name
        if not root.exists():
            continue
        for p in sorted(root.rglob("*.cpp")):
            out.append(p.resolve())
    return out


def possible_obj_basenames(cpp: Path) -> set[str]:
    """All plausible .obj / .o basenames for a .cpp, across generators.

    cl.exe emits <stem>.obj; CMake's Ninja generator (MSVC or clang-cl)
    renames it to <stem>.cpp.obj to disambiguate. Clang/GCC emit <stem>.o
    or <stem>.cpp.o depending on flags. A .cpp is considered "used" if any
    of these basenames appears in the map.
    """
    stem = cpp.stem
    return {
        stem + ".obj",
        stem + ".cpp.obj",
        stem + ".o",
        stem + ".cpp.o",
    }


def write_set(path: Path, header: list[str], items: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for h in header:
            f.write(h + "\n")
        for it in items:
            f.write(it + "\n")


def classify_sources(
    sources: list[Path],
    used_objs: set[str],
    target: str,
    all_targets: set[str],
) -> tuple[list[Path], list[Path], list[Path]]:
    """Split sources into (used, unused, skipped) for this target.

    skipped = sources that cannot reach this target by directory; they are
    tracked separately so the user can see they were considered, and so
    counts add up to a consistent total.
    """
    used: list[Path] = []
    unused: list[Path] = []
    skipped: list[Path] = []
    for cpp in sources:
        if target not in targets_for_source(cpp, all_targets):
            skipped.append(cpp)
            continue
        if possible_obj_basenames(cpp) & used_objs:
            used.append(cpp)
        else:
            unused.append(cpp)
    return used, unused, skipped


def process_target(
    map_path: Path,
    source_dir: Path,
    out_dir: Path,
    all_targets: set[str],
) -> dict:
    fmt = detect_format(map_path)
    target = map_path.stem  # file is App.map -> target = "App"
    objs = parse_map(map_path)
    sources = enumerate_project_sources(source_dir)
    used, unused, skipped = classify_sources(sources, objs, target, all_targets)

    rel_map = map_path
    write_set(
        out_dir / f"{target}.kept-objects.txt",
        [
            f"# Objects with surviving symbols in {target}.map",
            f"# Map file: {rel_map}",
            f"# Format: {fmt}",
            f"# Total objects: {len(objs)}",
            "",
        ],
        sorted(objs),
    )
    write_set(
        out_dir / f"{target}.used-cpp.txt",
        [
            f"# {target}: first-party .cpp files whose compiled object IS referenced",
            f"# Map file: {rel_map}",
            "",
        ],
        [str(p) for p in used],
    )
    write_set(
        out_dir / f"{target}.unused-cpp.txt",
        [
            f"# {target}: first-party .cpp files whose compiled object is NOT referenced.",
            f"# These files contributed nothing to {target}.exe and are candidates",
            f"# for removal. VERIFY FIRST - some headers may still be required even",
            f"# though no .obj symbols are kept (e.g. template-only headers).",
            f"# Map file: {rel_map}",
            "",
        ],
        [str(p) for p in unused],
    )
    write_set(
        out_dir / f"{target}.skipped-cpp.txt",
        [
            f"# {target}: first-party .cpp files NOT considered for this target",
            f"# (they belong to other targets and are listed here for transparency).",
            f"# Map file: {rel_map}",
            "",
        ],
        [str(p) for p in skipped],
    )
    eligible = len(used) + len(unused)
    summary = {
        "target": target,
        "map": str(rel_map),
        "format": fmt,
        "objects": len(objs),
        "sources": len(sources),
        "eligible": eligible,
        "used": len(used),
        "unused": len(unused),
        "skipped": len(skipped),
    }
    write_set(
        out_dir / f"{target}.summary.txt",
        [
            f"target: {target}",
            f"map: {rel_map}",
            f"format: {fmt}",
            f"objects: {len(objs)}",
            f"sources: {len(sources)}",
            f"eligible: {eligible}",
            f"used: {len(used)}",
            f"unused: {len(unused)}",
            f"skipped: {len(skipped)}",
        ],
        [],
    )
    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, help="CMake build directory")
    ap.add_argument("--source-dir", required=True, help="Source tree root (contains src/ and tools/)")
    ap.add_argument("--out-dir", required=True, help="Where to write reports")
    args = ap.parse_args()

    build = Path(args.build_dir).resolve()
    src = Path(args.source_dir).resolve()
    out = Path(args.out_dir).resolve()
    if not build.is_dir():
        print(f"error: build dir not found: {build}", file=sys.stderr)
        return 2
    if not src.is_dir():
        print(f"error: source dir not found: {src}", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)

    maps = list(find_map_files(build))
    if not maps:
        print(
            f"error: no .map files found under {build}.",
            file=sys.stderr,
        )
        print(
            f"  Did you reconfigure with -DAETHERCORE_DEAD_STRIP_REPORT=ON",
            file=sys.stderr,
        )
        print(
            f"  and then build (cmake --build --preset <preset>)?",
            file=sys.stderr,
        )
        return 1

    print(f"Found {len(maps)} map file(s) under {build}")
    # All discovered target names, used to decide per-source reachability
    # (so a .cpp in an unrecognised dir still appears in every target's report).
    all_targets = {m.stem for m in maps}
    summaries = []
    for m in maps:
        s = process_target(m, src, out, all_targets)
        summaries.append(s)
        print(
            f"  {s['target']:>16}: {s['objects']:>5} kept objects, "
            f"{s['used']:>4}/{s['eligible']:>4} eligible contributing, "
            f"{s['unused']:>4} candidates, "
            f"{s['skipped']:>4} skipped (other targets)"
        )

    md = ["# Dead-strip report", ""]
    md.append(f"Build dir: `{build}`")
    md.append("")
    md.append("| Target | Format | Kept objects | Eligible | Contributing | Unused | Skipped |")
    md.append("|---|---|---:|---:|---:|---:|---:|")
    for s in summaries:
        md.append(
            f"| `{s['target']}` | {s['format']} | {s['objects']} | {s['eligible']} | "
            f"{s['used']} | **{s['unused']}** | {s['skipped']} |"
        )
    md.append("")
    md.append("## Per-target reports")
    md.append("")
    for s in summaries:
        md.append(f"- `{s['target']}.unused-cpp.txt` - {s['unused']} candidate(s) for removal")
        md.append(f"- `{s['target']}.used-cpp.txt` - {s['used']} contributing")
        md.append(f"- `{s['target']}.skipped-cpp.txt` - {s['skipped']} belonging to other targets")
        md.append(f"- `{s['target']}.kept-objects.txt` - {s['objects']} object file(s)")
        md.append("")
    md.append("## How the per-target filter works")
    md.append("")
    md.append("- `src/app/*.cpp` are only considered for `App`.")
    md.append("- `tools/assetpack/*.cpp` are only considered for `AssetPacker`.")
    md.append("- `src/engine/*.cpp` are considered for both (they are compiled into")
    md.append("  the `Engine` static lib which both executables link).")
    md.append("- `.cpp` files outside these dirs are reported against every target")
    md.append("  so new code is never silently hidden from the report.")
    md.append("")
    md.append("## Caveats")
    md.append("")
    md.append("- A .cpp showing as 'unused' only means its compiled object did not")
    md.append("  contribute symbols to *this* executable. Verify by deleting the")
    md.append("  .cpp and rebuilding, or use `grep` to confirm no header reference.")
    md.append("- Template / inline / header-only code is attributed to the .cpp that")
    md.append("  first instantiates it. A header whose templates are used in this")
    md.append("  executable will not appear as 'unused' even if you intended to")
    md.append("  delete the header.")
    md.append("- The link map is a snapshot of one build configuration. Run again")
    md.append("  after changing features (debug, release, with/without Tracy, etc.)")
    md.append("  to get a complete picture.")
    (out / "summary.md").write_text("\n".join(md) + "\n", encoding="utf-8")

    print()
    print(f"Reports written to: {out}")
    print(f"Overview: {out / 'summary.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
