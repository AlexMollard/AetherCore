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
 def test_csharp_is_specific_failure(self):
  (self.root/'ProjectSettings.toml').write_text(self.settings.replace('rust','csharp'),encoding='utf-8');self.expect('WEB_CSHARP_UNAVAILABLE')
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
 def test_shader_additional_asset_fails(self):
  (self.root/'assets/test.slang').write_text('shader',encoding='utf-8');(self.root/'ProjectSettings.toml').write_text(self.settings+'[publish.web]\nadditional_assets=["project://assets/test.slang"]\n',encoding='utf-8');self.expect('WEB_SHADER_UNSUPPORTED')
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
if __name__=='__main__':unittest.main()
