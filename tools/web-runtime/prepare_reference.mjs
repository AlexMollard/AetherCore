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
const fixture = path.join(repo, 'resources/web/reference');
await cp(fixture, directory, {recursive : true});
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
console.log(`Prepared browser reference fixture in ${directory}`);
