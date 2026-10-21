import assert from 'node:assert/strict';
import {mkdir, readFile, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';
import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

if (process.argv.includes('--help')) {
    console.log('Private authored 3D language qualifier: node authored-language-checks.mjs --packages=packages.json --report=logs/report.json [--playwright=module] [--executable=chrome]');
    process.exit(0);
}
const options = Object.fromEntries(process.argv.slice(2).map(arg => {
    const separator = arg.indexOf('=');
    if (separator < 3) throw Error('Arguments must use --name=value');
    return [arg.slice(2, separator), arg.slice(separator + 1)];
}));
const reportPath = path.resolve(options.report || 'logs/web3d/authored-languages/browser.json');
const report = {schemaVersion:1, scope:'private-3d-qualification', releasedProfile:'webgpu-2d-v1', status:'failed', cases:[]};
let browser, server;
const closeServer = async () => {
    if (server) {
        const owned = server;
        server = undefined;
        owned.closeAllConnections();
        await new Promise(resolve => owned.close(resolve));
    }
};
const cancel = () => { void browser?.close(); void closeServer(); };
process.once('SIGINT', cancel);
process.once('SIGTERM', cancel);

async function capture(page) {
    const requestedTick = await page.evaluate(() => {
        delete aetherRuntime.capture;
        Module._aether_web_capture();
        return aetherRuntime.ticks;
    });
    await page.waitForFunction(() => !!aetherRuntime.capture || aetherRuntime.status === 'failed');
    return page.evaluate(requestedTick => ({requestedTick, completedTick:aetherRuntime.ticks, image:aetherRuntime.capture}), requestedTick);
}

async function saveCapture(page, image, target) {
    const encoded = await page.evaluate(image => {
        const canvas = document.createElement('canvas');
        canvas.width = image.width;
        canvas.height = image.height;
        canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(image.pixels), image.width, image.height), 0, 0);
        return canvas.toDataURL('image/png').split(',')[1];
    }, image);
    await writeFile(target, Buffer.from(encoded, 'base64'));
}

function redCentroid(image) {
    assert.ok(image?.pixels, 'Actual GPU capture is required');
    let count = 0, sumX = 0, sumY = 0;
    for (let y = 0; y < image.height; ++y) {
        for (let x = 0; x < image.width; ++x) {
            const offset = (y * image.width + x) * 4;
            if (image.pixels[offset] > 240 && image.pixels[offset + 1] < 10 && image.pixels[offset + 2] < 10) {
                ++count; sumX += x; sumY += y;
            }
        }
    }
    assert.ok(count > 20, 'Authored red model must reach GPU readback');
    return {x:sumX / count, y:sumY / count, pixels:count};
}

