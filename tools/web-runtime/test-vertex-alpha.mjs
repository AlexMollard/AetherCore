import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const context=vm.createContext({importScripts(){},self:{},TextEncoder,TextDecoder,URL,Blob,
 prepareSkeleton(){return {nodes:[],skins:[],animations:[]};},prepareMaterials(){return [];}});
vm.runInContext(fs.readFileSync(new URL('../../resources/web/streaming-worker.js',import.meta.url),'utf8'),context);
const mesh=new Uint8Array(72+3*96+6),view=new DataView(mesh.buffer);
for(let index=0;index<3;index++){
 view.setUint32(72+index*96+48,0x00ffffff|([0,128,255][index]<<24),true);
 view.setUint16(72+3*96+index*2,index,true);
}
const info={vertices:3,indices:3,type:0,offset:72+3*96,ranges:[{first:0,count:3,material:0}],keys:['alpha'],skinRef:'',bounds:Array(10).fill(0)};
const output=context.prepareMesh(mesh,info,'project://alpha.mesh',{},4096),header=new DataView(output.buffer);
assert.deepEqual([...output.subarray(0,4)],[65,69,80,51],'Prepared format distinguishes RGBA runtime vertices');
assert.equal(header.getUint32(8,true),3*104+3*4);
const start=16+header.getUint32(4,true);
for(let index=0;index<3;index++)assert.ok(Math.abs(header.getFloat32(start+index*104+100,true)-[0,128/255,1][index])<1e-7,'Worker preserves each packed vertex alpha');
console.log('Prepared model RGBA alpha and stride regression passed');
