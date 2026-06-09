import json
from pathlib import Path

analysis = json.loads(Path("graphify-out/.graphify_analysis.json").read_text(encoding="utf-8"))
extraction = json.loads(Path("graphify-out/.graphify_extract.json").read_text(encoding="utf-8"))

# Build node-id -> label map
node_map = {n["id"]: n.get("label", n["id"]) for n in extraction["nodes"]}

communities = analysis["communities"]
print(f"Total communities: {len(communities)}")
print()

# Print each community with sample nodes and size
sorted_comms = sorted(communities.items(), key=lambda x: len(x[1]), reverse=True)
for cid_str, nodes in sorted_comms[:40]:
    sample = [node_map.get(n, n) for n in nodes[:8]]
    print(f"Community {cid_str} ({len(nodes)} nodes): {sample}")
