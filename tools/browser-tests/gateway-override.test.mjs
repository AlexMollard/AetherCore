import assert from 'node:assert/strict';
import test from 'node:test';
import {overrideVerificationGateway} from './gateway-override.mjs';

const config={schemaVersion:1,scene:'project://scene.toml',network:{gatewayUrl:'ws://127.0.0.1:8789/net',authToken:'unchanged',targetAlias:'oathmere',serverPort:27056},extra:{nested:['keep',3]}};
const source=value=>Buffer.from('globalThis.aetherProject='+JSON.stringify(value)+';\n');
test('verification override changes only the served gateway and preserves input bytes',()=>{
 const bytes=source(config),before=Buffer.from(bytes),result=overrideVerificationGateway(bytes,'ws://127.0.0.1:8790/net');
 assert.deepEqual(bytes,before);assert.notEqual(result.bytes,bytes);
 const served=JSON.parse(result.bytes.toString().trim().slice('globalThis.aetherProject='.length,-1));
 assert.deepEqual(served,{...config,network:{...config.network,gatewayUrl:'ws://127.0.0.1:8790/net'}});
 assert.equal(result.override.from,config.network.gatewayUrl);assert.equal(result.override.to,'ws://127.0.0.1:8790/net');
 assert.match(result.override.scope,/startup readiness/);assert.match(result.override.scope,/not full gameplay parity/);
 assert.equal(overrideVerificationGateway(bytes,undefined).bytes,bytes);
 assert.equal(overrideVerificationGateway(bytes,undefined).override,null);
});
test('verification gateway accepts only ws/wss net endpoints without credentials or URL extras',()=>{
 for(const value of ['', 'https://example.com/net','ws://user:pass@example.com/net','ws://example.com/other','ws://example.com/net/','ws://example.com/net?','ws://example.com/net?token=x','ws://example.com/net#','ws://example.com/net#x',' ws://example.com/net','ws://example.com/net\n','ws://','ws://example.com:99999/net'])assert.throws(()=>overrideVerificationGateway(source(config),value),/gateway/i,value);
 assert.equal(overrideVerificationGateway(source(config),'wss://example.com/net').override.to,'wss://example.com/net');
});
test('missing or malformed versioned network configuration fails before browser startup',()=>{
 for(const value of [null,{},[],{schemaVersion:2,network:config.network},{schemaVersion:1},{schemaVersion:1,network:[]},{schemaVersion:1,network:{}},{schemaVersion:1,network:{gatewayUrl:23}},{schemaVersion:1,network:{gatewayUrl:'http://localhost/net'}}])assert.throws(()=>overrideVerificationGateway(source(value),'ws://127.0.0.1:8790/net'),/configuration|gateway/i);
 for(const bytes of [Buffer.from('globalThis.aetherProject=invalid;'),Buffer.from('doSomething();'),Buffer.from('globalThis.aetherProject={};evil();')])assert.throws(()=>overrideVerificationGateway(bytes,'ws://127.0.0.1:8790/net'));
});
