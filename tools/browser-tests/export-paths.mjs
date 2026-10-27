export function isExportPath(value) {
    return typeof value==='string' && value.length>0 && !/[:\\]/.test(value) &&
        value.split('/').every(part=>part.length>0 && part!=='.' && part!=='..');
}

// Read only the versioned pak index; the package SHA-256 is verified separately.
export function pakPaths(bytes) {
    if (bytes.length < 64 || bytes.toString('ascii',0,4)!=='AEPK' || bytes.readUInt32LE(4)!==2)
        throw Error('Invalid game.pak header');
    const count=bytes.readUInt32LE(8), offset=Number(bytes.readBigUInt64LE(16)), size=Number(bytes.readBigUInt64LE(24));
    if (count>262144 || offset!==64+40*count || !Number.isSafeInteger(size) || offset+size>bytes.length)
        throw Error('Invalid game.pak index');
    const paths=new Set();
    for(let i=0;i<count;i++) {
        const at=64+40*i, start=bytes.readUInt32LE(at), length=bytes.readUInt32LE(at+4);
        if(start+length>size) throw Error('Invalid game.pak path');
        const name=bytes.toString('utf8',offset+start,offset+start+length);
        if(paths.has(name)) throw Error('Duplicate game.pak path');
        paths.add(name);
    }
    return paths;
}

export function validate3dAssets(manifest, paths) {
    const assets=manifest.cookedAssets;
    if (!assets || !Array.isArray(assets.meshes) || !Array.isArray(assets.shaders))
        throw Error('3D cookedAssets meshes/shaders declarations missing');
    for (const [kind, extension] of [['meshes','.mesh'],['shaders','.wshader']])
        for (const name of assets[kind]) {
            if (!isExportPath(name) || !name.endsWith(extension) || !paths.has(name))
                throw Error(`Missing or invalid cooked ${kind} entry: ${name}`);
        }
}
