import assert from 'node:assert/strict';
import test from 'node:test';
import {isExportPath, pakPaths, validate3dAssets} from './export-paths.mjs';

test('export paths support managed framework files without escaping the package', () => {
    for (const name of ['index.html','_framework/dotnet.native.hash.wasm','_framework/System.Runtime.hash.wasm']) {
        assert.equal(isExportPath(name),true,name);
    }
    for (const name of ['',null,'../secret','/absolute','C:/absolute','a\\b','a//b','a/./b','a/../b']) {
        assert.equal(isExportPath(name),false,String(name));
    }
});

test('3D manifest cooked assets must exist in the pak index', () => {
    const names=['assets/model.mesh','assets/pass.wshader'];
    const pathBytes=Buffer.from(names.join(''));
    const pak=Buffer.alloc(64+40*names.length+pathBytes.length);
    pak.write('AEPK');pak.writeUInt32LE(2,4);pak.writeUInt32LE(names.length,8);
    pak.writeBigUInt64LE(BigInt(64+40*names.length),16);pak.writeBigUInt64LE(BigInt(pathBytes.length),24);
    let offset=0;
    names.forEach((name,i)=>{pak.writeUInt32LE(offset,64+40*i);pak.writeUInt32LE(Buffer.byteLength(name),68+40*i);offset+=Buffer.byteLength(name);});
    pathBytes.copy(pak,64+40*names.length);
    const paths=pakPaths(pak), manifest={cookedAssets:{meshes:[names[0]],shaders:[names[1]]}};
    validate3dAssets(manifest,paths);
    assert.throws(()=>validate3dAssets({cookedAssets:{meshes:['assets/missing.mesh'],shaders:[]}},paths),/Missing/);
    assert.throws(()=>validate3dAssets({},paths),/declarations/);
    pak.writeUInt32LE(999,68);
    assert.throws(()=>pakPaths(pak),/Invalid game.pak path/);
});
