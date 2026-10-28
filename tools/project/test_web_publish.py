import importlib.util
from pathlib import Path
import tempfile
import unittest
SPEC=importlib.util.spec_from_file_location('web_publish',Path(__file__).with_name('web_publish.py'))
class WebPreflightTests(unittest.TestCase):
 def setUp(self):
  self.assertTrue(Path(SPEC.origin).is_file(),'target-aware web publish service is missing')
  self.api=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(self.api)
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
  (self.root/'scripts/src').mkdir(parents=True);(self.root/'scenes').mkdir();(self.root/'assets').mkdir()
  self.settings='[project]\nname="Test"\nkind="2d"\n[scripting]\nlanguage="rust"\n[app]\nstartupscene="Start"\n'
  (self.root/'ProjectSettings.toml').write_text(self.settings,encoding='utf-8')
  (self.root/'scripts/Cargo.toml').write_text('[package]\nname="test_game"\nversion="0.1.0"\nedition="2024"\n[lib]\ncrate-type=["cdylib"]\n[dependencies]\n',encoding='utf-8')
  (self.root/'scripts/src/lib.rs').write_text('export_gameplay!(Player);',encoding='utf-8')
  (self.root/'scenes/Start.scene.toml').write_text('[scene]\nversion=16\nkind="2d"\n[[entities]]\nname="Camera"\n[entities.camera]\nprojection="orthographic"\nmain=true\n',encoding='utf-8')
 def expect(self,code):
  with self.assertRaises(self.api.PreflightError) as error:self.api.preflight(self.root)
  self.assertEqual(error.exception.code,code)
 def test_default_scene_and_target_profile(self):
  result=self.api.preflight(self.root);self.assertEqual(result['profile'],'webgpu-2d-v1');self.assertIn('Start',result['scenes'])
 def test_csharp_selects_managed_project(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"/>',encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertEqual(result['language'],'csharp');self.assertEqual(result['managed_project'].name,'AetherGame.csproj')
 def test_project_root_scripts_ignore_generated_native_build_caches(self):
  for language,extension,generated,bad,code in [('rust','rs','use std::net::TcpStream;','use std::net::TcpStream;','WEB_NETWORK_UNSUPPORTED'),('csharp','cs','[DllImport("generated")]','NativeLibrary.Load("real");','WEB_NATIVE_API_UNSUPPORTED')]:
   with self.subTest(language=language):
    (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust',language)+'[paths]\nscripts="."\n',encoding='utf-8')
    if language=='rust':
     (self.root/'Cargo.toml').write_bytes((self.root/'scripts/Cargo.toml').read_bytes())
    else:
     (self.root/'AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
    cache=self.root/'Builds/Intermediate/generated'
    cache.mkdir(parents=True,exist_ok=True)
    (cache/('generated.'+extension)).write_text(generated,encoding='utf-8')
    self.assertEqual(self.api.preflight(self.root)['language'],language)
    source=self.root/('real.'+extension)
    source.write_text(bad,encoding='utf-8');self.expect(code);source.unlink()
 def test_csharp_requires_project_and_rejects_native_imports(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8');self.expect('WEB_MANAGED_PROJECT')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  (self.root/'scripts/Bad.cs').write_text('[DllImport("external")] static extern void Native();',encoding='utf-8');self.expect('WEB_NATIVE_API_UNSUPPORTED')
 def test_csharp_reachable_scenes_use_existing_scene_contract(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  (self.root/'scripts/Game.cs').write_text('Scene.Load("Results");',encoding='utf-8');self.expect('WEB_SCENE_MISSING')
 def test_missing_dynamic_scene(self):
  (self.root/'scripts/src/lib.rs').write_text('ctx.load_scene("Missing")',encoding='utf-8');self.expect('WEB_SCENE_MISSING')
 def test_dynamic_scene_needs_declaration(self):
  (self.root/'scripts/src/lib.rs').write_text('ctx.load_scene(variable)',encoding='utf-8');self.expect('WEB_DYNAMIC_SCENE_UNDECLARED')
 def test_rust_networking_fails(self):
  (self.root/'scripts/src/lib.rs').write_text('ctx.network_connect("host")',encoding='utf-8');self.expect('WEB_NETWORK_UNSUPPORTED')
 def test_3d_and_shader_fail(self):
  p=self.root/'scenes/Start.scene.toml';p.write_text(p.read_text()+'[entities.mesh]\npath="x.glb"\n',encoding='utf-8');self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_material_and_unsupported_profile_fail_specifically(self):
  p=self.root/'scenes/Start.scene.toml';original=p.read_text(encoding='utf-8');p.write_text(original+'[entities.material]\nshader="custom"\n',encoding='utf-8');self.expect('WEB_SHADER_UNSUPPORTED')
  p.write_text(original,encoding='utf-8');(self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d"\n',encoding='utf-8');self.expect('WEB_PROFILE_UNSUPPORTED')
 def test_dynamic_audio_requires_additional_assets(self):
  (self.root/'scripts/src/lib.rs').write_text('ctx.play_audio(path)',encoding='utf-8');self.expect('WEB_DYNAMIC_ASSET_UNDECLARED')
 def test_additional_assets_cannot_escape(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nadditional_assets=["../secret"]\n',encoding='utf-8');self.expect('WEB_ASSET_PATH')
 def test_failed_preflight_uses_the_agent_report_schema(self):
  import gameplay_jobs
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  result=self.api.publish(dict(project=str(self.root),language='csharp',jobId='web-rejected'),self.root/'report')
  self.assertEqual(result['status'],'failed');self.assertEqual(result['target'],'web');self.assertEqual(result['language'],'csharp')
  gameplay_jobs.validate_report(result,result['jobId'],1)
 def test_web_coverage_report_schema(self):
  import copy
  import gameplay_jobs
  report=gameplay_jobs._report(dict(jobId='web-coverage',target='web',language='rust'),'passed','commit')
  report['coverage']={
   'featuresUsed':['effects','lighting_3d'],
   'authoredGraphicsSettings':{'bloomstrength':0.035,'contactshadows':True,'renderscale':1},
   'unqualified':[{'feature':'effects','status':'bounded','ledger':'docs/ledger#effects','unqualified':'Outside recorded cases'}],
   'unqualifiedSettings':[{'setting':'graphics.bloomstrength','value':0.035,'ledger':'docs/ledger#effects','qualification':'Not individually qualified'},
                          {'setting':'graphics.contactshadows','value':True,'ledger':'docs/ledger#effects','qualification':'Not individually qualified'}]}
  gameplay_jobs.validate_report(report,'web-coverage',0)
  for mutate in (
   lambda c:c.update(extra=True),
   lambda c:c['featuresUsed'].append(1),
   lambda c:c['authoredGraphicsSettings'].update(bloomstrength=[]),
   lambda c:c['authoredGraphicsSettings'].update(bloomstrength=float('nan')),
   lambda c:c['unqualified'][0].update(extra=True),
   lambda c:c['unqualified'][0].pop('ledger'),
   lambda c:c['unqualifiedSettings'][0].update(value=None),
   lambda c:c['unqualifiedSettings'][0].update(value=float('inf')),
   lambda c:c['unqualifiedSettings'][0].update(extra=True),
  ):
   invalid=copy.deepcopy(report);mutate(invalid['coverage'])
   with self.subTest(coverage=invalid['coverage']):
    with self.assertRaises(ValueError):gameplay_jobs.validate_report(invalid,'web-coverage',0)
 def test_shader_additional_asset_fails(self):
  (self.root/'assets/test.slang').write_text('shader',encoding='utf-8');(self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nadditional_assets=["project://assets/test.slang"]\n',encoding='utf-8');self.expect('WEB_SHADER_UNSUPPORTED')
 def test_custom_pass_shader_including_portable_contract_still_fails(self):
  (self.root/'assets/shaders').mkdir()
  (self.root/'assets/shaders/ink.slang').write_text('#include "include/CustomPass.slangh"\n[shader("vertex")] float4 vertexMain(uint i : SV_VertexID) : SV_Position { return CustomPassParams(); }',encoding='utf-8')
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nadditional_assets=["project://assets/shaders/ink.slang"]\n',encoding='utf-8');self.expect('WEB_SHADER_UNSUPPORTED')
 def test_tilemap_dependency_is_length_prefixed(self):
  import struct
  uri=b'project://assets/test.tileset.toml'
  (self.root/'assets/test.tileset.toml').write_text('[tileset]\nname="Test"\n',encoding='utf-8')
  (self.root/'assets/test.tilemap').write_bytes(b'ATLM'+struct.pack('<II',1,len(uri))+uri+struct.pack('<fII',.3,0,0))
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nadditional_assets=["project://assets/test.tilemap"]\n',encoding='utf-8')
  self.assertIn((self.root/'assets/test.tileset.toml').resolve(),self.api.preflight(self.root)['files'])
 def test_missing_scene_kind_matches_native_3d_default(self):
  p=self.root/'scenes/Start.scene.toml';p.write_text(p.read_text(encoding='utf-8').replace('kind="2d"\n',''),encoding='utf-8')
  self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_present_camera_defaults_to_native_perspective(self):
  p=self.root/'scenes/Start.scene.toml';p.write_text(p.read_text(encoding='utf-8').replace('projection="orthographic"\n',''),encoding='utf-8')
  self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_headerless_2d_prefab_remains_supported(self):
  (self.root/'assets/prefabs').mkdir()
  prefab=self.root/'assets/prefabs/Coin.prefab.toml'
  prefab.write_text('[[entities]]\nname="Coin"\n[entities.sprite]\n',encoding='utf-8')
  (self.root/'scripts/src/lib.rs').write_text('ctx.instantiate_prefab("Coin")',encoding='utf-8')
  self.assertIn(prefab.resolve(),self.api.preflight(self.root)['files'])
 def test_new_project_camel_case_settings_choose_authored_startup(self):
  (self.root/'scenes/Start.scene.toml').rename(self.root/'scenes/Launch.scene.toml')
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('startupscene="Start"','startupScene="Launch"'),encoding='utf-8')
  self.assertEqual(self.api.preflight(self.root)['startup'],'Launch')
 def test_3d_profile_follows_scene_and_prefab_links(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\n',encoding='utf-8')
  (self.root/'scenes/Start.scene.toml').write_text('[scene]\nversion=16\nkind="3d"\n[[includes]]\nscene="Other"\n[[prefab_instances]]\nprefab="Prop"\n',encoding='utf-8')
  (self.root/'scenes/Other.scene.toml').write_text('[scene]\nversion=16\nkind="3d"\n',encoding='utf-8')
  (self.root/'assets/prefabs').mkdir()
  (self.root/'assets/prefabs/Prop.prefab.toml').write_text('[[entities]]\n[entities.mesh]\nkind="model"\npath="project://assets/model.gltf"\n',encoding='utf-8')
  (self.root/'assets/model.gltf').write_text('{"buffers":[{"uri":"model.bin"}]}',encoding='utf-8')
  (self.root/'assets/model.bin').write_bytes(b'model')
  result=self.api.preflight(self.root)
  self.assertEqual(result['profile'],'webgpu-3d-v1')
  self.assertTrue({(self.root/'scenes/Other.scene.toml').resolve(),(self.root/'assets/prefabs/Prop.prefab.toml').resolve(),(self.root/'assets/model.bin').resolve()}.issubset(result['files']))
  (self.root/'assets/model.bin').unlink();self.expect('WEB_ASSET_MISSING')
 def test_3d_shader_portability(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\nadditional_assets=["project://assets/pass.slang"]\n',encoding='utf-8')
  shader=self.root/'assets/pass.slang';shader.write_text('not portable',encoding='utf-8');self.expect('WEB_CUSTOM_PASS_PORTABILITY')
  shader.write_text('#include "include/CustomPass.slangh"\n',encoding='utf-8');self.assertIn(shader.resolve(),self.api.preflight(self.root)['files'])
 def test_3d_desktop_animation_is_rejected(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp')+'[publish.web]\nprofile="webgpu-3d-v1"\n',encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  for expression in ('Animation.Add(1);', 'Animation.LoadExternal("x");', 'Animation.Compile();', 'Animation.SetJointOffset(1);', 'Physics.DebugEnabled = true;'):
   (self.root/'scripts/Game.cs').write_text(expression,encoding='utf-8');self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_3d_rejects_spirv_and_unknown_component(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\nadditional_assets=["project://assets/raw.spv"]\n',encoding='utf-8')
  (self.root/'assets/raw.spv').write_bytes(b'spirv');self.expect('WEB_SHADER_UNSUPPORTED')
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\n',encoding='utf-8')
  p=self.root/'scenes/Start.scene.toml';p.write_text(p.read_text()+'[entities.unknown_component]\nvalue=1\n',encoding='utf-8')
  self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_3d_material_texture_closure(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\nadditional_assets=["project://assets/test.material.toml"]\n',encoding='utf-8')
  (self.root/'assets/test.material.toml').write_text('[material]\nshader="shaders://gltf_mesh.spv"\n[textures]\nalbedo="project://assets/albedo.png"\n',encoding='utf-8')
  (self.root/'assets/albedo.png').write_bytes(b'png')
  self.assertIn((self.root/'assets/albedo.png').resolve(),self.api.preflight(self.root)['files'])
  (self.root/'assets/albedo.png').unlink();self.expect('WEB_ASSET_MISSING')
 def test_browser_reference_scene_component_tables_are_supported(self):
  import tomllib
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d-v1"\n',encoding='utf-8')
  fixtures=Path(SPEC.origin).resolve().parents[2]/'resources/web/reference/scenes'
  scenes=sorted(path for path in fixtures.glob('Browser*.scene.toml') if not path.name.startswith('BrowserBad'))
  self.assertTrue(scenes)
  for scene in scenes:
   with self.subTest(scene=scene.name):
    document=tomllib.loads(scene.read_text(encoding='utf-8-sig'))
    keys={key for entity in document.get('entities',[]) for key,value in entity.items()
          if isinstance(value,dict) or isinstance(value,list) and value and isinstance(value[0],dict)}
    # Keep each actual table key while removing unrelated asset references and values.
    text='[scene]\nversion=16\nkind="3d"\n'
    for key in sorted(keys):
     text+='[[entities]]\n[entities.'+key+']\n'
     if key=='effect':text+='name="plasma"\n'
    (self.root/'scenes/Start.scene.toml').write_text(text,encoding='utf-8')
    self.api.preflight(self.root)
if __name__=='__main__':unittest.main()
