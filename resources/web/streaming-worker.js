'use strict';
importScripts('streaming-model-skeleton.js', 'streaming-model-materials.js', 'AetherSceneParser.js');
const jobs = new Map();
function dimensions(bytes, path) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const n = bytes.length;
    const ascii = (offset, text) => offset + text.length <= n && [...text].every((c, i) => bytes[offset+i] === c.charCodeAt(0));
    if (n >= 24 && ascii(1, 'PNG\r\n\x1a\n') && bytes[0] === 137) return [v.getUint32(16), v.getUint32(20)];
    if (n >= 26 && ascii(0, 'BM')) return [v.getUint32(18, true), Math.abs(v.getInt32(22, true))];
    if (/\.tga$/i.test(path) && n>=18) return [v.getUint16(12,true),v.getUint16(14,true)];
    if (n >= 30 && ascii(0, 'RIFF') && ascii(8, 'WEBP')) {
        if (ascii(12, 'VP8X')) return [1 + bytes[24] + (bytes[25]<<8) + (bytes[26]<<16), 1 + bytes[27] + (bytes[28]<<8) + (bytes[29]<<16)];
        if (ascii(12, 'VP8 ') && n >= 30 && bytes[23] === 0x9d && bytes[24] === 1 && bytes[25] === 0x2a) return [v.getUint16(26,true)&0x3fff, v.getUint16(28,true)&0x3fff];
        if (ascii(12, 'VP8L') && bytes[20] === 0x2f) { const bits=v.getUint32(21,true); return [(bits&0x3fff)+1, ((bits>>>14)&0x3fff)+1]; }
    }
    if (n >= 4 && bytes[0] === 0xff && bytes[1] === 0xd8) {
        let p=2;
        while (p+4 <= n) {
            if (bytes[p++] !== 0xff) throw Error('Invalid JPEG marker');
            while (p<n && bytes[p]===0xff) ++p;
            const marker=bytes[p++]; if (marker===0xd9 || marker===0xda) break;
            if (marker===0x01 || (marker>=0xd0 && marker<=0xd7)) continue;
            if (p+2>n) break; const len=v.getUint16(p); if(len<2 || p+len>n) break;
            if ([0xc0,0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc9,0xca,0xcb,0xcd,0xce,0xcf].includes(marker)) {
                if(len<7) break; return [v.getUint16(p+5),v.getUint16(p+3)];
            }
            p+=len;
        }
    }
    throw Error('Unsupported or invalid streamed image: '+path);
}
function meshInfo(bytes) {
    if(bytes.length<72) throw Error('Truncated mesh header');
    const v=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
    if(v.getUint32(0,true)!==0x4853454d || ![3,4].includes(v.getUint32(4,true))) throw Error('Invalid mesh header');
    const vertices=v.getUint32(8,true), indices=v.getUint32(12,true), skin=v.getUint32(16,true), materials=v.getUint32(20,true), submeshes=v.getUint32(24,true), type=v.getUint8(68);
    for(let i=0;i<10;++i) if(!Number.isFinite(v.getFloat32(28+i*4,true))) throw Error('Non-finite mesh bounds');
    for(let i=0;i<3;++i) if(v.getFloat32(28+i*4,true)>v.getFloat32(40+i*4,true)) throw Error('Invalid mesh bounds');
    if(v.getFloat32(64,true)<0) throw Error('Invalid mesh radius');
    if(vertices>8388608 || indices>33554432 || type>1 || skin>4096 || submeshes>65536 || materials>65536) throw Error('Mesh header exceeds limits');
    let offset=72+vertices*96, indexBytes=indices*(type?4:2), sub=offset+indexBytes;
    if(!Number.isSafeInteger(sub) || sub+submeshes*12>bytes.length) throw Error('Truncated mesh streams');
    for(let i=0;i<indices;++i) if((type?v.getUint32(offset+i*4,true):v.getUint16(offset+i*2,true))>=vertices) throw Error('Mesh index out of bounds');
    const ranges=[];
    for(let i=0;i<submeshes;++i) { const first=v.getUint32(sub+i*12,true), count=v.getUint32(sub+i*12+4,true), material=v.getUint32(sub+i*12+8,true); if(!count || first>indices || count>indices-first) throw Error('Invalid submesh range'); ranges.push({first,count,material}); }
    let cursor=sub+submeshes*12;
    const decode=new TextDecoder('utf-8',{fatal:true});
    function string16() { if(cursor+2>bytes.length) throw Error('Truncated mesh string'); const length=v.getUint16(cursor,true); cursor+=2; if(cursor+length>bytes.length) throw Error('Truncated mesh string'); const result=decode.decode(bytes.subarray(cursor,cursor+length)); cursor+=length; return result; }
    const keys=[];
    if(v.getUint32(4,true)>=4) for(let i=0;i<submeshes;++i) keys.push(string16() || 'unidentified');
    if(cursor+skin>bytes.length) throw Error('Truncated mesh skin reference');
    const skinRef=decode.decode(bytes.subarray(cursor,cursor+skin)); cursor+=skin;
    const materialPaths=[]; for(let i=0;i<materials;++i) materialPaths.push(string16());
    const bounds=Array.from({length:10},(_,i)=>v.getFloat32(28+i*4,true));
    return {vertices, indices, type, offset, size:16+vertices*96+indices*4, ranges, keys, skinRef, materialPaths, bounds};
}
function prepareTga(bytes, width, height) {
    const type=bytes[2], bits=bytes[16], descriptor=bytes[17];
    if(bytes[1]!==0 || ![2,3,10,11].includes(type) || !([24,32].includes(bits) && (type===2 || type===10) || bits===8 && (type===3 || type===11))) throw Error('Unsupported TGA encoding');
    const bpp=bits/8, count=width*height, out=new Uint8Array(32+count*4), view=new DataView(out.buffer);
    out.set([65,69,82,56]); view.setUint32(4,1,true); view.setUint32(8,width,true); view.setUint32(12,height,true); view.setUint32(16,1,true); view.setBigUint64(24,BigInt(count*4),true);
    let input=18+bytes[0], pixel=0;
    function readPixel() {
        if(input+bpp>bytes.length) throw Error('Truncated TGA pixel');
        const value=bpp===1?[bytes[input],bytes[input],bytes[input],255]:[bytes[input+2],bytes[input+1],bytes[input],bpp===4?bytes[input+3]:255];
        input+=bpp; return value;
    }
    function put(value) {
        if(pixel>=count) throw Error('TGA run exceeds image');
        const x=pixel%width, y=Math.floor(pixel/width), dx=(descriptor&16)?width-1-x:x, dy=(descriptor&32)?y:height-1-y;
        out.set(value,32+(dy*width+dx)*4); ++pixel;
    }
    while(pixel<count) {
        if(type===2 || type===3) { put(readPixel()); continue; }
        if(input>=bytes.length) throw Error('Truncated TGA packet');
        const packet=bytes[input++], length=(packet&127)+1;
        if(length>count-pixel) throw Error('TGA packet exceeds image');
        if(packet&128) { const value=readPixel(); for(let i=0;i<length;++i) put(value); }
        else for(let i=0;i<length;++i) put(readPixel());
    }
    return out;
}
function prepareMesh(bytes, info, meshPath, files, capacity) {
    const skeleton=prepareSkeleton(meshPath,files,info.skinRef);
    const materials=prepareMaterials(meshPath,files,info.materialPaths);
    const src=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength), streams=[], primitives=[];
    const ranges=info.ranges.length?info.ranges:[{first:0,count:info.indices,material:0}];
    for(let s=0;s<ranges.length;++s) {
        const range=ranges[s], remap=new Map(), vertices=[], indices=new Uint32Array(range.count);
        if(!info.ranges.length) for(let i=0;i<info.vertices;++i) { remap.set(i,i); vertices.push(i); }
        for(let i=0;i<range.count;++i) {
            const at=info.offset+(range.first+i)*(info.type?4:2), original=info.type?src.getUint32(at,true):src.getUint16(at,true);
            if(!remap.has(original)) { remap.set(original,vertices.length); vertices.push(original); }
            indices[i]=remap.get(original);
        }
        const output=new Uint8Array(vertices.length*100), view=new DataView(output.buffer);
        for(let i=0;i<vertices.length;++i) {
            const from=72+vertices[i]*96, to=i*100;
            output.set(bytes.subarray(from,from+48),to);
            output.set(bytes.subarray(from+52,from+60),to+48);
            const color=src.getUint32(from+48,true);
            for(let c=0;c<3;++c) view.setFloat32(to+56+c*4,((color>>>(c*8))&255)/255,true);
            output.set(bytes.subarray(from+64,from+96),to+68);
        }
        primitives.push({materialIndex:range.material<materials.length?range.material:materials.length?0:-1, skinIndex:info.skinRef?0:-1, primitiveKey:info.keys[s]||'unidentified',vertexCount:vertices.length,indexCount:range.count});
        streams.push(output,new Uint8Array(indices.buffer));
    }
    const metadata=new TextEncoder().encode(JSON.stringify({...skeleton,materials,primitives,bounds:info.bounds}));
    const required=16+metadata.length+streams.reduce((sum,stream)=>sum+stream.length,0);
    if(required>capacity) throw Error('Prepared model exceeds admitted budget');
    const out=new Uint8Array(capacity), view=new DataView(out.buffer);
    out.set([65,69,80,50]); view.setUint32(4,metadata.length,true); view.setUint32(8,required-16-metadata.length,true);
    out.set(metadata,16); let offset=16+metadata.length;
    for(const stream of streams) { out.set(stream,offset); offset+=stream.length; }
    return out;
}
async function prepareImage(bytes, width, height) {
    const bitmap=await createImageBitmap(new Blob([bytes]), {premultiplyAlpha:'none', colorSpaceConversion:'none'});
    try {
        if(bitmap.width!==width || bitmap.height!==height) throw Error('Image dimensions disagree with header');
        const canvas=new OffscreenCanvas(width,height), ctx=canvas.getContext('2d',{willReadFrequently:true,alpha:true});
        if(!ctx) throw Error('Image decoder canvas unavailable');
        ctx.drawImage(bitmap,0,0);
        const pixels=ctx.getImageData(0,0,width,height).data;
        const out=new Uint8Array(32+pixels.length), v=new DataView(out.buffer);
        out.set([65,69,82,56]); v.setUint32(4,1,true); v.setUint32(8,width,true); v.setUint32(12,height,true); v.setUint32(16,1,true); v.setBigUint64(24,BigInt(pixels.length),true); out.set(pixels,32);
        return out;
    } finally { bitmap.close(); }
}
function mipSize(width,height) {
    let levels=0, pixels=0, w=width, h=height;
    do { pixels+=w*h*4; ++levels; if(w===1 && h===1) break; w=Math.max(1,Math.floor(w/2)); h=Math.max(1,Math.floor(h/2)); } while(true);
    return 20+2*(12*levels+pixels);
}
let mipModulePromise;
async function prepareMips(texture,width,height,expected) {
    const module=await (mipModulePromise ||= createAetherSceneParser({locateFile:name=>new URL(name,self.location.href).href}));
    const size=width*height*4, pointer=module._malloc(size);
    if(!pointer) throw Error('Mip worker allocation failed');
    try {
        module.HEAPU8.set(texture.subarray(32),pointer);
        if(!module._stream_parser_build_mips(pointer,size,width,height)) throw Error(module.UTF8ToString(module._stream_parser_error()));
        const length=module._stream_parser_size(), result=module._stream_parser_data();
        if(length!==expected || !result) throw Error('Invalid prepared mip length');
        return module.HEAPU8.slice(result,result+length);
    } finally { module._free(pointer); module._stream_parser_reset(); }
}
self.onmessage = async ({data}) => {
    if(data.type==='cancel') { const job=jobs.get(data.id); if(job) { job.controller.abort(); job.resume?.({admit:false}); } return; }
    if(data.type==='continue') { jobs.get(data.id)?.resume?.(data); return; }
    if(data.type!=='fetch' || jobs.has(data.id)) return;
    const controller=new AbortController(), job={controller,resume:null}; jobs.set(data.id,job);
    try {
        if(!Number.isSafeInteger(data.size) || data.size<0 || !/^[a-f0-9]{64}$/.test(data.sha256)) throw Error('Invalid streaming asset metadata');
        const response=await fetch(data.url,{signal:controller.signal,cache:'no-store'});
        if(!response.ok || !response.body) throw Error(`Streaming HTTP ${response.status}`);
        const declared=response.headers.get('content-length');
        if(declared!==null && (!/^\d+$/.test(declared) || Number(declared)!==data.size)) throw Error('Streaming content length mismatch');
        const bytes=new Uint8Array(data.size); let offset=0; const reader=response.body.getReader();
        try { for(;;) { const {done,value}=await reader.read(); if(done) break; if(value.byteLength>bytes.length-offset) { await reader.cancel(); throw Error('Streaming asset exceeds declared size'); } bytes.set(value,offset); offset+=value.byteLength; } } finally { reader.releaseLock(); }
        if(offset!==bytes.length) throw Error('Truncated streaming asset');
        const digest=new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));
        const hash=Array.from(digest,x=>x.toString(16).padStart(2,'0')).join('');
        if(hash!==data.sha256) throw Error('Streaming asset hash mismatch');
        let info=null, preparedSize=0, baseSize=0, mipsSize=0, kind='';
        if(data.path.endsWith('.mesh')) { kind='mesh'; info=meshInfo(bytes); const expanded=info.ranges.length?info.ranges.reduce((n,r)=>n+r.count,0):Math.max(info.indices,info.vertices); const text=info.keys.reduce((n,k)=>n+k.length,0)+info.skinRef.length+info.materialPaths.reduce((n,p)=>n+p.length,0); baseSize=16+104*expanded+8*(text+data.dependencyBytes)+info.ranges.length*256+65536; preparedSize=baseSize; }
        else if(/\.(png|jpe?g|bmp|webp|tga)$/i.test(data.path)) { kind='image'; const [width,height]=dimensions(bytes,data.path); if(!width || !height || width>4096 || height>4096) throw Error('Image dimensions exceed limits'); info={width,height}; baseSize=32+width*height*4; mipsSize=mipSize(width,height); preparedSize=baseSize+mipsSize; }
        else if(data.path.endsWith('.texture')) { const v=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength); if(bytes.length<32 || v.getUint32(0,true)!==0x38524541 || v.getUint32(4,true)!==1 || v.getUint32(16,true)!==1 || v.getUint32(20,true)!==0) throw Error('Invalid cooked texture'); const width=v.getUint32(8,true), height=v.getUint32(12,true); if(!width || !height || width>4096 || height>4096 || v.getBigUint64(24,true)!==BigInt(width*height*4) || bytes.length!==32+width*height*4) throw Error('Invalid cooked texture extent'); kind='cookedImage'; info={width,height}; mipsSize=mipSize(width,height); preparedSize=mipsSize; }
        let prepared=null, mips=null;
        if(kind) {
            if(!Number.isSafeInteger(preparedSize) || preparedSize>64*1024*1024+32 || mipsSize>32*1024*1024) throw Error('Prepared asset exceeds limits');
            self.postMessage({id:data.id,phase:'prepare',preparedSize,baseSize,mipsSize,kind});
            const continuation=await new Promise(resolve=>{job.resume=resolve; if(controller.signal.aborted) resolve({admit:false});}); job.resume=null;
            if(!continuation.admit || controller.signal.aborted) throw Error('Prepared asset budget rejected or cancelled');
            const files=new Map((continuation.files||[]).map(f=>[f.path,new Uint8Array(f.bytes)]));
            if(kind==='mesh') prepared=prepareMesh(bytes,info,data.path,files,baseSize);
            else {
                if(kind==='image') prepared=/\.tga$/i.test(data.path)?prepareTga(bytes,info.width,info.height):await prepareImage(bytes,info.width,info.height);
                mips=await prepareMips(prepared||bytes,info.width,info.height,mipsSize);
            }
        }
        if(controller.signal.aborted) throw Error('Cancelled');
        if(prepared || mips) { const transfer=[bytes.buffer]; if(prepared) transfer.push(prepared.buffer); if(mips) transfer.push(mips.buffer); self.postMessage({id:data.id,bytes:bytes.buffer,prepared:prepared?.buffer,mips:mips?.buffer,kind},transfer); }
        else self.postMessage({id:data.id,bytes:bytes.buffer},[bytes.buffer]);
    } catch(error) { self.postMessage({id:data.id,error:controller.signal.aborted?'Cancelled':String(error?.message||error)}); }
    finally { jobs.delete(data.id); }
};
