import json
from pathlib import Path

extract = json.loads(Path('graphify-out/.graphify_extract.json').read_text(encoding='utf-8'))
n_nodes = len(extract.get('nodes', []))
n_edges = len(extract.get('edges', []))
print(f'Extract: {n_nodes} nodes, {n_edges} edges')

incremental = json.loads(Path('graphify-out/.graphify_incremental.json').read_text(encoding='utf-8'))
nf = incremental.get('new_files', {})
total_new = sum(len(v) for v in nf.values())
code_exts = {'.cpp','.hpp','.h','.c','.cc','.cxx','.json','.ps1'}
all_changed = [f for lst in nf.values() for f in lst]
all_code = all(Path(f).suffix.lower() in code_exts for f in all_changed)
print(f'Total new/changed: {total_new}')
print(f'Code-only: {all_code}')
print(f'New code files: {len(nf.get("code",[]))}')
print(f'New doc files: {len(nf.get("document",[]))}')
print(f'New image files: {len(nf.get("image",[]))}')
