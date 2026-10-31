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
 def test_distribution_notices_copy_exact_bytes_and_reject_missing_input(self):
  build=self.root/'build';stage=self.root/'stage';stage.mkdir()
  (build/'data/notices/runtime').mkdir(parents=True)
  notice=build/'data/THIRD_PARTY_NOTICES.txt';notice.write_bytes(b'exact dependency notice\r\n')
  terms=self.root/'DISTRIBUTION.md';terms.write_bytes(b'private distribution terms\n')
  (build/'data/notices/runtime/LICENSE.txt').write_bytes(b'exact runtime license')
  self.api.stage_distribution_notices(build,stage,terms)
  self.assertEqual((stage/'THIRD_PARTY_NOTICES.txt').read_bytes(),notice.read_bytes())
  self.assertEqual((stage/'notices/runtime/LICENSE.txt').read_bytes(),b'exact runtime license')
  notice.unlink()
  with self.assertRaises(self.api.PreflightError) as error:self.api.stage_distribution_notices(build,stage,terms)
  self.assertEqual(error.exception.code,'WEB_NOTICES_MISSING')
 def test_dynamic_font_families_and_existing_curves_are_packaged(self):
  fonts=self.root/'assets/fonts';fonts.mkdir()
  source=fonts/'Custom-Regular.ttf';source.write_bytes(b'font')
  curves=fonts/'Custom-Regular.fontcurves';curves.write_bytes(b'curves')
  data=self.api.preflight(self.root)
  self.assertIn(source.resolve(),data['bootstrap_files'])
  self.assertIn(curves.resolve(),data['bootstrap_files'])
  self.assertIn(curves.resolve(),data['dependencies'][source.resolve()])
 def test_custom_font_and_unicode_scene_are_reachable(self):
  fonts=self.root/'assets/fonts';fonts.mkdir()
  source=fonts/'Custom-Regular.ttf';source.write_bytes(b'font')
  scene=self.root/'scenes/Start.scene.toml'
  scene.write_text(scene.read_text(encoding='utf-8')+'[[entities]]\nname="Label"\n[entities.ui_text]\nfont="Custom"\ntext="Café  Καλημέρα"\n',encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertIn(source.resolve(),result['files'])
 def test_csharp_selects_managed_project(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"/>',encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertEqual(result['language'],'csharp');self.assertEqual(result['managed_project'].name,'AetherGame.csproj')
 def test_asset_queries_and_write_destinations_are_not_read_dependencies(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  used=self.root/'assets/read.txt';used.write_text('read',encoding='utf-8')
  unused=self.root/'assets/unused.txt';unused.write_text('not declared',encoding='utf-8')
  source=self.root/'scripts/Game.cs'
  code='Assets.List("project://assets/*.txt"); Assets.List("project://../*"); Assets.WriteText("project://assets/new.txt","x"); Assets.ReadText("project://assets/read.txt");'
  source.write_text(code,encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertIn(used.resolve(),result['files'])
  self.assertNotIn(unused.resolve(),result['files'])
  source.write_text(code+' Assets.ReadText("project://assets/*.txt");',encoding='utf-8')
  self.expect('WEB_ASSET_MISSING')
 def test_ui_shader_script_literals_and_dynamic_declarations(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  shaders=self.root/'assets/shaders';shaders.mkdir()
  material=shaders/'ink.slang';material.write_text('#include "include/UiVertexOut.slangh"\n',encoding='utf-8')
  effect=shaders/'fade.slang';effect.write_text('[shader("vertex")] void vertexMain() {}',encoding='utf-8')
  code=self.root/'scripts/Game.cs'
  code.write_text('Ui.SetMaterial(button, "ink"); Ui.CreateEffect(canvas, "fade");',encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertEqual(result['ui_shader_sources'][material.resolve()],{'material'})
  self.assertEqual(result['ui_shader_sources'][effect.resolve()],{'effect'})
  code.write_text('Ui.SetMaterial(button, shader);',encoding='utf-8')
  self.expect('WEB_DYNAMIC_ASSET_UNDECLARED')
  settings=self.settings.replace('rust','csharp')+'[publish.web]\nadditional_assets=["project://assets/shaders/ink.slang"]\n'
  (self.root/'ProjectSettings.toml').write_text(settings,encoding='utf-8')
  self.assertEqual(self.api.preflight(self.root)['ui_shader_sources'][material.resolve()],{'material'})
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
 def test_network_config_is_validated_and_preserved(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web.network]\ngatewayUrl="wss://example.org/net"\ntargetAlias="arena"\nauthToken="public-token"\n',encoding='utf-8')
  self.assertEqual(self.api.preflight(self.root)['network'],{'gatewayUrl':'wss://example.org/net','targetAlias':'arena','authToken':'public-token'})
  for gateway in ('http://example.org/net','wss://user:pass@example.org/net','wss://example.org/other','wss://example.org/net?token=secret','wss://example.org:bad/net'):
   with self.subTest(gateway=gateway):
    (self.root/'ProjectSettings.toml').write_text(self.settings+f'[publish.web.network]\ngatewayUrl="{gateway}"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
    self.expect('WEB_NETWORK_CONFIG')
  for extra in ('targetAlias=""\nauthToken="token"','targetAlias="arena"\nauthToken=42','targetAlias="arena"','targetAlias="arena"\nauthToken=""'):
   with self.subTest(extra=extra):
    (self.root/'ProjectSettings.toml').write_text(self.settings+f'[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\n{extra}\n',encoding='utf-8')
    self.expect('WEB_NETWORK_CONFIG')
 def test_network_capability_requires_config(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\ncapabilities=["network"]\n',encoding='utf-8')
  self.expect('WEB_NETWORK_CONFIG')
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\ncapabilities=["network"]\n[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
  (self.root/'scripts/src/lib.rs').write_text('ctx.net_connect("room:arena", 0); ctx.net_is_connected();',encoding='utf-8')
  (self.root/'ProjectSettings.toml').write_text(self.settings,encoding='utf-8');self.expect('WEB_NETWORK_CONFIG')
  (self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\ncapabilities=["network"]\n[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
  self.assertEqual(self.api.preflight(self.root)['network']['targetAlias'],'arena')
 def test_network_audit_ignores_comments_and_literals(self):
  (self.root/'scripts/src/lib.rs').write_text('// std::net::TcpStream\nlet label = "network_connect"; let network_score = 1;',encoding='utf-8')
  self.api.preflight(self.root)
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  (self.root/'scripts/Game.cs').write_text('// Net.Connect and EditorGui\nvar label = "System.Net.Sockets"; var address = System.Net.IPAddress.Loopback;',encoding='utf-8')
  self.api.preflight(self.root)
 def test_csharp_network_and_editor_apis_are_rejected(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  for source,code in [('Net.ConfigureRelay("host", 1, "u", "p");','WEB_NETWORK_UNSUPPORTED'),('using System.Net.Sockets;','WEB_NETWORK_UNSUPPORTED'),('new TcpClient();','WEB_NETWORK_UNSUPPORTED'),('EditorGui.Begin();','WEB_CAPABILITY_UNSUPPORTED'),('class MyWindow : IEditorWindow {}','WEB_CAPABILITY_UNSUPPORTED')]:
   with self.subTest(source=source):
    (self.root/'scripts/Game.cs').write_text(source,encoding='utf-8');self.expect(code)
  (self.root/'scripts/Game.cs').write_text('[NetRpc(NetRpcTarget.Server)] void Send() { Net.CallServer(Self, "Send"); } Net . Connect("host", 1);',encoding='utf-8')
  self.expect('WEB_NETWORK_CONFIG')
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp')+'[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
  self.api.preflight(self.root)
 def test_csharp_predicted_spawn_requires_gateway_and_is_supported(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  (self.root/'scripts/Game.cs').write_text('Net.SpawnPredicted("Ghost", position, 99); Net.TryTakePredictedSpawn<int>(out var payload);',encoding='utf-8')
  self.expect('WEB_NETWORK_CONFIG')
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp')+'[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
  self.expect('WEB_ASSET_MISSING')
  prefabs=self.root/'assets/prefabs';prefabs.mkdir()
  ghost=prefabs/'Ghost.prefab.toml';ghost.write_text('[scene]\nversion=16\nkind="2d"\n[[entities]]\nname="Ghost"\n',encoding='utf-8')
  self.assertIn(ghost.resolve(),self.api.preflight(self.root)['files'])
 def test_network_scene_components_require_config_in_both_profiles(self):
  scene=self.root/'scenes/Start.scene.toml'
  scene.write_text(scene.read_text(encoding='utf-8')+'[entities.network_identity]\n[entities.network_transform]\n[entities.net_player]\n',encoding='utf-8')
  self.expect('WEB_NETWORK_CONFIG')
  for profile in ('webgpu-2d-v1','webgpu-3d-v1'):
   with self.subTest(profile=profile):
    (self.root/'ProjectSettings.toml').write_text(self.settings+f'[publish.web]\nprofile="{profile}"\n[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n',encoding='utf-8')
    self.api.preflight(self.root)
 def test_network_spawn_packages_local_dynamic_and_remote_prefabs(self):
  prefabs=self.root/'assets/prefabs';prefabs.mkdir()
  for name in ('Local','Remote'):
   (prefabs/(name+'.prefab.toml')).write_text('[scene]\nversion=16\nkind="2d"\n[[entities]]\nname="Player"\n[[entities.scripts]]\ntype="Player"\n',encoding='utf-8')
  network='[publish.web.network]\ngatewayUrl="ws://localhost:8080/net"\ntargetAlias="arena"\nauthToken="token"\n'
  (self.root/'ProjectSettings.toml').write_text(self.settings+network,encoding='utf-8')
  for source in ('ctx.net_spawn("Local", position, 0);','ctx.net_spawn(name, position, 0);'):
   with self.subTest(source=source):
    (self.root/'scripts/src/lib.rs').write_text(source,encoding='utf-8')
    files=self.api.preflight(self.root)['files']
    self.assertIn((prefabs/'Local.prefab.toml').resolve(),files)
    self.assertIn((prefabs/'Remote.prefab.toml').resolve(),files)
  (self.root/'scripts/src/lib.rs').write_text('ctx.net_spawn("Missing", position, 0);',encoding='utf-8')
  self.expect('WEB_ASSET_MISSING')
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp')+network,encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  (self.root/'scripts/Game.cs').write_text('Net.Spawn("Local", position);',encoding='utf-8')
  self.assertIn((prefabs/'Remote.prefab.toml').resolve(),self.api.preflight(self.root)['files'])
 def test_3d_and_shader_fail(self):
  p=self.root/'scenes/Start.scene.toml';p.write_text(p.read_text()+'[entities.mesh]\npath="x.glb"\n',encoding='utf-8');self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_material_and_unsupported_profile_fail_specifically(self):
  p=self.root/'scenes/Start.scene.toml';original=p.read_text(encoding='utf-8');p.write_text(original+'[entities.material]\nshader="custom"\n',encoding='utf-8');self.expect('WEB_SHADER_UNSUPPORTED')
  p.write_text(original,encoding='utf-8');(self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nprofile="webgpu-3d"\n',encoding='utf-8');self.expect('WEB_PROFILE_UNSUPPORTED')
 def test_dynamic_audio_requires_additional_assets(self):
  (self.root/'scripts/src/lib.rs').write_text('ctx.play_audio(path)',encoding='utf-8');self.expect('WEB_DYNAMIC_ASSET_UNDECLARED')
 def test_dynamic_managed_audio_requires_additional_assets(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  for method in ('Play','PlayAt','PlayMusic'):
   with self.subTest(method=method):
    (self.root/'scripts/Game.cs').write_text(f'Audio.{method}(path);',encoding='utf-8')
    self.expect('WEB_DYNAMIC_ASSET_UNDECLARED')
 def test_managed_audio_entity_overload_and_authored_source_reach_clip(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8')
  (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
  clip=self.root/'assets/sound.wav';clip.write_bytes(b'fixture')
  (self.root/'scripts/Game.cs').write_text('Audio.PlayAt(entity, "project://assets/sound.wav");',encoding='utf-8')
  scene=self.root/'scenes/Start.scene.toml'
  scene.write_text(scene.read_text(encoding='utf-8')+'[entities.audio_source]\nclipPath="project://assets/sound.wav"\n',encoding='utf-8')
  self.assertIn(clip.resolve(),self.api.preflight(self.root)['files'])
  (self.root/'scripts/Game.cs').write_text('Audio.PlayAt(entity, path);',encoding='utf-8')
  self.expect('WEB_DYNAMIC_ASSET_UNDECLARED')
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
 def test_startup_uid_resolves_catalog_and_stages_it(self):
  uid='0123456789abcdef0123456789abcdef'
  (self.root/'scenes/Start.scene.toml').rename(self.root/'scenes/Renamed.scene.toml')
  catalog=self.root/'asset-catalog.toml'
  catalog.write_text('version=1\n[[assets]]\nuid="'+uid+'"\npath="scenes/Renamed.scene.toml"\n',encoding='utf-8')
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('startupscene="Start"','startupscene="uid://'+uid+'"'),encoding='utf-8')
  result=self.api.preflight(self.root)
  self.assertEqual(result['startup'],'uid://'+uid)
  self.assertEqual(result['startup_path'],(self.root/'scenes/Renamed.scene.toml').resolve())
  self.assertIn(catalog.resolve(),result['files'])
 def test_missing_startup_uid_fails(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('startupscene="Start"','startupscene="uid://0123456789abcdef0123456789abcdef"'),encoding='utf-8')
  self.expect('WEB_ASSET_MISSING')
 def test_scene_uid_asset_is_reachable(self):
  uid='0123456789abcdef0123456789abcdef'
  (self.root/'asset-catalog.toml').write_text('version=1\n[[assets]]\nuid="'+uid+'"\npath="assets/atlas.toml"\n',encoding='utf-8')
  asset=self.root/'assets/atlas.toml';asset.write_text('[atlas]\n',encoding='utf-8')
  scene=self.root/'scenes/Start.scene.toml';scene.write_text(scene.read_text(encoding='utf-8')+'[entities.sprite]\natlas="uid://'+uid+'"\n',encoding='utf-8')
  self.assertIn(asset.resolve(),self.api.preflight(self.root)['files'])
 def test_upgraded_scene_schema_and_legacy_versions_remain_publishable(self):
  scene=self.root/'scenes/Start.scene.toml'
  original=scene.read_text(encoding='utf-8')
  for version in (1,16,17,18):
   with self.subTest(version=version):
    scene.write_text(original.replace('version=16','version='+str(version)),encoding='utf-8')
    self.assertEqual(self.api.preflight(self.root)['startup'],'Start')
  for version in ('19','0','true','1.5','"18"'):
   with self.subTest(version=version):
    scene.write_text(original.replace('version=16','version='+version),encoding='utf-8')
    self.expect('WEB_FEATURE_UNSUPPORTED')
 def test_script_prefab_references_follow_moved_asset(self):
  uid='0123456789abcdef0123456789abcdef'
  (self.root/'assets/moved').mkdir()
  prefab=self.root/'assets/moved/Coin.prefab.toml'
  prefab.write_text('[[entities]]\nname="Coin"\n[entities.sprite]\n',encoding='utf-8')
  (self.root/'asset-catalog.toml').write_text('version=1\n[[assets]]\nuid="'+uid+'"\npath="assets/moved/Coin.prefab.toml"\n',encoding='utf-8')
  for language,source,call in (('rust','src/lib.rs','ctx.instantiate_prefab'),('csharp','Main.cs','Scene.Instantiate')):
   (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust',language),encoding='utf-8')
   (self.root/'scripts/AetherGame.csproj').write_text('<Project/>',encoding='utf-8')
   for reference in ('uid://'+uid,'project://assets/moved/Coin.prefab.toml'):
    with self.subTest(language=language,reference=reference):
     (self.root/'scripts'/source).write_text(call+'("'+reference+'")',encoding='utf-8')
     self.assertIn(prefab.resolve(),self.api.preflight(self.root)['files'])
   (self.root/'scripts'/source).write_text(call+'("uid://ffffffffffffffffffffffffffffffff")',encoding='utf-8')
   self.expect('WEB_ASSET_MISSING')
 def test_authored_graphics_keep_browser_names_and_false_zero_values(self):
  graphics='[graphics]\nexposureKey=0.06\nGTAOSTRENGTH=0.65\nautoExposure=false\nbloomStrength=0\nexposure=0.85\ntonemap="AgX"\nANISOTROPY=8\nMIPLODBIAS=-1.25\n'
  (self.root/'ProjectSettings.toml').write_text(self.settings+graphics,encoding='utf-8')
  exported=self.api.graphics_for_web(self.api.preflight(self.root)['settings']['graphics'])
  self.assertEqual(exported,{'exposureKey':0.06,'gtaoStrength':0.65,'autoExposure':False,'bloomStrength':0,'exposure':0.85,'tonemap':'AgX','anisotropy':8,'mipLodBias':-1.25})
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
