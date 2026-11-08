import assert from 'node:assert/strict';

function gateway(value){
 assert.equal(typeof value,'string','Verification gateway URL must be a string');
 assert.ok(value.length>0 && !/[\s\\?#]/.test(value),'Verification gateway URL cannot contain whitespace, query or fragment');
 let url;
 try{url=new URL(value);}catch{throw new Error('Invalid verification gateway URL');}
 assert.ok((url.protocol==='ws:' || url.protocol==='wss:') && url.hostname && url.pathname==='/net' && !url.username && !url.password,'Verification gateway must be a ws/wss /net endpoint without credentials');
 return url.href;
}

// The publisher verifies original package hashes first. This creates response bytes
// for its temporary HTTP server; it never edits the on-disk deployment artifact.
export function overrideVerificationGateway(bytes,value){
 if(value===undefined)return {bytes,override:null};
 const to=gateway(value),prefix='globalThis.aetherProject=',source=bytes.toString('utf8').trim();
 assert.ok(source.startsWith(prefix) && source.endsWith(';'),'Invalid versioned project.js network configuration assignment');
 let config;
 try{config=JSON.parse(source.slice(prefix.length,-1));}catch{throw new Error('Invalid project.js network configuration JSON');}
 assert.ok(config && !Array.isArray(config) && config.schemaVersion===1 && config.network && typeof config.network==='object' && !Array.isArray(config.network),'Missing versioned project.js network configuration');
 gateway(config.network.gatewayUrl);
 const from=config.network.gatewayUrl;
 config.network.gatewayUrl=to;
 return {bytes:Buffer.from(prefix+JSON.stringify(config)+';\n'),override:{from,to,scope:'Temporary verification server deployment configuration; artifact files unchanged; startup readiness, not full gameplay parity'}};
}
