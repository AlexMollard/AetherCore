"""Extra scene/property preservation checks for the trimmed C# reference export."""
from reference_fixture import ROOT, assets


def write_fixture(project, revision=1):
    for relative, content in assets().items():
        path = project / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    source = (ROOT / 'projects/AgentPipelineCSharp/scripts/Reference.cs').read_text(encoding='utf-8')
    source = source.replace('public sealed class ReferencePlayer : EntityScript',
        'public enum MetadataChoice { First, Second }\n\npublic sealed class ReferencePlayer : EntityScript')
    source = source.replace('    public int ticks;', '''    public float amount;
    public Vector3 direction;
    public string caption = "";
    public MetadataChoice choice;
    public CameraRef linked;
    public Entity target;
    public override void OnAttach()
    {
        if (amount != REVISION.25f || direction != new Vector3(1, 2, 3) || caption != "café")
            throw new InvalidOperationException("Trimmed scalar/vector/string property mismatch.");
        if (choice != MetadataChoice.Second || !linked.Owner.IsValid || linked.Owner != target)
            throw new InvalidOperationException("Trimmed enum/component/entity property mismatch.");
    }
    public int ticks;'''.replace('REVISION', str(revision)), 1)
    (project / 'scripts/Reference.cs').write_text(source, encoding='utf-8', newline='\r\n')
    properties = f'''[entities.scripts.properties.amount]
t = 'float'
v = {revision}.25
[entities.scripts.properties.direction]
t = 'vec3'
v = [1.0, 2.0, 3.0]
[entities.scripts.properties.caption]
t = 'string'
v = 'café'
[entities.scripts.properties.choice]
t = 'enum'
v = 1
[entities.scripts.properties.linked]
t = 'component'
c = 'Camera'
v = 1
[entities.scripts.properties.target]
t = 'entity'
v = 1
'''
    scene = project / 'scenes/ReferenceStart.scene.toml'
    scene.write_text(scene.read_text(encoding='utf-8').replace("type = 'ReferencePlayer'\n",
        "type = 'ReferencePlayer'\n" + properties), encoding='utf-8', newline='\r\n')
