'use strict';

// Match GltfAsset.cpp's ResolveRelativeVfsPath, including its mount-relative
// treatment of references that do not contain "..".
function materialResolvePath(baseFilePath, reference) {
    if (reference.startsWith('data:') || reference.includes('://')) return reference;
    const separator = baseFilePath.indexOf('://');
    if (separator < 0) throw Error('Invalid material base VFS path: ' + baseFilePath);
    const mount = baseFilePath.slice(0, separator);
    const base = baseFilePath.slice(separator + 3).replace(/\\/g, '/');
    let path = reference.replace(/\\/g, '/');
    const absolute = path.startsWith('/') || /^[A-Za-z]:\//.test(path);
    if (absolute || reference.includes('..')) {
        if (!absolute) {
            const slash = base.lastIndexOf('/');
            path = (slash < 0 ? '' : base.slice(0, slash + 1)) + path;
        }
    }
    const drive = /^[A-Za-z]:/.exec(path)?.[0] || '';
    const rooted = path.slice(drive.length).startsWith('/');
    const trailing = path.endsWith('/');
    const parts = [];
    for (const part of path.slice(drive.length).split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (parts.length && parts[parts.length - 1] !== '..') parts.pop();
            else if (!rooted) parts.push(part);
        } else parts.push(part);
    }
    let normalized = drive + (rooted ? '/' : '') + parts.join('/');
    if (trailing && normalized && !normalized.endsWith('/')) normalized += '/';
    if (!normalized && path && !rooted) normalized = '.';
    return mount + '://' + normalized;
}

function prepareMaterials(meshPath, files, materialPaths) {
    if (!(files instanceof Map) || !Array.isArray(materialPaths) || materialPaths.length > 65536)
        throw Error('Invalid material inputs');
    const decoder = new TextDecoder('utf-8', {fatal: true});
    return materialPaths.map(name => {
        if (typeof name !== 'string' || !name) throw Error('Invalid material reference');
        const path = materialResolvePath(meshPath, name);
        const bytes = files.get(path);
        if (!(bytes instanceof Uint8Array)) throw Error('Missing material: ' + path);
        if (bytes.length < 64) throw Error('Truncated material header: ' + path);
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const version = view.getUint32(4, true);
        if (view.getUint32(0, true) !== 0x4c54414d || (version !== 1 && version !== 2))
            throw Error('Invalid material header: ' + path);
        const readFloat = offset => {
            const value = view.getFloat32(offset, true);
            if (!Number.isFinite(value)) throw Error('Non-finite material value: ' + path);
            return value;
        };
        const flags = bytes[54];
        const mat = {
            name,
            baseColorFactor: [readFloat(8), readFloat(12), readFloat(16), readFloat(20)],
            metallicFactor: readFloat(24), roughnessFactor: readFloat(28),
            emissiveFactor: [readFloat(32), readFloat(36), readFloat(40)], alphaCutoff: readFloat(44),
            doubleSided: bytes[48] !== 0, alphaBlend: bytes[49] !== 0,
            additiveBlend: bytes[49] === 2, alphaMask: bytes[50] !== 0,
            modulateVertexColor: bytes[52] !== 0, foliage: bytes[53] !== 0,
            bakedLighting: !!(flags & 1), sky: !!(flags & 2), lightShaft: !!(flags & 4),
            gem: !!(flags & 8), sheen: !!(flags & 16), water: !!(flags & 32),
            character: !!(flags & 64), objectLit: bytes[55] !== 0,
            uvScroll: [readFloat(56), readFloat(60)],
            baseColorTexture: -1, metallicRoughnessTexture: -1, normalTexture: -1,
            occlusionTexture: -1, emissiveTexture: -1,
            albedoPath: '', normalPath: '', metallicRoughnessPath: '',
            occlusionPath: '', emissivePath: '', shaderVfsPath: ''
        };
        let offset = 64;
        const readString = () => {
            if (offset + 2 > bytes.length) throw Error('Truncated material string: ' + path);
            const length = view.getUint16(offset, true);
            offset += 2;
            if (length > bytes.length - offset) throw Error('Truncated material string: ' + path);
            const value = decoder.decode(bytes.subarray(offset, offset + length));
            offset += length;
            return value;
        };
        const slots = ['albedoPath', 'metallicRoughnessPath', 'normalPath', 'occlusionPath', 'emissivePath'];
        const count = bytes[51];
        if (count * 3 > bytes.length - offset) throw Error('Truncated material texture records: ' + path);
        for (let i = 0; i < count; ++i) {
            const type = bytes[offset++];
            const reference = readString();
            if (type >= slots.length || !reference) throw Error('Invalid material texture record: ' + path);
            mat[slots[type]] = materialResolvePath(path, reference);
        }
        // Older cooked materials may omit the shader trailer altogether.
        if (offset < bytes.length) mat.shaderVfsPath = readString();
        if (offset !== bytes.length) throw Error('Trailing material data: ' + path);
        return mat;
    });
}
