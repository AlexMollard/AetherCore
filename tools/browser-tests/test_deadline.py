"""Exercise the actual PowerShell build supervisor against a stalled process tree."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
REPO=Path(__file__).resolve().parents[2]
class DeadlineTests(unittest.TestCase):
 def test_stalled_build_and_descendant_are_terminated(self):
  logs=REPO/'logs/browser-tests';logs.mkdir(parents=True,exist_ok=True)
  with tempfile.TemporaryDirectory(dir=logs) as directory:
   root=Path(directory);output=root/'command.log';script=root/'check.ps1'
   node=shutil.which('node');shell=shutil.which('pwsh')
   self.assertIsNotNone(node);self.assertIsNotNone(shell)
   command="const {spawn}=require('node:child_process');const child=spawn(process.execPath,['-e','setInterval(()=>{},1000)'],{windowsHide:true,stdio:'ignore'});console.log(child.pid);setInterval(()=>{},1000)"
   quote=lambda value:"'"+str(value).replace("'","''")+"'"
   script.write_text('. '+quote(REPO/'scripts/Invoke-BrowserCommand.ps1')+'\ntry { Invoke-BrowserCommand '+quote(node)+' @('+quote('-e')+','+quote(command)+') '+quote(output)+' 500; exit 9 } catch [TimeoutException] { exit 0 }',encoding='utf-8')
   started=time.monotonic()
   result=subprocess.run([shell,'-NoProfile','-File',str(script)],capture_output=True,timeout=15)
   self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'));self.assertLess(time.monotonic()-started,10)
   pid=int(output.read_text(encoding='utf-8').splitlines()[0])
   for _ in range(100):
    try:os.kill(pid,0)
    except OSError:break
    time.sleep(.02)
   else:self.fail('Owned compiler descendant survived timeout')
if __name__=='__main__':unittest.main()
