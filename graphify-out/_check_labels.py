import json
from pathlib import Path

g = json.loads(Path("graphify-out/graph.json").read_text(encoding="utf-8"))
nodes = g.get("nodes", [])

# Check label quality
print(f"Total nodes: {len(nodes)}")
print()

# First 40 node labels
print("First 40 node IDs and labels:")
for n in nodes[:40]:
    print(f"  id={n.get('id','')[:65]:65s} label={str(n.get('label',''))[:65]}")

# Check if there's a mismatch - are labels the same as IDs?
mismatch = 0
same = 0
for n in nodes:
    nid = n.get("id", "")
    lbl = str(n.get("label", ""))
    if nid == lbl:
        same += 1
    else:
        mismatch += 1

print(f"\nLabels match IDs: {same}")
print(f"Labels differ from IDs: {mismatch}")

# Check community labels
lbl_path = Path("graphify-out/.graphify_labels.json")
if lbl_path.exists():
    labels = json.loads(lbl_path.read_text(encoding="utf-8"))
    print(f"\nCommunity labels file: {len(labels)} entries")
    # Show first 20
    for cid in sorted(labels.keys())[:20]:
        print(f"  Community {cid}: {labels[cid]}")
