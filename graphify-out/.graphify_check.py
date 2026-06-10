import json
d = json.load(open('graphify-out/graph.json', 'r', encoding='utf-8'))
nodes = d.get('nodes', [])
edges = d.get('edges', [])
communities = d.get('communities', {})
print(f'nodes: {len(nodes)}, edges: {len(edges)}, communities: {len(communities)}')
