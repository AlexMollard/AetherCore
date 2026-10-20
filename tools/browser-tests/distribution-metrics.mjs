import assert from 'node:assert/strict';

export function percentile(values, fraction) {
    assert.ok(values.length && values.every(Number.isFinite), 'Finite frame samples required');
    return [...values].sort((a,b)=>a-b)[Math.ceil(values.length*fraction)-1];
}

const required = ['zipBytes','coldInteractiveMs','warmInteractiveMs','wasmLinearMemoryBytes','frameIntervalP95Ms','editToReadyMs'];
export function checkBudgets(metrics, budgets) {
    const checks = required.map(name=>({name, actual:metrics[name], maximum:budgets[name], status:
        Number.isFinite(metrics[name]) && metrics[name]>0 && Number.isFinite(budgets[name]) && budgets[name]>0 && metrics[name]<=budgets[name] ? 'passed':'failed'}));
    return {status:checks.every(item=>item.status==='passed')?'passed':'failed',checks};
}

export async function measureDistribution(page, context, url) {
    const cdp=await context.newCDPSession(page);
    await cdp.send('Network.enable');
    await cdp.send('Network.clearBrowserCache');
    await cdp.send('Network.setCacheDisabled',{cacheDisabled:true});
    const sample=async()=>{
        await page.goto(url);
        await page.waitForFunction(()=>aetherRuntime.rendererReady && aetherRuntime.scene || aetherRuntime.status==='failed');
        assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed');
        await page.locator('#start').click();await page.locator('#canvas').focus();
        await page.waitForFunction(()=>aetherRuntime.ticks>=1 || aetherRuntime.status==='failed');
        assert.equal(await page.evaluate(()=>aetherRuntime.status),'running');
        return page.evaluate(()=>({interactiveMs:performance.now(), resources:performance.getEntriesByType('resource').map(({name,transferSize,encodedBodySize})=>({name:new URL(name).pathname,transferSize,encodedBodySize}))}));
    };
    try {
        const cold=await sample();
        await page.evaluate(()=>Module._aether_web_stop());
        await cdp.send('Network.setCacheDisabled',{cacheDisabled:false});
        // Populate an ordinary HTTP cache before measuring a cache-enabled reload.
        await sample();await page.evaluate(()=>Module._aether_web_stop());
        const warm=await sample();
        const memory=await page.evaluate(()=>({wasmLinearMemoryBytes:(globalThis.HEAPU8 || Module.HEAPU8).byteLength, jsHeapUsedBytes:performance.memory?.usedJSHeapSize ?? null}));
        const frames=await page.evaluate(()=>new Promise((resolve,reject)=>{
            const samples=[];let previous=performance.now(),last=aetherRuntime.frames;
            const deadline=setTimeout(()=>reject(new Error('120 engine frames did not complete in 15s')),15000);
            function frame(now){
                if(aetherRuntime.status==='failed'){clearTimeout(deadline);reject(new Error(JSON.stringify(aetherRuntime.diagnostics)));return;}
                if(aetherRuntime.frames>last){samples.push(now-previous);previous=now;last=aetherRuntime.frames;}
                if(samples.length===120){clearTimeout(deadline);resolve(samples);}else{requestAnimationFrame(frame);}
            }
            requestAnimationFrame(frame);
        }));
        await page.evaluate(()=>Module._aether_web_stop());
        return {coldInteractiveMs:cold.interactiveMs,warmInteractiveMs:warm.interactiveMs,...memory,
            frameIntervalP95Ms:percentile(frames,.95),frameSamplesMs:frames,coldResources:cold.resources,warmResources:warm.resources,
            methodology:'One browser-HTTP-cache-disabled navigation and one cache-enabled reload after priming; nav-to-first-focused-tick includes automated trusted gesture. 120 observed engine frame intervals, not GPU timings. Wasm linear memory excludes GPU allocations, JS heap and browser overhead.'};
    } finally {await cdp.detach();}
}

export async function qualifyAgentGame(page, context, url, embedded=false) {
    const keyboard=embedded ? page.page().keyboard : page.keyboard;
    await page.locator('#canvas').focus();await keyboard.down('Space');
    await page.waitForFunction(()=>aetherRuntime.scene==='ReferenceResult' || aetherRuntime.status==='failed');
    await keyboard.up('Space');
    const state=await page.evaluate(()=>aetherRuntime);
    assert.equal(state.status,'running');assert.equal(state.scene,'ReferenceResult');
    assert.equal(state.platform.committedWrites,2);assert.ok(state.platform.peak>0);assert.ok(state.platform.scheduledSounds>0);
    assert.match(state.uiText,/^Saved score [1-9][0-9]*$/);
    const saves=await page.evaluate(()=>({score:aetherPlatform.read('reference_score'),label:aetherPlatform.read('reference_label')}));
    assert.match(saves.score,/^[1-9][0-9]*,true,true$/,'Physics collision and trigger callbacks reach gameplay');
    assert.equal(saves.label,'caf\u00e9\0agent','Length-aware UTF-8/NUL survives durable saving');
    const fresh=embedded ? page : await context.newPage();
    try {
        await fresh.goto(url+'?scene=ReferenceResult');
        await fresh.waitForFunction(()=>aetherRuntime.uiText?.startsWith('Saved score ') || aetherRuntime.status==='failed');
        assert.equal(await fresh.evaluate(()=>aetherRuntime.uiText),state.uiText);
        assert.equal(await fresh.evaluate(()=>aetherRuntime.status),'running');
        await fresh.evaluate(()=>Module._aether_web_stop());
    } finally {if(!embedded){await fresh.close();}}
    return {status:'passed',savedText:state.uiText,audioPeak:state.platform.peak,committedWrites:state.platform.committedWrites};
}
