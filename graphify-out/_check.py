import json
from pathlib import Path

ast = json.loads(Path('graphify-out/.graphify_ast.json').read_text(encoding='utf-8'))
print(f'AST: {len(ast.get("nodes", []))} nodes, {len(ast.get("edges", []))} edges')

sem_path = Path('graphify-out/.graphify_semantic.json')
if sem_path.exists():
    sem = json.loads(sem_path.read_text(encoding='utf-8'))
    print(f'Semantic: {len(sem.get("nodes", []))} nodes, {len(sem.get("edges", []))} edges')
else:
    print('Semantic: not available')

extract_path = Path('graphify-out/.graphify_extract.json')
if extract_path.exists():
    extract = json.loads(extract_path.read_text(encoding='utf-8'))
    print(f'Extract: {len(extract.get("nodes", []))} nodes, {len(extract.get("edges", []))} edges')
else:
    print('Extract file missing')
    # Check what files exist in graphify-out
    print('Files in graphify-out:')
    for f in Path('graphify-out').iterdir():
        print(f'  {f.name}')
