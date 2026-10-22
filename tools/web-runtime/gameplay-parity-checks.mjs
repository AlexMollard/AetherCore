import assert from 'node:assert/strict';
import {readFile, writeFile, realpath, stat} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';
import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

// Steps the browser runtime to the requested checkpoint ticks and records the read-only
// aetherTest.entities() state. Never writes ECS state; input uses the same key-down bridge as tests.
const options = Object.fromEntries(process.argv.slice(2).map(arg => {
    const split = arg.indexOf('=');
    if (split < 3) throw Error('Expected --name=value');
    return [arg.slice(2, split), arg.slice(split + 1)];
}));
const reportPath = path.resolve(options.report);
const report = {schemaVersion:1, status:'failed', scenes:[], diagnostics:[], cleanup:{browser:false,server:false}};
let browser, server, timer, active;

try {
    const request = JSON.parse(await readFile(options.request,'utf8'));
    const root = await realpath(request.root);
    const pins = JSON.parse(await readFile(new URL('../experiments/rust-webgpu/toolchain.json',import.meta.url),'utf8'));
    const require = createRequire(import.meta.url), module = request.playwright || process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
    const {chromium} = require(module);
    assert.equal(require(path.join(module,'package.json')).version,pins.playwright);
    report.playwrightVersion=pins.playwright;
    server = createServer(async(req,res)=>{
        try {
            const url = new URL(req.url,'http://localhost');
            if(url.pathname==='/favicon.ico'){res.writeHead(204).end();return;}
            if(url.pathname==='/project.js'){
                const config = {schemaVersion:1,id:'private-gameplay-parity',name:'Gameplay parity',language:request.language,
                    startupScene:active.scene,scenes:request.declaredScenes,
                    paths:{assets:'assets',scenes:'scenes',prefabs:'assets/prefabs',scripts:'scripts'}};
                res.writeHead(200,{'Content-Type':'text/javascript','Cache-Control':'no-store'}).end('globalThis.aetherProject='+JSON.stringify(config)+';');
                return;
            }
            const candidate=path.resolve(root,'.'+decodeURIComponent(url.pathname));
            const target=await realpath((await stat(candidate)).isDirectory()?path.join(candidate,'index.html'):candidate);
            const relative=path.relative(root,target);
            if(relative.startsWith('..') || path.isAbsolute(relative)){res.writeHead(403).end();return;}
            const mime={'.js':'text/javascript','.mjs':'text/javascript','.html':'text/html','.wasm':'application/wasm'}[path.extname(target)] || 'application/octet-stream';
            res.writeHead(200,{'Content-Type':mime,'Cache-Control':'no-store'}).end(await readFile(target));
        }catch{res.writeHead(404).end();}
    });
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    browser=await chromium.launch({channel:'chrome',headless:true,timeout:15000,args:['--enable-gpu']});
    report.browserVersion=browser.version();
    timer=setTimeout(()=>{report.diagnostics.push('Browser helper exceeded its deadline');void browser.close();},request.deadlineMs ?? 180000);
    for(const scene of request.scenes){
        active=scene;
        const item={scene:scene.scene,status:'failed',checkpoints:{},console:[],pageErrors:[]};report.scenes.push(item);
        const context=await browser.newContext();
        let page;
        try {
            page=await context.newPage();page.setDefaultTimeout(20000);
            page.on('console',message=>{if(message.type()==='error')item.console.push(message.text());});
            page.on('pageerror',error=>item.pageErrors.push(String(error)));
            await page.goto(`http://127.0.0.1:${server.address().port}/?reference=1&scene=${encodeURIComponent(scene.scene)}&automation=1&test=1`);
            await page.waitForFunction(()=>aetherRuntime.scene && aetherRuntime.rendererReady || aetherRuntime.status==='failed');
            assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
            assert.equal(await page.evaluate(()=>aetherTest.version),1);
            item.adapter=await page.evaluate(()=>aetherRuntime.adapter);assert.ok(isHardwareAdapter(item.adapter),'Hardware WebGPU required');
            await page.evaluate(([width,height])=>Module._aether_web_resize(width,height),[512,512]);
            await page.waitForFunction(()=>aetherRuntime.status==='failed' || document.getElementById('start')?.disabled===false);
            assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
            await page.locator('#start').click();await page.locator('#canvas').focus();
            await page.waitForFunction(()=>aetherTest.readiness().ready || aetherRuntime.status==='failed');
            assert.equal(await page.evaluate(()=>aetherRuntime.ticks || 0),0,'Manual mode must prevent uncontrolled simulation');
            const checkpoints=new Set(scene.checkpoints), inputs=[...scene.input].sort((a,b)=>a.tick-b.tick);
            const stops=[...new Set([...scene.checkpoints,...inputs.map(e=>e.tick)])].filter(t=>t>=0).sort((a,b)=>a-b);
            const last=Math.max(...scene.checkpoints);
            let ticks=0;
            for(const stop of stops){
                if(stop>last)break;
                if(stop>ticks){
                    const snapshot=await page.evaluate(n=>aetherTest.step(n),stop-ticks);
                    ticks=snapshot.ticks;
                    assert.equal(ticks,stop,'Step must land on the requested tick');
                }
                // An event at tick N lands before update N+1, the native runner's convention.
                for(const event of inputs.filter(e=>e.tick===stop))await page.evaluate(e=>aetherTest.input({key:e.key,down:e.down}),event);
                if(checkpoints.has(stop)&&stop>0){
                    const state=await page.evaluate(()=>aetherTest.entities());
                    assert.equal(state.tick,stop,'Entity state must belong to the checkpoint tick');
                    item.checkpoints[String(stop)]=state;
                }
            }
            const final=await page.evaluate(()=>aetherTest.snapshot());
            assert.deepEqual(final.diagnostics,[]);assert.deepEqual(item.console,[]);assert.deepEqual(item.pageErrors,[]);
            await page.evaluate(()=>aetherTest.stop());
            await page.waitForFunction(()=>aetherRuntime.state==='stopped' && aetherRuntime.inFlight===0);
            item.status='passed';
        }catch(error){
            item.error=String(error.stack || error);
            if(page){
                try{item.runtime=await page.evaluate(()=>typeof aetherRuntime==='undefined'?null:globalThis.aetherTest?.snapshot?.() || aetherRuntime);}
                catch(snapshotError){item.snapshotError=String(snapshotError);}
            }
            throw error;
        }finally{await context.close();}
    }
    report.status='passed';
} catch(error){report.diagnostics.push(String(error.stack || error));}
finally{
    clearTimeout(timer);
    if(browser){try{await browser.close();report.cleanup.browser=true;}catch(error){report.diagnostics.push(String(error));}}
    if(server){server.closeAllConnections();await new Promise(resolve=>server.close(resolve));report.cleanup.server=true;}
    if(!report.cleanup.browser || !report.cleanup.server)report.status='failed';
    await writeFile(reportPath,JSON.stringify(report,null,2)+'\n');
}
process.exitCode=report.status==='passed'?0:1;