try {
    assert.ok(options.packages, '--packages is required; use the Python driver to publish and privately stage both languages');
    const packages = JSON.parse(await readFile(options.packages, 'utf8'));
    assert.deepEqual(packages.map(item => item.language).sort(), ['csharp', 'rust']);
    const require = createRequire(import.meta.url);
    const module = options.playwright || process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
    const pins = JSON.parse(await readFile(new URL('../experiments/rust-webgpu/toolchain.json', import.meta.url), 'utf8'));
    assert.equal(require(path.join(module, 'package.json')).version, pins.playwright);
    const {chromium} = require(module);
    browser = await chromium.launch({...options.executable ? {executablePath:options.executable} : {channel:'chrome'}, headless:true, args:['--enable-gpu'], timeout:20000});
    report.browserVersion = browser.version();
    await mkdir(path.dirname(reportPath), {recursive:true});
    for (const entry of packages) {
        const item = {language:entry.language, scope:'private-3d-qualification', status:'failed', console:[], pageErrors:[]};
        report.cases.push(item);
        const root = path.resolve(entry.root);
        const qualification = await readFile(path.join(root, 'PRIVATE-3D-QUALIFICATION.txt'), 'utf8');
        assert.match(qualification, /Private qualification only/);
        const provenance = JSON.parse(await readFile(path.join(root, 'manifest.json'), 'utf8'));
        assert.equal(provenance.language, entry.language);
        assert.equal(provenance.profile, 'webgpu-2d-v1', 'Private qualification retains the original released-profile provenance');
        server = createServer(async (request, response) => {
            try {
                const url = new URL(request.url, 'http://localhost');
                if (url.pathname === '/favicon.ico') { response.writeHead(204).end(); return; }
                const target = path.resolve(root, '.' + decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname));
                const relative = path.relative(root, target);
                if (relative.startsWith('..') || path.isAbsolute(relative)) { response.writeHead(403).end(); return; }
                const mime = {'.js':'text/javascript', '.mjs':'text/javascript', '.html':'text/html', '.wasm':'application/wasm', '.json':'application/json'}[path.extname(target)] || 'application/octet-stream';
                response.writeHead(200, {'Content-Type':mime, 'Cache-Control':'no-store'}).end(await readFile(target));
            } catch { response.writeHead(404).end(); }
        });
        await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
        const context = await browser.newContext({viewport:{width:640, height:800}});
        const page = await context.newPage();
        page.setDefaultTimeout(30000);
        page.on('console', message => item.console.push({type:message.type(), text:message.text()}));
        page.on('pageerror', error => item.pageErrors.push(error.stack || error.message));
        try {
            await page.goto(`http://127.0.0.1:${server.address().port}/`);
            await page.waitForFunction(() => (aetherRuntime.rendererReady && aetherRuntime.assetsReady && aetherRuntime.storageReady && aetherRuntime.scene === 'BrowserModels') || aetherRuntime.status === 'failed');
            assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
            assert.equal(await page.evaluate(() => aetherProject.qualification), 'private-3d-qualification');
            item.adapter = await page.evaluate(() => aetherRuntime.adapter);
            assert.ok(isHardwareAdapter(item.adapter), 'Hardware WebGPU adapter required');
            assert.equal(await page.evaluate(() => typeof globalThis.aetherTest), 'undefined', 'Production language packages must remain bridge-free');
            assert.equal(await page.evaluate(() => aetherRuntime.ticks), 0, 'Simulation waits for a real Play gesture');
            const before = await capture(page);
            const beforeCenter = redCentroid(before.image);
            assert.equal(before.completedTick, 0);
            assert.ok(Math.abs(beforeCenter.x - (before.image.width - 1) / 2) < 2, 'Authored camera places initial model at the screen center');
            const beforePath = path.join(path.dirname(reportPath), entry.language + '-model-before.png');
            await saveCapture(page, before.image, beforePath);
            await page.locator('#start').click();
            await page.locator('#canvas').focus();
            assert.equal(await page.evaluate(() => document.activeElement?.id), 'canvas');
            await page.waitForFunction(() => aetherRuntime.ticks >= 30 || aetherRuntime.status === 'failed');
            const after = await capture(page);
            const afterCenter = redCentroid(after.image);
            assert.equal(after.image.width, before.image.width);
            assert.equal(after.image.height, before.image.height);
            const displacement = afterCenter.x - beforeCenter.x;
            // At z=-2 with vertical FOV 90, one world unit projects to height/4.
            // Readback is asynchronous, so the image's tick lies inside this
            // recorded request/completion interval, with one submitted frame slack.
            const pixelsPerTick = after.image.height / 4 / 60;
            const minimum = (after.requestedTick - before.completedTick - 1) * pixelsPerTick - 2;
            const maximum = (after.completedTick - before.requestedTick + 1) * pixelsPerTick + 2;
            assert.ok(displacement >= minimum && displacement <= maximum, JSON.stringify({displacement, minimum, maximum, afterRequested:after.requestedTick, afterCompleted:after.completedTick}));
            assert.ok(displacement > after.image.height / 12, 'Actual script-driven model moves visibly right');
            assert.ok(Math.abs(afterCenter.y - beforeCenter.y) < 2, 'Position mutation preserves model height');
            assert.ok(Math.abs(afterCenter.pixels - beforeCenter.pixels) <= beforeCenter.pixels * 0.05, 'Movement preserves model coverage and depth');
            const afterPath = path.join(path.dirname(reportPath), entry.language + '-model-after.png');
            await saveCapture(page, after.image, afterPath);
            item.movement = {before:beforeCenter, after:afterCenter, requestTick:after.requestedTick, completedTick:after.completedTick, pixelsPerTick, displacement, worldUnitsPerSecond:1};
            item.images = [beforePath, afterPath];
            item.actualTicks = after.completedTick;
            await page.evaluate(() => Module._aether_web_stop());
            await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0 && aetherRuntime.gameplayStopped);
            item.finalState = await page.evaluate(() => { const {capture, ...state} = aetherRuntime; return state; });
            assert.equal(item.finalState.diagnostics.length, 0, JSON.stringify(item.finalState.diagnostics));
            assert.equal(item.pageErrors.length, 0, item.pageErrors.join('\n'));
            assert.equal(item.console.filter(message => message.type === 'error').length, 0, JSON.stringify(item.console));
            item.status = 'passed';
        } catch (error) {
            item.error = error.stack;
            item.failureState = await page.evaluate(() => { const {capture, ...state} = aetherRuntime; return state; }).catch(() => null);
        } finally {
            await page.evaluate(() => Module?._aether_web_stop?.()).catch(() => {});
            await context.close();
            await closeServer();
        }
    }
    report.status = report.cases.length === 2 && report.cases.every(item => item.status === 'passed') ? 'passed' : 'failed';
} catch (error) {
    report.error = error.stack;
} finally {
    await browser?.close();
    await closeServer();
    report.cleanup = {browserClosed:!browser || !browser.isConnected(), serverClosed:!server};
    await mkdir(path.dirname(reportPath), {recursive:true});
    await writeFile(reportPath, JSON.stringify(report, null, 2));
    console.log(JSON.stringify({scope:report.scope, status:report.status, cases:report.cases.map(({language,status,error}) => ({language,status,error})), report:reportPath}));
    process.exitCode = report.status === 'passed' ? 0 : 1;
}
