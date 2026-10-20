import assert from 'node:assert/strict';
import test from 'node:test';
import {isExportPath} from './export-paths.mjs';

test('export paths support managed framework files without escaping the package', () => {
    for (const name of ['index.html','_framework/dotnet.native.hash.wasm','_framework/System.Runtime.hash.wasm']) {
        assert.equal(isExportPath(name),true,name);
    }
    for (const name of ['',null,'../secret','/absolute','C:/absolute','a\\b','a//b','a/./b','a/../b']) {
        assert.equal(isExportPath(name),false,String(name));
    }
});
