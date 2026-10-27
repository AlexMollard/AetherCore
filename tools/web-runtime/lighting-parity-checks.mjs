import assert from 'node:assert/strict';
import {readFile, writeFile, realpath, stat} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';
import {crc32, deflateSync} from 'node:zlib';
import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

const options = Object.fromEntries(process.argv.slice(2).map(arg => {
    const split = arg.indexOf('=');
    if (split < 3) throw Error('Expected --name=value');
    return [arg.slice(2, split), arg.slice(split + 1)];
}));
const reportPath = path.resolve(options.report);
const report = {schemaVersion:1, status:'failed', captures:[], diagnostics:[], cleanup:{browser:false,server:false}};
let browser, server, timer, active;

function png(width, height, pixels) {
    const chunk = (name, data) => {
        const type = Buffer.from(name), size = Buffer.alloc(4), checksum = Buffer.alloc(4);
        size.writeUInt32BE(data.length); checksum.writeUInt32BE(crc32(Buffer.concat([type,data])));
        return Buffer.concat([size,type,data,checksum]);
    };
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width); header.writeUInt32BE(height,4); header[8]=8; header[9]=6;
    const rows = Buffer.alloc(height*(width*4+1));
    for(let y=0;y<height;y++) pixels.copy(rows,y*(width*4+1)+1,y*width*4,(y+1)*width*4);
    return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',header),chunk('IDAT',deflateSync(rows)),chunk('IEND',Buffer.alloc(0))]);
}
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
                const config = {schemaVersion:1,id:'private-lighting-parity',name:'Lighting parity',language:request.language || 'rust',
                    startupScene:active.scene,scenes:request.captures.map(c=>c.scene),graphics:request.graphics,
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
    timer=setTimeout(()=>{report.diagnostics.push('Browser helper exceeded 180 seconds');void browser.close();},180000);
    for(const capture of request.captures){
        active=capture;
        assert.ok(Number.isInteger(capture.width) && capture.width>0 && capture.width<=4096);
        assert.ok(Number.isInteger(capture.height) && capture.height>0 && capture.height<=4096);
        const item={scene:capture.scene,status:'failed',console:[],pageErrors:[]};report.captures.push(item);
        const context=await browser.newContext();
        let page;
        try {
            page=await context.newPage();page.setDefaultTimeout(20000);
            page.on('console',message=>{if(message.type()==='error')item.console.push(message.text());});
            page.on('pageerror',error=>item.pageErrors.push(String(error)));
            await page.goto(`http://127.0.0.1:${server.address().port}/?reference=1&scene=${encodeURIComponent(capture.scene)}&automation=1&test=1`);
            await page.waitForFunction(()=>aetherRuntime.scene && aetherRuntime.rendererReady || aetherRuntime.status==='failed');
            assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
            assert.equal(await page.evaluate(()=>aetherTest.version),1);
            item.adapter=await page.evaluate(()=>aetherRuntime.adapter);assert.ok(isHardwareAdapter(item.adapter),'Hardware WebGPU required');
            const initial=request.initialExtent ?? [capture.width,capture.height];
            await page.evaluate(([width,height])=>Module._aether_web_resize(width,height),initial);
            await page.waitForFunction(()=>aetherRuntime.status==='failed' || document.getElementById('start')?.disabled===false);
            assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
            await page.locator('#start').click();await page.locator('#canvas').focus();
            await page.waitForFunction(()=>aetherTest.readiness().ready || aetherRuntime.status==='failed');
            assert.equal(await page.evaluate(()=>aetherRuntime.ticks || 0),0,'Manual mode must prevent uncontrolled simulation');
            // A change at tick N lands before update N+1, matching the native runner's input convention.
            let ticks=0;
            for(const change of [...(request.changes??[])].sort((a,b)=>a.tick-b.tick)){
                if(change.tick>ticks){ticks=(await page.evaluate(n=>aetherTest.step(n),change.tick-ticks)).ticks;}
                assert.equal(ticks,change.tick,'Scheduled change must land at its tick');
                if(change.graphics){await page.evaluate(values=>{globalThis.aetherProject.graphics={...globalThis.aetherProject.graphics,...values};},change.graphics);}
                if(change.resize){await page.evaluate(([width,height])=>Module._aether_web_resize(width,height),change.resize);}
            }
            const stepped=await page.evaluate(n=>aetherTest.step(n),10-ticks);assert.equal(stepped.ticks,10);
            const image=await page.evaluate(()=>aetherTest.capture());
            item.frameEvidence=Object.fromEntries(['frameIndex','temporalFrameIndex','simulationTick','generation','exposureUsed','width','height','renderWidth','renderHeight'].map(key=>[key,image[key]]));
            assert.ok(Number.isSafeInteger(image.frameIndex) && Number.isSafeInteger(image.temporalFrameIndex) && image.simulationTick===10,'Actual accepted-frame identity/tick10 receipt required; rebuild browser runtime');
            assert.ok(Number.isFinite(image.exposureUsed) && image.exposureUsed>=0,'Finite actual GPU exposure receipt required');
            if(capture.scene.startsWith('BrowserPost')){
                const before=await page.evaluate(()=>aetherTest.snapshot());
                await page.waitForTimeout(100);
                const after=await page.evaluate(()=>aetherTest.snapshot());
                assert.equal(after.ticks,before.ticks,'Frozen manual mode must preserve simulation tick');
                assert.equal(after.frames,before.frames,'Frozen manual mode must not submit temporal render frames');
                const repeated=await page.evaluate(()=>aetherTest.capture());
                assert.equal(repeated.frameIndex,image.frameIndex,'Repeated capture must reuse the same accepted moving frame');
                assert.equal(repeated.exposureUsed,image.exposureUsed,'Repeated capture must preserve metered exposure');
                item.frozenEvidence={durationMs:100,ticks:after.ticks,frames:after.frames,
                    repeatedFrameIndex:repeated.frameIndex,repeatedExposureUsed:repeated.exposureUsed};
            }
            assert.equal(image.width,capture.width);assert.equal(image.height,capture.height);
            const pixels=Buffer.from(image.pixels);assert.equal(pixels.length,capture.width*capture.height*4);
            const stem=path.join(path.dirname(reportPath),capture.scene+'-web');
            await writeFile(stem+'.rgba',pixels);await writeFile(stem+'.png',png(capture.width,capture.height,pixels));
            item.state=await page.evaluate(()=>aetherTest.snapshot());
            assert.deepEqual(item.state.diagnostics,[]);assert.deepEqual(item.console,[]);assert.deepEqual(item.pageErrors,[]);
            await page.evaluate(()=>aetherTest.stop());
            await page.waitForFunction(()=>aetherRuntime.state==='stopped' && aetherRuntime.inFlight===0);
            item.retired=await page.evaluate(()=>({state:aetherRuntime.state,inFlight:aetherRuntime.inFlight}));
            item.rgba=stem+'.rgba';item.png=stem+'.png';item.status='passed';
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
