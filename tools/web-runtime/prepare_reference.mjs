import {cp, mkdir, readFile, rm, writeFile} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {crc32, deflateSync, zstdCompressSync} from 'node:zlib';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const project = path.resolve(process.argv[2]);
const build = path.resolve(process.argv[3]);
const directory = path.join(build, 'reference-project');
if (!build.startsWith(path.join(repo, 'build') + path.sep) || path.dirname(directory) !== build ||
    !project.startsWith(path.join(repo, 'projects') + path.sep)) {
    throw Error('Reference fixture paths must remain inside repository projects/build');
}
// Only this generated staging directory is replaced; original projects are read-only.
await rm(directory, {recursive : true, force : true});
await mkdir(directory, {recursive : true});
for (const name of ['assets', 'scenes', 'ProjectSettings.toml'])
    await cp(path.join(project, name), path.join(directory, name), {recursive : true});
await cp(path.join(project, 'assets/audio/reference.wav'), path.join(directory, 'assets/audio/secondary.wav'));
await cp(path.join(repo, 'projects/EmberRun/assets/ember'), path.join(directory, 'assets/ember'), {recursive: true});
await cp(path.join(repo, 'projects/EmberRun/scenes/EmberRun.scene.toml'), path.join(directory, 'scenes/EmberRun.scene.toml'));
const fixture = path.join(repo, 'resources/web/reference');
await cp(fixture, directory, {recursive : true});
// The Human skeleton/skinned model (two clips) feeds the 3D ragdoll and animation parity fixtures; it is
// copied from its source project so the 10 MB buffer is not duplicated in the repository again.
await mkdir(path.join(directory, 'assets/models/Human'), {recursive : true});
for (const file of ['Human.gltf', 'buffer.bin', 'walk.bin'])
    await cp(path.join(repo, 'projects/HumanDemo/assets/models/Human', file), path.join(directory, 'assets/models/Human', file));
const u32 = value => {
    const bytes = Buffer.alloc(4);
    bytes.writeUInt32LE(value);
    return bytes
};
const i32 = value => {
    const bytes = Buffer.alloc(4);
    bytes.writeInt32LE(value);
    return bytes
};
const f32 = value => {
    const bytes = Buffer.alloc(4);
    bytes.writeFloatLE(value);
    return bytes
};
const str = value => {
    const bytes = Buffer.from(value);
    return Buffer.concat([ u32(bytes.length), bytes ])
};
const u64 = value => {
    const bytes = Buffer.alloc(8);
    bytes.writeBigUInt64LE(BigInt(value));
    return bytes
};

// Standard PNG and ATLM assets consumed by the existing host packer / engine loaders.
const chunk = (name, bytes) => {
    const type = Buffer.from(name);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([ type, bytes ])));
    const size = Buffer.alloc(4);
    size.writeUInt32BE(bytes.length);
    return Buffer.concat([ size, type, bytes, crc ]);
};
// Small deterministic material textures use the same standard PNG/chunk path.
async function materialPng(name, width, height, rgba) {
    const materialHeader = Buffer.alloc(13);
    materialHeader.writeUInt32BE(width);
    materialHeader.writeUInt32BE(height, 4);
    materialHeader[8] = 8;
    materialHeader[9] = 6;
    const materialPixels = Buffer.alloc(height * (1 + width * 4));
    for (let y = 0; y < height; ++y)
        for (let x = 0; x < width; ++x)
            Buffer.from(rgba(x, y)).copy(materialPixels, y * (1 + width * 4) + 1 + x * 4);
    await writeFile(path.join(directory, 'assets/textures', name), Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', materialHeader),
        chunk('IDAT', deflateSync(materialPixels)), chunk('IEND', Buffer.alloc(0))
    ]));
}
await materialPng('browser-material-gray.png', 1, 1, () => [128, 128, 128, 255]);
await cp(path.join(directory, 'assets/textures/browser-material-gray.png'), path.join(directory, 'assets/materials/browser-gray/albedo.png'));
await materialPng('browser-material-alpha.png', 1, 1, () => [255, 255, 255, 128]);
await materialPng('browser-material-mask.png', 2, 2, (x, y) => [255, 255, 255, (x + y) % 2 ? 255 : 0]);
await materialPng('browser-shadow-art.png', 4, 4, (x, y) => [255, 255, 255, y < 2 ? 255 : 0]);
await materialPng('browser-normal-right.png', 1, 1, () => [255, 128, 128, 255]);
await materialPng('browser-orm.png', 1, 1, () => [64, 128, 255, 255]);
// One white column per eight texels: level 3 is encoded gray 32. Base-only
// sampling aliases to black at these fixture pixel centers.
await materialPng('browser-material-mips.png', 64, 64, x => {
    const value = x % 8 === 7 ? 255 : 0;
    return [value, value, value, 255];
});
const header = Buffer.alloc(13);
header.writeUInt32BE(16);
header.writeUInt32BE(8, 4);
header[8] = 8;
header[9] = 6;
const pixels = Buffer.alloc(8 * (1 + 16 * 4));
for (let y = 0; y < 8; ++y)
    for (let x = 0; x < 16; ++x) {
	const offset = y * 65 + 1 + x * 4;
	pixels[offset + (x < 8 ? 0 : 2)] = 255;
	pixels[offset + 3] = 255;
    }
await writeFile(path.join(directory, 'assets/textures/browser-frames.png'), Buffer.concat([
    Buffer.from([ 137, 80, 78, 71, 13, 10, 26, 10 ]), chunk('IHDR', header), chunk('IDAT', deflateSync(pixels)),
    chunk('IEND', Buffer.alloc(0))
]));
const cells = Buffer.alloc(32 * 32 * 4);
for (let x = 0; x < 4; ++x)
    cells.writeUInt32LE(1, x * 4);
const compressed = zstdCompressSync(cells);
await writeFile(path.join(directory, 'assets/browser/test.tilemap'), Buffer.concat([
    Buffer.from('ATLM'),
    u32(1),
    str('project://assets/browser/test.tileset.toml'),
    f32(1),
    u32(1),
    u64(100),
    u32(1),
    str('Ground'),
    Buffer.from([ 1, 1 ]),
    f32(1),
    f32(1),
    f32(1),
    f32(1),
    f32(1),
    i32(0),
    i32(0),
    u32(1),
    i32(0),
    i32(0),
    u32(compressed.length),
    compressed
]));
const shadowCells = Buffer.alloc(32 * 32 * 4);
shadowCells.writeUInt32LE(1, 0);
const shadowCompressed = zstdCompressSync(shadowCells);
await writeFile(path.join(directory, 'assets/browser/shadow.tilemap'), Buffer.concat([
    Buffer.from('ATLM'), u32(1), str('project://assets/browser/shadow.tileset.toml'), f32(2),
    u32(1), u64(100), u32(1), str('Shadow artwork'), Buffer.from([1, 1]),
    f32(1), f32(1), f32(1), f32(1), f32(1), i32(0), i32(0), u32(1), i32(0), i32(0),
    u32(shadowCompressed.length), shadowCompressed
]));
console.log(`Prepared browser reference fixture in ${directory}`);
