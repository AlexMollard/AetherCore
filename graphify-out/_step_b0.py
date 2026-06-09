import json
from graphify.detect import detect
from graphify.cache import check_semantic_cache
from pathlib import Path

result = detect(Path("."))
Path("graphify-out/.graphify_detect.json").write_text(
    json.dumps(result, ensure_ascii=False), encoding="utf-8"
)

all_files = []
for ftype in ["document", "image", "paper", "video"]:
    all_files.extend(result["files"].get(ftype, []))

print(f"Non-code files: {len(all_files)}")
for f in all_files:
    print(f"  {f}")
print(f"Total: {result['total_files']} files, {result['total_words']} words")

# Check semantic cache
cached_nodes, cached_edges, cached_hyperedges, uncached = check_semantic_cache(all_files)
print(f"\nCache: {len(all_files) - len(uncached)} files hit, {len(uncached)} files need extraction")

if cached_nodes or cached_edges or cached_hyperedges:
    Path("graphify-out/.graphify_cached.json").write_text(
        json.dumps({"nodes": cached_nodes, "edges": cached_edges, "hyperedges": cached_hyperedges},
                   ensure_ascii=False), encoding="utf-8"
    )
Path("graphify-out/.graphify_uncached.txt").write_text("\n".join(uncached), encoding="utf-8")

if uncached:
    print("\nUncached files:")
    for f in uncached:
        print(f"  {f}")
