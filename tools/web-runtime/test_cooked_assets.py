import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
TOOL=Path(os.environ.get('AETHER_ASSETPACKER',ROOT/'build/default/tools/RelWithDebInfo/AssetPacker.exe'))

def entries(path):
    raw=path.read_bytes()
    magic,version,count,flags,path_offset,path_size,data_offset,data_size,index_hash,reserved=struct.unpack_from('<4sIII6Q',raw)
    assert magic==b'AEPK' and version==2
    result={}
    for i in range(count):
        start,length,entry_flags,pad,offset,size,content_hash=struct.unpack_from('<IIIIQQQ',raw,64+i*40)
        assert entry_flags==0
        name=raw[path_offset+start:path_offset+start+length].decode('utf-8')
        result[name]=raw[data_offset+offset:data_offset+offset+size]
    return result

class CookedAssetTests(unittest.TestCase):
    def pack(self,source,output,target):
        return subprocess.run([str(TOOL),'--target',target,'--compress-level','0',str(source),str(output)],capture_output=True,timeout=90)
    def test_web_rgba_target_is_separate_from_desktop_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'assets';source.mkdir();pak=root/'game.pak'
            shutil.copyfile(ROOT/'projects/AgentPipelineRust/assets/textures/reference.png',source/'reference.png')
            (source/'caf\u00e9.txt').write_bytes('caf\u00e9\0agent'.encode())
            web=self.pack(source,pak,'webgpu-2d-v1')
            self.assertEqual(web.returncode,0,web.stderr.decode(errors='replace'))
            cooked=entries(pak)
            self.assertEqual(cooked['reference.texture'][:4],b'AER8')
            self.assertEqual(cooked['caf\u00e9.txt'],'caf\u00e9\0agent'.encode())
            self.assertIn(b'target=webgpu-2d-v1',cooked['__aetherpak/manifest.txt'])
            native=self.pack(source,pak,'desktop')
            self.assertEqual(native.returncode,0,native.stderr.decode(errors='replace'))
            self.assertEqual(entries(pak)['reference.texture'][:4],b'DDS ')
            again=self.pack(source,pak,'webgpu-2d-v1')
            self.assertEqual(again.returncode,0,again.stderr.decode(errors='replace'))
            self.assertEqual(entries(pak)['reference.texture'][:4],b'AER8')
    def test_invalid_image_and_desktop_only_payload_fail_web_cooking(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'assets';source.mkdir();image=source/'broken.png';image.write_bytes(b'not a PNG')
            self.assertNotEqual(self.pack(source,root/'bad.pak','webgpu-2d-v1').returncode,0)
            image.unlink();(source/'native.texture').write_bytes(b'DDS '+bytes(124))
            self.assertNotEqual(self.pack(source,root/'bad.pak','webgpu-2d-v1').returncode,0)
    def test_trailing_options_are_rejected_instead_of_silently_packing_desktop(self):
        with tempfile.TemporaryDirectory() as temp:
            result=subprocess.run([str(TOOL),temp,str(Path(temp)/'game.pak'),'--target','webgpu-2d-v1'],capture_output=True,timeout=30)
            self.assertNotEqual(result.returncode,0)
    def test_unknown_target_is_an_error(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertNotEqual(self.pack(Path(temp),Path(temp)/'unknown.pak','unknown').returncode,0)

if __name__=='__main__':unittest.main()
