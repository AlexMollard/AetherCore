// Classic worker helper for the packed skeleton and animation dependencies of a mesh.
function prepareSkeleton(meshPath, files, skinRef) {
    const identity = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    const node = (name, parentIndex, meshIndex, skinIndex) => ({
        name, parentIndex, children: [], meshIndex, skinIndex,
        translation: [0, 0, 0], rotation: [0, 0, 0, 1], scale: [1, 1, 1],
        matrix: identity(), hasMatrix: false
    });
    const fail = message => { throw new Error(message); };
    const normalize = path => {
        const absolute = path.startsWith('/');
        const parts = [];
        for (const part of path.replace(/\\/g, '/').split('/')) {
            if (!part || part === '.') continue;
            if (part === '..') {
                if (parts.length && parts[parts.length - 1] !== '..') parts.pop();
                else if (!absolute) parts.push(part);
            } else parts.push(part);
        }
        return (absolute ? '/' : '') + parts.join('/');
    };
    const split = path => {
        const at = path.indexOf('://');
        if (at < 0) fail('Invalid VFS path: ' + path);
        return [path.slice(0, at), path.slice(at + 3)];
    };
    const resolve = (base, relative) => {
        if (relative.startsWith('data:') || relative.includes('://')) return relative;
        const [mount, path] = split(base);
        const rel = relative.replace(/\\/g, '/');
        const parent = path.slice(0, Math.max(0, path.lastIndexOf('/') + 1));
        return mount + '://' + normalize(!rel.startsWith('/') && !relative.includes('..') ? rel : (rel.startsWith('/') ? rel : parent + rel));
    };
    const get = path => {
        const bytes = files.get(path);
        if (!(bytes instanceof Uint8Array)) fail('Missing dependency: ' + path);
        return bytes;
    };
    const reader = (bytes, label) => {
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        let pos = 0;
        const take = n => { if (!Number.isSafeInteger(n) || n < 0 || n > view.byteLength - pos) fail('Truncated ' + label); const p = pos; pos += n; return p; };
        return {
            remaining: () => view.byteLength - pos,
            u8: () => view.getUint8(take(1)),
            u16: () => view.getUint16(take(2), true),
            u32: () => view.getUint32(take(4), true),
            i32: () => view.getInt32(take(4), true),
            f32: () => { const v = view.getFloat32(take(4), true); if (!Number.isFinite(v)) fail('Non-finite float in ' + label); return v; },
            skip: n => { take(n); },
            str: n => new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(take(n), pos)),
            magic: expected => { const p = take(4); for (let i = 0; i < 4; ++i) if (view.getUint8(p + i) !== expected.charCodeAt(i)) fail('Invalid ' + label + ' magic'); },
            end: () => { if (pos !== view.byteLength) fail('Trailing data in ' + label); }
        };
    };
    const invert = m => {
        const rows = Array.from({length: 4}, (_, r) => Array.from({length: 8}, (_, c) => c < 4 ? m[c * 4 + r] : +(c - 4 === r)));
        for (let c = 0; c < 4; ++c) {
            let pivot = c;
            for (let r = c + 1; r < 4; ++r) if (Math.abs(rows[r][c]) > Math.abs(rows[pivot][c])) pivot = r;
            if (rows[pivot][c] === 0) fail('Singular inverse bind matrix');
            [rows[c], rows[pivot]] = [rows[pivot], rows[c]];
            const d = rows[c][c];
            for (let j = 0; j < 8; ++j) rows[c][j] /= d;
            for (let r = 0; r < 4; ++r) if (r !== c) {
                const factor = rows[r][c];
                for (let j = 0; j < 8; ++j) rows[r][j] -= factor * rows[c][j];
            }
        }
        const out = Array.from({length: 16}, (_, i) => rows[i % 4][4 + Math.floor(i / 4)]);
        if (!out.every(Number.isFinite)) fail('Invalid inverse bind matrix');
        return out;
    };
    const multiply = (a, b) => Array.from({length: 16}, (_, i) => {
        const r = i % 4, c = Math.floor(i / 4);
        return a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    });
    const decompose = (m, n) => {
        n.translation = m.slice(12, 15);
        n.scale = [0, 1, 2].map(c => Math.hypot(m[c * 4], m[c * 4 + 1], m[c * 4 + 2]));
        const a = m.slice();
        for (let c = 0; c < 3; ++c) if (n.scale[c] > 0.0001) for (let r = 0; r < 3; ++r) a[c * 4 + r] /= n.scale[c];
        // GLM quat_cast chooses the largest of w, x, y and z for numerical stability.
        const candidates = [a[0] + a[5] + a[10], a[0] - a[5] - a[10], a[5] - a[0] - a[10], a[10] - a[0] - a[5]];
        let k = 0;
        for (let i = 1; i < 4; ++i) if (candidates[i] > candidates[k]) k = i;
        const biggest = Math.sqrt(candidates[k] + 1) * 0.5, mult = 0.25 / biggest;
        if (k === 0) n.rotation = [(a[6] - a[9]) * mult, (a[8] - a[2]) * mult, (a[1] - a[4]) * mult, biggest];
        else if (k === 1) n.rotation = [biggest, (a[1] + a[4]) * mult, (a[8] + a[2]) * mult, (a[6] - a[9]) * mult];
        else if (k === 2) n.rotation = [(a[1] + a[4]) * mult, biggest, (a[6] + a[9]) * mult, (a[8] - a[2]) * mult];
        else n.rotation = [(a[8] + a[2]) * mult, (a[6] + a[9]) * mult, biggest, (a[1] - a[4]) * mult];
        if (![...n.translation, ...n.scale, ...n.rotation].every(Number.isFinite)) fail('Invalid bone transform');
    };
    const nodes = [node('root', -1, 0, skinRef ? 0 : -1)], skins = [], animations = [];
    if (skinRef) {
        let path = resolve(meshPath, skinRef);
        if (!files.has(path)) {
            const slash = path.lastIndexOf('/'), dot = path.lastIndexOf('.');
            if (dot > slash) path = path.slice(0, dot) + '.skel';
        }
        const r = reader(get(path), path);
        r.magic('SKEL');
        if (r.u32() !== 1) fail('Unsupported skeleton version');
        const count = r.u32(), nameLength = r.u16();
        r.skip(8);
        if (count > 4096 || count * 70 + nameLength > r.remaining()) fail('Invalid bone count');
        r.skip(nameLength);
        const bones = [];
        for (let i = 0; i < count; ++i) {
            const name = r.str(r.u16()), parent = r.i32();
            if (parent >= count || parent === i) fail('Invalid bone parent');
            const inverseBind = Array.from({length: 16}, () => r.f32());
            bones.push({name, parent, inverseBind, world: invert(inverseBind)});
        }
        r.end();
        const states = new Uint8Array(count);
        for (let i = 0; i < count; ++i) {
            let current = i, chain = [];
            while (current >= 0 && states[current] === 0) { states[current] = 1; chain.push(current); current = bones[current].parent; }
            if (current >= 0 && states[current] === 1) fail('Cyclic bone hierarchy');
            for (const index of chain) states[index] = 2;
        }
        const skin = {name: path.slice(path.lastIndexOf('/') + 1), skeletonRoot: -1, joints: [], inverseBindMatrices: []};
        for (let i = 0; i < count; ++i) {
            const bone = bones[i], n = node(bone.name, bone.parent < 0 ? 0 : bone.parent + 1, -1, -1);
            decompose(bone.parent < 0 ? bone.world : multiply(bones[bone.parent].inverseBind, bone.world), n);
            nodes.push(n); skin.joints.push(i + 1); skin.inverseBindMatrices.push(bone.inverseBind);
        }
        skins.push(skin);
    }
    const [mount, relative] = split(meshPath);
    const slash = relative.lastIndexOf('/'), dot = relative.lastIndexOf('.');
    const animSetPath = mount + '://' + relative.slice(0, dot > slash ? dot : relative.length) + '.animset';
    if (files.has(animSetPath)) {
        const set = reader(get(animSetPath), animSetPath);
        set.magic('ASET');
        if (set.u32() !== 1) fail('Unsupported animation set version');
        const count = set.u32(); set.skip(12);
        if (count > 65536 || count * 2 > set.remaining()) fail('Invalid animation count');
        const names = new Map();
        nodes.forEach((n, i) => { if (n.name && !names.has(n.name)) names.set(n.name, i); });
        for (let i = 0; i < count; ++i) {
            const path = mount + '://' + set.str(set.u16());
            const r = reader(get(path), path);
            r.magic('ANIM');
            const version = r.u32(), channelsCount = r.u32(), nameLength = r.u16();
            if (version < 1 || version > 4 || channelsCount > 65536) fail('Invalid animation header');
            const flags = version >= 3 ? r.u16() : 0;
            if (flags & ~1) fail('Invalid animation flags');
            const animation = {name: r.str(nameLength), channels: [], rootLocked: false};
            if (channelsCount * 12 > r.remaining()) fail('Invalid channel count');
            for (let c = 0; c < channelsCount; ++c) {
                let nodeIndex = r.u32();
                const target = r.u8(), interpolation = r.u8(); r.skip(2);
                const keyCount = r.u32();
                if (target > 3 || interpolation > 2 || keyCount === 0 || keyCount > (1 << 20)) fail('Invalid animation channel');
                const boneName = flags & 1 ? r.str(r.u16()) : '';
                const valueCount = keyCount * (interpolation === 2 ? 3 : 1);
                if (keyCount * 4 + valueCount * 16 > r.remaining()) fail('Truncated animation keys');
                const times = []; let previous = -1;
                for (let k = 0; k < keyCount; ++k) { const time = r.f32(); if (time < 0 || time <= previous) fail('Invalid animation time'); times.push(time); previous = time; }
                const values = Array.from({length: valueCount}, () => [r.f32(), r.f32(), r.f32(), r.f32()]);
                if (skins.length && skins[0].joints.length) nodeIndex += 1;
                if (boneName && names.has(boneName)) nodeIndex = names.get(boneName);
                animation.channels.push({nodeIndex, boneName: '', path: target, interpolation, times, values});
            }
            r.end();
            if (animation.name) animations.push(animation);
        }
        set.end();
    }
    return {nodes, skins, animations};
}
