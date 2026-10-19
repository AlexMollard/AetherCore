import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {readFile,stat,mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {createRequire} from 'node:module';
import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';
const require=createRequire(import.meta.url);
const options=Object.fromEntries(process.argv.slice(2).map(value=>{const i=value.indexOf('=');if(i<3)throw Error('--name=value required');return[value.slice(2,i),value.slice(i+1)]}));
const root=path.resolve(options.root||'build/webgpu-browser/web');
const reportPath=path.resolve(options.report||'logs/web-runtime/browser.json');
const playwrightModule=options.playwright||process.env.AETHER_PLAYWRIGHT_MODULE||'playwright';
const {chromium}=require(playwrightModule);
const report={schemaVersion:1,status:'failed',cases:[],playwrightVersion:require(path.join(playwrightModule,'package.json')).version};
const server=createServer(async(req,res)=>{try{const name=decodeURIComponent(new URL(req.url,'http://localhost').pathname);if(name==='/favicon.ico'){res.writeHead(204).end();return}const target=path.resolve(root,'.'+name);const relative=path.relative(root,target);if(relative.startsWith('..')||path.isAbsolute(relative)){res.writeHead(403).end();return}const file=(await stat(target)).isDirectory()?path.join(target,'index.html'):target;res.writeHead(200,{'Content-Type':({'.js':'text/javascript','.html':'text/html','.wasm':'application/wasm','.json':'application/json'})[path.extname(file)]||'application/octet-stream','Cache-Control':'no-store'}).end(await readFile(file));}catch{res.writeHead(404).end()}});
let browser;
async function capture(page){await page.evaluate(()=>{delete aetherRuntime.capture;Module._aether_web_capture()});await page.waitForFunction(()=>!!aetherRuntime.capture,undefined,{timeout:10000});return page.evaluate(()=>aetherRuntime.capture)}
function pixel(image,x,y){return image.pixels.slice((y*image.width+x)*4,(y*image.width+x)*4+4)}
try{
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 browser=await chromium.launch({...(options.executable?{executablePath:options.executable}:{channel:'chrome'}),headless:true,args:['--enable-gpu']});
 report.browserVersion=browser.version();
 for(const name of ['sprite-input-resize-shutdown','unavailable','limits','device-loss','stop-during-init','stop-in-flight']){
  const item={name,status:'failed',console:[],pageErrors:[]};report.cases.push(item);
  const page=await browser.newPage();page.on('console',m=>item.console.push({type:m.type(),text:m.text()}));page.on('pageerror',e=>item.pageErrors.push(e.message));
  try{
   const query=name==='unavailable'?'?forceNoAdapter=1':name==='limits'?'?forceLimits=1':name==='stop-during-init'?'?stopBeforeReady=1':name==='stop-in-flight'?'?stopAfterSubmit=1':'';
   await page.goto(`http://127.0.0.1:${server.address().port}/${query}`);
   if(name.startsWith('stop-')){await page.waitForFunction(()=>aetherRuntime.state==='stopped',undefined,{timeout:10000});const state=await page.evaluate(()=>aetherRuntime);assert.equal(state.gameplayStopped,true);assert.equal(state.diagnostics.length,0,JSON.stringify(state));if(name==='stop-in-flight'){assert.ok(state.peakInFlight>0);assert.equal(state.inFlight,0);assert.ok(state.capture);}}
   else if(query){await page.waitForFunction(()=>aetherRuntime.status==='failed');const result=await page.evaluate(()=>aetherRuntime);assert.ok(result.diagnostics.some(d=>d.code===(name==='limits'?'WEBGPU_LIMITS':'WEBGPU_UNAVAILABLE')),JSON.stringify(result));assert.equal(result.diagnostics.length,1,JSON.stringify(result));item.result=result;}
   else{
    await page.waitForFunction(()=>aetherRuntime.rendererReady||aetherRuntime.status==='failed',undefined,{timeout:30000});
    let state=await page.evaluate(()=>aetherRuntime);assert.notEqual(state.status,'failed',JSON.stringify(state));assert.ok(isHardwareAdapter(state.adapter),'Hardware adapter identity required: '+JSON.stringify(state.adapter));
    if(name==='device-loss'){await page.evaluate(()=>Module._aether_web_lose_device());await page.waitForFunction(()=>aetherRuntime.state==='device-lost');assert.ok((await page.evaluate(()=>aetherRuntime.diagnostics)).some(d=>d.code==='WEBGPU_DEVICE_LOST'));}
    else{
     const image=await capture(page);const expected=[[255,0,0,255],[0,255,0,255],[0,0,255,255],[255,255,0,255]];
     for(let y=0;y<64;++y){for(let x=0;x<64;++x){assert.deepEqual(pixel(image,x,y),expected[(y>=32?2:0)+(x>=32?1:0)],`pixel ${x},${y}`)}}
     item.pixelChecks=4096;
     const before=await page.evaluate(()=>({x:aetherRuntime.x,ticks:aetherRuntime.ticks}));
     await page.keyboard.down('ArrowRight');await page.waitForFunction(ticks=>aetherRuntime.ticks>=ticks+15,before.ticks);await page.keyboard.up('ArrowRight');
     state=await page.evaluate(()=>aetherRuntime);assert.ok(state.x>before.x+.2,JSON.stringify(state));item.movement={before:before.x,after:state.x};
     const moved=await capture(page);assert.deepEqual(pixel(moved,0,32),[0,0,0,255]);
     await page.evaluate(()=>Module._aether_web_resize(96,48));const resized=await capture(page);assert.equal(resized.width,96);assert.equal(resized.height,48);item.resize={width:resized.width,height:resized.height};
     await page.screenshot({path:path.join(path.dirname(reportPath),'runtime.png')});
    }
   }
   await page.evaluate(()=>Module._aether_web_stop());await page.waitForFunction(()=>aetherRuntime.state==='stopped',undefined,{timeout:10000});
   const {capture:unused,...result}=await page.evaluate(()=>aetherRuntime);item.result=result;assert.equal(result.gameplayStopped,true);assert.equal(item.pageErrors.length,0,item.pageErrors.join(';'));
   if(name==='sprite-input-resize-shutdown'){assert.equal(result.diagnostics.length,0,JSON.stringify(result));assert.equal(item.console.filter(m=>m.type==='error').length,0,JSON.stringify(item.console));}
   item.status='passed';
  }catch(error){item.error=error.stack;console.log(name,error.message)}finally{await page.close()}
 }
 report.status=report.cases.every(c=>c.status==='passed')?'passed':'failed';
}catch(error){report.error=error.stack}finally{await browser?.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve));await mkdir(path.dirname(reportPath),{recursive:true});await writeFile(reportPath,JSON.stringify(report,null,2));console.log(JSON.stringify({status:report.status,cases:report.cases.map(({name,status,error})=>({name,status,error})),error:report.error}));process.exitCode=report.status==='passed'?0:1}
