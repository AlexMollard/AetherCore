import json, glob
from pathlib import Path

# Step B3: Collect, cache, and merge semantic chunks
chunks = sorted(glob.glob("graphify-out/.graphify_chunk_*.json"))
print(f"Found {len(chunks)} chunks")

all_nodes, all_edges, all_hyperedges = [], [], []
total_in, total_out = 0, 0
for c in chunks:
    d = json.loads(Path(c).read_text(encoding="utf-8"))
    nodes = d.get("nodes", [])
    edges = d.get("edges", [])
    hyps = d.get("hyperedges", [])
    all_nodes += nodes
    all_edges += edges
    all_hyperedges += hyps
    total_in += d.get("input_tokens", 0)
    total_out += d.get("output_tokens", 0)
    print(f"  {c}: {len(nodes)} nodes, {len(edges)} edges, {len(hyps)} hyperedges")

# Write merged semantic
sem_new = {
    "nodes": all_nodes, "edges": all_edges, "hyperedges": all_hyperedges,
    "input_tokens": total_in, "output_tokens": total_out,
}
Path("graphify-out/.graphify_semantic_new.json").write_text(
    json.dumps(sem_new, indent=2, ensure_ascii=False), encoding="utf-8"
)
print(f"\nSemantic merged: {len(all_nodes)} nodes, {len(all_edges)} edges")

# Save to cache
from graphify.cache import save_semantic_cache
saved = save_semantic_cache(all_nodes, all_edges, all_hyperedges)
print(f"Cached: {saved} files")

# Merge cached + new into .graphify_semantic.json
cached = {}
cached_path = Path("graphify-out/.graphify_cached.json")
if cached_path.exists():
    cached = json.loads(cached_path.read_text(encoding="utf-8"))

all_sem_nodes = cached.get("nodes", []) + all_nodes
all_sem_edges = cached.get("edges", []) + all_edges
all_sem_hyps = cached.get("hyperedges", []) + all_hyperedges

# Dedup nodes by id
seen = set()
deduped = []
for n in all_sem_nodes:
    if n["id"] not in seen:
        seen.add(n["id"])
        deduped.append(n)

sem_final = {
    "nodes": deduped, "edges": all_sem_edges, "hyperedges": all_sem_hyps,
    "input_tokens": total_in, "output_tokens": total_out,
}
Path("graphify-out/.graphify_semantic.json").write_text(
    json.dumps(sem_final, indent=2, ensure_ascii=False), encoding="utf-8"
)
print(f"Semantic final: {len(deduped)} nodes, {len(all_sem_edges)} edges")

# Part C: Merge AST + semantic
ast = json.loads(Path("graphify-out/.graphify_ast.json").read_text(encoding="utf-8"))
print(f"\nAST: {len(ast['nodes'])} nodes, {len(ast['edges'])} edges")

seen = {n["id"] for n in ast["nodes"]}
merged_nodes = list(ast["nodes"])
for n in deduped:
    if n["id"] not in seen:
        merged_nodes.append(n)
        seen.add(n["id"])

merged_edges = ast["edges"] + all_sem_edges
merged_hyps = all_sem_hyps
merged = {
    "nodes": merged_nodes, "edges": merged_edges, "hyperedges": merged_hyps,
    "input_tokens": total_in, "output_tokens": total_out,
}
Path("graphify-out/.graphify_extract.json").write_text(
    json.dumps(merged, indent=2, ensure_ascii=False), encoding="utf-8"
)
print(f"Merged: {len(merged_nodes)} nodes, {len(merged_edges)} edges ({len(ast['nodes'])} AST + {len(deduped)} semantic)")

# Cleanup temp files
for c in chunks:
    Path(c).unlink()
for tmp in [".graphify_cached.json", ".graphify_uncached.txt", ".graphify_semantic_new.json"]:
    p = Path(f"graphify-out/{tmp}")
    if p.exists():
        p.unlink()
