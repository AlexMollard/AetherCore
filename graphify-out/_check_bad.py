import json
from pathlib import Path

g = json.loads(Path("graphify-out/graph.json").read_text(encoding="utf-8"))

# Search for bad labels
bad = [n for n in g["nodes"] if "render subsystem" in str(n.get("label", "")).lower()]
print(f'Nodes with "render subsystem" in label: {len(bad)}')
for n in bad[:10]:
    print(f'  id={n.get("id","")} label={n.get("label","")}')

if not bad:
    # Show sample of nodes from render-related communities
    labels = json.loads(Path("graphify-out/.graphify_labels.json").read_text(encoding="utf-8"))
    node_map = {n["id"]: n.get("label", n["id"]) for n in g["nodes"]}
    
    # Show community 1 (Render Queue)
    print("\nCommunity 1 (Render Queue) sample nodes:")
    comm1_nodes = [n for n in g["nodes"] if n.get("community") == 1][:10]
    for n in comm1_nodes:
        print(f'  label={n.get("label","")}')

    print("\nCommunity 0 (UI System Core) sample nodes:")
    comm0_nodes = [n for n in g["nodes"] if n.get("community") == 0][:10]
    for n in comm0_nodes:
        print(f'  label={n.get("label","")}')
