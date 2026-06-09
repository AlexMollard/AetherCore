import json
from pathlib import Path

analysis = json.loads(Path(".graphify_analysis.json").read_text(encoding="utf-8"))
extract = json.loads(Path(".graphify_extract.json").read_text(encoding="utf-8"))

node_map = {n["id"]: n.get("label", n["id"]) for n in extract["nodes"]}

# Show details for problematic communities
problem_ids = ["99", "138", "170", "328", "348", "369", "81", "139", "141", "160", "162", "171",
               "182", "209", "213", "218", "228", "233", "234", "248", "258", "264", "265",
               "273", "276", "277", "284", "285", "286", "291", "310", "311", "313", "314",
               "315", "321", "322", "326", "327", "335", "341", "351", "354", "364", "365",
               "366", "367", "368", "79", "92", "108", "128", "152", "154", "176", "177",
               "178", "188", "203", "223", "242", "267", "288", "289", "299", "300", "301",
               "302", "363"]

for cid in problem_ids:
    if cid in analysis["communities"]:
        nodes = analysis["communities"][cid]
        labels = [node_map.get(n, n) for n in nodes[:15]]
        print(f"--- Community {cid} ({len(nodes)} nodes) ---")
        print(f"  Sample IDs: {nodes[:8]}")
        print(f"  Sample labels: {labels}")
        print()
