import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import os from 'node:os';
import path from 'node:path';
import {checkBudgets, measureDistribution, qualifyAgentGame} from './distribution-metrics.mjs';
import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

const options=Object.fromEntries(process.argv.slice(2).map(arg=>{const i=arg.indexOf('=');return [arg.slice(2,i),arg.slice(i+1)];}));
const root=path.resolve(options.root), reportPath=path.resolve(options.report);
const report={schemaVersion:1,status:'failed',target:'web',profile:'webgpu-2d-v1',environment:{os:os.platform(),release:os.release()},checks:[],console:[],pageErrors:[]};
const started=performance.now();let browser,server;
try {
 const manifest=JSON.parse(await readFile(path.join(root,'manifest.json'),'utf8'));
 assert.equal(manifest.schemaVersion,1);assert.equal(manifest.target,'web');assert.equal(manifest.profile,'webgpu-2d-v1');
 for(const file of manifest.files){
  assert.equal(path.basename(file.path),file.path);assert.ok(!/[:\\]/.test(file.path));
  const bytes=await readFile(path.join(root,file.path));assert.equal(bytes.length,file.sizeBytes);
  assert.equal(createHash('sha256').update(bytes).digest('hex'),file.sha256);
  if(/\.(js|html|wasm)$/.test(file.path))for(const marker of ['aetherTestInternal','aether_web_lose_device','aether_browser_test_panic','test-bridge.js'])assert.equal(bytes.includes(Buffer.from(marker)),false,marker+' leaked into export');
 }
 report.checks.push({name:'manifest-integrity-and-production-boundary',status:'passed'});
 const require=createRequire(import.meta.url), module=process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
 const {chromium}=require(module);assert.equal(require(path.join(module,'package.json')).version,manifest.toolchains.playwright);
 server=createServer(async(req,res)=>{
  try {
   const url=new URL(req.url,'http://localhost');
   if(url.pathname==='/favicon.ico'){res.writeHead(204).end();return;}
   if(url.pathname==='/embed.html'){const src=options.measure ? `http://localhost:${server.address().port}/nested/game/` : '/nested/game/';res.writeHead(200,{'Content-Type':'text/html'}).end(`<!doctype html><title>Relocated Web export iframe</title><iframe title="Game" sandbox="allow-scripts allow-same-origin" allow="autoplay; fullscreen; gamepad; webgpu" src="${src}" style="width:640px;height:700px;border:0"></iframe>`);return;}
   if(!url.pathname.startsWith('/nested/game/')){res.writeHead(404).end();return;}
   const name=decodeURIComponent(url.pathname.slice('/nested/game/'.length)) || 'index.html';
   if(name!==path.basename(name)){res.writeHead(403).end();return;}
   const bytes=await readFile(path.join(root,name));
   const mime={'.js':'text/javascript','.html':'text/html','.wasm':'application/wasm'}[path.extname(name)] || 'application/octet-stream';
   res.writeHead(200,{'Content-Type':mime,'Cache-Control':options.measure ? 'public, max-age=3600' : 'no-store'}).end(bytes);
  } catch {res.writeHead(404).end();}
 });
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const base=`http://127.0.0.1:${server.address().port}`;
 browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000,args:['--enable-gpu']});
 report.environment.browserVersion=browser.version();
 const context=await browser.newContext();
 try {
  const page=await context.newPage();page.setDefaultTimeout(20000);
  page.on('console',message=>report.console.push({type:message.type(),text:message.text()}));page.on('pageerror',error=>report.pageErrors.push(error.stack));
  await page.goto(base+'/nested/game/');
  await page.waitForFunction(()=>(aetherRuntime.rendererReady && aetherRuntime.fontReady && aetherRuntime.scene) || aetherRuntime.status==='failed');
  assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
  report.environment.gpu=await page.evaluate(()=>aetherRuntime.adapter);assert.ok(isHardwareAdapter(report.environment.gpu),'Hardware WebGPU adapter required');
  assert.equal(await page.evaluate(()=>typeof globalThis.aetherTest),'undefined');
  assert.equal(await page.evaluate(()=>aetherRuntime.scene),manifest.project.startupScene);
  await page.locator('#start').click();await page.locator('#canvas').focus();await page.keyboard.down('Space');
  await page.waitForFunction(()=>aetherRuntime.ticks>=5 || aetherRuntime.status==='failed');
  await page.keyboard.up('Space');
  if(options.scenario==='agent-reference-v1'){report.agentGame=await qualifyAgentGame(page,context,base+'/nested/game/');}
  report.directState=await page.evaluate(()=>{const{capture,...state}=aetherRuntime;return state;});
  assert.equal(report.directState.status,'running');assert.equal(report.directState.diagnostics.length,0);
  report.checks.push({name:'relocated-nested-path-gameplay',status:'passed'});
  const screenshot=path.join(path.dirname(reportPath),'web-export.png');await page.screenshot({path:screenshot});report.screenshot=screenshot;
  await page.evaluate(()=>Module._aether_web_stop());
  await page.goto(base+'/embed.html');
  await page.locator('iframe').waitFor();
  const frame=page.frames().find(frame=>frame.url().includes('/nested/game/'));
  assert.ok(frame);await frame.waitForFunction(()=>aetherRuntime.scene && aetherRuntime.rendererReady && aetherRuntime.fontReady);
  await frame.locator('#start').click();await frame.locator('#canvas').focus();
  await frame.waitForFunction(()=>aetherRuntime.ticks>=3 || aetherRuntime.status==='failed');
  assert.equal(await frame.evaluate(()=>aetherRuntime.status),'running');
  report.iframePolicy=await frame.evaluate(()=>{const policy=document.featurePolicy;const supported=policy?.features() || [];return {origin:location.origin,secureContext:isSecureContext,webgpuPolicyRecognized:supported.includes('webgpu'),webgpuAllowed:supported.includes('webgpu') ? policy.allowsFeature('webgpu') : null,autoplayAllowed:policy?.allowsFeature('autoplay') ?? null};});
  assert.equal(report.iframePolicy.secureContext,true);assert.notEqual(report.iframePolicy.webgpuAllowed,false);
  if(options.scenario==='agent-reference-v1'){report.iframeGame=await qualifyAgentGame(frame,context,frame.url(),true);}
  report.checks.push({name:'local-iframe-gesture-and-gameplay',status:'passed'});
  await frame.evaluate(()=>Module._aether_web_stop());
  assert.equal(report.pageErrors.length,0,report.pageErrors.join('\n'));assert.equal(report.console.filter(item=>item.type==='error').length,0,JSON.stringify(report.console));
  const unavailable=await context.newPage();try{
   await unavailable.addInitScript(()=>Object.defineProperty(navigator,'gpu',{value:undefined,configurable:true}));
   await unavailable.goto(base+'/nested/game/');await unavailable.waitForFunction(()=>aetherRuntime.status==='failed');
   await unavailable.waitForFunction(()=>/WebGPU|localhost|HTTPS/.test(document.getElementById('result').textContent));
   report.unavailableState=await unavailable.evaluate(()=>aetherRuntime);
   assert.ok(report.unavailableState.diagnostics.some(item=>item.code==='WEBGPU_UNAVAILABLE'));
  }finally{await unavailable.close();}
  report.checks.push({name:'actionable-webgpu-unavailable',status:'passed'});
  if(options.measure){report.measurements=await measureDistribution(page,context,base+'/nested/game/');report.checks.push({name:'measured-hardware-gameplay',status:'passed'});if(options.budgets){const budgets=JSON.parse(await readFile(options.budgets,'utf8'));report.budgetChecks=checkBudgets({...report.measurements,zipBytes:Number(options.zipBytes),editToReadyMs:Number(options.editToReadyMs)},budgets);assert.equal(report.budgetChecks.status,'passed',JSON.stringify(report.budgetChecks));}}
 }catch(error){report.failureStates=await Promise.all(context.pages().map(page=>page.evaluate(()=>globalThis.aetherRuntime).catch(()=>null)));throw error;}finally{await context.close();}
 report.status='passed';
}catch(error){report.error=error.stack;}
finally{
 await browser?.close();if(server){server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
 report.durationMs=performance.now()-started;report.cleanup={browserClosed:!browser||!browser.isConnected(),serverClosed:!server||!server.listening};
 await mkdir(path.dirname(reportPath),{recursive:true});await writeFile(reportPath,JSON.stringify(report,null,2));process.exitCode=report.status==='passed'?0:1;
}
