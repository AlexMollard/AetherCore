import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';

const code=await readFile(new URL('../../resources/web/test-bridge.js',import.meta.url),'utf8');
function fixture() {
    const state={status:'running',rendererReady:true,assetsReady:true,storageReady:true,ticks:10,
        lastRenderedFrameIndex:9,renderSceneGeneration:2,lastRenderedFrameCaptured:true};
    let requests=0;
    const image=(overrides={})=>({width:64,height:64,frameIndex:9,temporalFrameIndex:9,simulationTick:10,generation:2,exposureUsed:.8,pixels:[1,2,3,255],...overrides});
    const context={aetherRuntime:state,aetherPlatform:{started:true},location:{search:'?automation=1'},
        URLSearchParams,Uint8Array,structuredClone,setTimeout,clearTimeout,performance,
        Module:{canvas:{width:64,height:64},_aether_web_capture:()=>{requests++;}},
        addEventListener:()=>{},document:{getElementById:()=>({addEventListener:()=>{}}),addEventListener:()=>{}}};
    vm.runInNewContext(code,context);
    return {state,image,context,requests:()=>requests};
}

{
    const f=fixture();f.state.capture=f.image();
    const captured=await f.context.aetherTest.capture();
    assert.equal(captured.frameIndex,9);
    assert.equal(captured.exposureUsed,.8);
    assert.equal(f.requests(),0,'Cached moving tick must not request a new temporal frame');
}
{
    const f=fixture();f.state.capture=f.image({frameIndex:8,simulationTick:9});
    setTimeout(()=>{f.state.capture=f.image();},5);
    const captured=await f.context.aetherTest.capture(100);
    assert.equal(captured.simulationTick,10);
    assert.equal(f.requests(),0,'Pending accepted readback must be awaited, not replaced by a motionless draw');
}
{
    const f=fixture();f.state.lastRenderedFrameCaptured=false;
    f.context.Module._aether_web_capture=()=>{
        f.state.lastRenderedFrameIndex=10;
        f.state.lastRenderedFrameCaptured=true;
        f.state.capture=f.image({frameIndex:10});
    };
    assert.equal((await f.context.aetherTest.capture(100)).frameIndex,10,'Nonadvancing initial/resize fallback must match its accepted frame');
}
{
    const f=fixture();f.state.capture=f.image({generation:1});
    setTimeout(()=>{f.state.capture=f.image();},5);
    assert.equal((await f.context.aetherTest.capture(100)).generation,2,'Previous-scene readback must not qualify');
}
{
    const f=fixture();f.state.capture=f.image({width:32,simulationTick:9});
    await assert.rejects(f.context.aetherTest.capture(20),/Accepted-frame capture failed/);
    assert.equal(f.requests(),0);
}
{
    const f=fixture();let resolved=false;
    const pending=f.context.aetherTest.step(1).then(()=>{resolved=true;});
    assert.equal(f.context.aetherTestInternal.beginFrame(),1);
    f.context.aetherTestInternal.afterTick();
    await Promise.resolve();
    assert.equal(resolved,false,'Simulation alone must not complete a requested rendered step');
    f.context.aetherTestInternal.endFrame();
    await pending;
    assert.equal(resolved,true);
}
console.log('6 temporal bridge cases passed');
