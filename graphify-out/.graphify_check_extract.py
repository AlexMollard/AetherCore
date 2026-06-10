import json
extract = json.load(open('graphify-out/.graphify_extract.json', 'r', encoding='utf-8'))
print(f'nodes: {len(extract.get("nodes", []))}, edges: {len(extract.get("edges", []))}, hyperedges: {len(extract.get("hyperedges", []))}')
if extract.get('nodes'):
    print(f'Sample node: {extract["nodes"][0]}')
if extract.get('edges'):
    print(f'Sample edge: {extract["edges"][0]}')
