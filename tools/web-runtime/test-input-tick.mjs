import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../../resources/web/platform.js', import.meta.url), 'utf8');
const handlers = new Map();
const canvas = {width:640,height:360,addEventListener:(name,fn)=>handlers.set(name,fn),
    getBoundingClientRect:()=>({left:0,top:0,width:640,height:360}),focus(){},setPointerCapture(){}};
const element = {addEventListener(){}};
const context = {console,URLSearchParams,location:{search:''},navigator:{},
    document:{getElementById:id=>id==='canvas'?canvas:element,addEventListener(){},activeElement:canvas,hasFocus:()=>true,hidden:false},
    window:{addEventListener(){}},setTimeout,clearTimeout,Map,Set};
vm.runInNewContext(source,context);
const input = context.aetherInput;

handlers.get('pointerdown')({buttons:1,clientX:100,clientY:80,pointerId:1});
handlers.get('pointerup')({buttons:0,clientX:100,clientY:80});
handlers.get('keydown')({key:'x'});
// Multiple renders without a simulation step must leave the bounded producer state intact.
for(let render=0;render<8;render++) assert.equal(input.pressed,1);
const first = input.takeTick();
assert.equal(first.buttons,1,'Short click survives until the next simulation tick');
assert.equal(first.typedText,'x');
assert.equal(input.takeTick().buttons,0,'Catch-up tick must release the pulse instead of repeating the press');
assert.equal(input.takeTick().typedText,'','Typed input is consumed once');
handlers.get('pointerdown')({buttons:1,clientX:100,clientY:80,pointerId:1});
assert.equal(input.takeTick().buttons,1);
assert.equal(input.takeTick().buttons,1,'Held state survives later ticks without replaying a new edge');
for(let i=0;i<20000;i++) handlers.get('keydown')({key:'x'});
assert.equal(input.takeTick().typedText.length,4096,'Paused/render-only text accumulation is bounded');
console.log('Browser tick input retention, one-shot consumption, held state and bounds passed');
