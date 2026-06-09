import sys, json
from graphify.build import build_from_json
from graphify.cluster import score_all
from graphify.analyze import god_nodes, surprising_connections, suggest_questions
from graphify.report import generate
from pathlib import Path

extraction = json.loads(Path("graphify-out/.graphify_extract.json").read_text(encoding="utf-8"))
detection  = json.loads(Path("graphify-out/.graphify_detect.json").read_text(encoding="utf-8"))
analysis   = json.loads(Path("graphify-out/.graphify_analysis.json").read_text(encoding="utf-8"))
labels_raw = json.loads(Path("graphify-out/.graphify_labels.json").read_text(encoding="utf-8"))

G = build_from_json(extraction)
communities = {int(k): v for k, v in analysis["communities"].items()}
cohesion = {int(k): v for k, v in analysis["cohesion"].items()}
tokens = {"input": extraction.get("input_tokens", 0), "output": extraction.get("output_tokens", 0)}
labels = {int(k): v for k, v in labels_raw.items()}

questions = suggest_questions(G, communities, labels)

report = generate(G, communities, cohesion, labels, analysis["gods"], analysis["surprises"], detection, tokens, ".", suggested_questions=questions)
Path("graphify-out/GRAPH_REPORT.md").write_text(report, encoding="utf-8")
print(f"Report updated: {len(labels)} communities labeled")
print(f"Graph: {G.number_of_nodes()} nodes, {G.number_of_edges()} edges")
