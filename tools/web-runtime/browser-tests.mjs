import {legacyCaseNames} from '../browser-tests/report.mjs';
import assert from 'node:assert/strict';
import {mkdir, readFile, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';

import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

const require = createRequire(import.meta.url);
const options = Object.fromEntries(process.argv.slice(2).map(value => {
    const i = value.indexOf('=');
    if (i < 3)
	throw Error('--name=value required');
    return [ value.slice(2, i), value.slice(i + 1) ]
}));
const root = path.resolve(options.root || 'build/webgpu-browser/web');
const reportPath = path.resolve(options.report || 'logs/web-runtime/browser.json');
const playwrightModule = options.playwright || process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
const {chromium} = require(playwrightModule);
const report = {
    schemaVersion : 1,
    status : 'failed',
    cases : [],
    playwrightVersion : require(path.join(playwrightModule, 'package.json')).version
};
const server = createServer(async (req, res) => {
    try {
	const name = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
	if (name === '/__hardware_probe') {res.writeHead(200, {'Content-Type':'text/html'}).end('<!doctype html><title>WebGPU qualification</title>');return;}
	if (name === '/favicon.ico') {
	    res.writeHead(204).end();
	    return
	}
	const target = path.resolve(root, '.' + name);
	const relative = path.relative(root, target);
	if (relative.startsWith('..') || path.isAbsolute(relative)) {
	    res.writeHead(403).end();
	    return
	}
	const file = (await stat(target)).isDirectory() ? path.join(target, 'index.html') : target;
	res.writeHead(200, {
	       'Content-Type' :
		   ({'.js' : 'text/javascript',
		     '.html' : 'text/html',
		     '.wasm' : 'application/wasm',
		     '.json' : 'application/json'})[path.extname(file)] ||
		       'application/octet-stream',
	       'Cache-Control' : 'no-store'
	   })
	    .end(await readFile(file));
    } catch {
	res.writeHead(404).end()
    }
});
let browser;
let cancelled = false;
const cancel = () => { cancelled = true; void browser?.close(); };
process.once('SIGINT', cancel);process.once('SIGTERM', cancel);
async function capture(page) {
    await page.evaluate(() => {
	delete aetherRuntime.capture;
	Module._aether_web_capture()
    });
    await page.waitForFunction(() => !!aetherRuntime.capture, undefined, {timeout : 10000});
    return page.evaluate(() => aetherRuntime.capture)
}
function pixel(image, x, y) { return image.pixels.slice((y * image.width + x) * 4, (y * image.width + x) * 4 + 4) }
try {
    const pins = JSON.parse(await readFile(new URL('../experiments/rust-webgpu/toolchain.json', import.meta.url), 'utf8'));
    assert.equal(report.playwrightVersion, pins.playwright, 'Browser qualification requires pinned Playwright');
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    browser = await chromium.launch({
	...(options.executable ? {executablePath : options.executable} : {channel : 'chrome'}),
	headless : true,
	args : [ '--enable-gpu' ]
    });
    report.browserVersion = browser.version();
    const probeContext=await browser.newContext();
    try {
        const probe=await probeContext.newPage();
        await probe.goto(`http://127.0.0.1:${server.address().port}/__hardware_probe`);
        report.adapter=await probe.evaluate(async()=>{
            if(!navigator.gpu)return null;
            const adapter=await navigator.gpu.requestAdapter();if(!adapter)return null;
            const info=adapter.info;
            return {vendor:info.vendor,architecture:info.architecture,device:info.device,description:info.description,isFallbackAdapter:info.isFallbackAdapter??adapter.isFallbackAdapter??false};
        });
    } finally {await probeContext.close();}
    if(!isHardwareAdapter(report.adapter)) {
        report.status='blocked';
        report.cases=legacyCaseNames.map(name=>({name,status:'blocked',reason:'Required hardware WebGPU adapter is unavailable'}));
        throw Error('Required hardware WebGPU adapter is unavailable');
    }

    for (const name of legacyCaseNames
	     .filter(name => !options.only || options.only === name)) {
	if (cancelled) throw Error('Browser checks cancelled');
	const item = {name, status : 'failed', console : [], pageErrors : []};
	report.cases.push(item);
	const context = await browser.newContext();
	const page = await context.newPage();
	page.on('console', m => item.console.push({type : m.type(), text : m.text()}));
	page.on('pageerror', e => item.pageErrors.push(e.stack || e.message));
	try {
	    if (name === 'browser-key-latch')
		await page.addInitScript(() => {
		    const original = requestAnimationFrame;
		    globalThis.requestAnimationFrame = callback => original(now => {
			if (globalThis.__slowFrames) {
			    now = globalThis.__lastRaf + 1;
			}
			globalThis.__lastRaf = now;
			if (globalThis.__pulse) {
			    Module.canvas.dispatchEvent(new KeyboardEvent('keydown', {code : 'Space', key : ' ', bubbles : true}));
			}
			callback(now);
			if (globalThis.__pulse) {
			    window.dispatchEvent(new KeyboardEvent('keyup', {code : 'Space', key : ' ', bubbles : true}));
			    globalThis.__pulse = false;
			}
		    });
		});
	    if (name === 'browser-gamepad')
		await page.addInitScript(() => {
		    globalThis.__pad = {
			index : 0,
			id : 'Injected standard pad',
			mapping : 'standard',
			connected : true,
			timestamp : 1,
			axes : [ 0, 0, 0, 0 ],
			buttons : Array.from({length : 17}, () => ({pressed : false, touched : false, value : 0}))
		    };
		    Object.defineProperty(navigator, 'getGamepads', {value : () => __pad ? [ __pad ] : []});
		});
	    if (name === 'platform-storage-denied')
		await page.addInitScript(() =>
					     Object.defineProperty(globalThis, 'indexedDB',
								   {get() { throw new DOMException('Storage denied', 'SecurityError') }}));
	    if (name === 'assets-cancel') {
		await page.route('**/game.pak', async route => {
		    await new Promise(resolve => setTimeout(resolve, 250));
		    try {
			await route.fulfill({status : 200, body : await readFile(path.join(root, 'game.pak'))});
		    } catch {
		    }
		});
	    }
	    if (name.startsWith('assets-') && name !== 'assets-ready' && name !== 'assets-cancel') {
		await page.route('**/game.pak', async route => {
		    if (name === 'assets-missing') {
			await route.fulfill({status : 404, body : 'missing'});
			return;
		    }
		    const bytes = Buffer.from(await readFile(path.join(root, 'game.pak')));
		    if (name === 'assets-version')
			bytes.writeUInt32LE(999, 4);
		    else if (name === 'assets-corrupt')
			bytes[64] ^= 1;
		    else {
			const desktop = await readFile(path.resolve('build/webgpu-browser/test-fixtures/desktop-reference.pak'));
			await route.fulfill({status : 200, body : desktop});
			return;
		    }
		    await route.fulfill({status : 200, body : bytes});
		});
	    }
	    let query = name === 'unavailable'	       ? '?forceNoAdapter=1'
			: name === 'limits'	       ? '?forceLimits=1'
			: name === 'stop-during-init'  ? '?stopBeforeReady=1'
			: name === 'stop-in-flight'    ? '?stopAfterSubmit=1'
			: name === 'layers'	       ? '?scene=layers'
			: name === 'batch-capacity'    ? '?scene=capacity'
			: name === 'drop-owner-init'   ? '?dropOwner=1'
			: name === 'stop-after-upload' ? '?stopAfterUpload=1'
						       : '?fixture=1';
	    if (name.startsWith('platform-'))
		query = '?platform=1';
	    if (name === 'reference-gameplay' || name === 'browser-key-latch')
		query = '?reference=1';
	    if (name === 'browser-features' || name === 'browser-gamepad' || name === 'browser-ui-order')
		query = '?reference=1&scene=BrowserFeatures';
	    if (name.startsWith('invalid-'))
		query = '?reference=1&scene=' + ({
			    'invalid-font' : 'BrowserBadFont',
			    'invalid-scene-version' : 'BrowserBadVersion',
			    'invalid-atlas-version' : 'BrowserBadAtlas',
			    'invalid-audio-component' : 'BrowserBadAudio'
			})[name];
	    if (name !== 'responsive' && name !== 'browser-features' && name !== 'browser-gamepad' && name !== 'browser-ui-order') {
		query += (query ? '&' : '?') + 'test=1';
	    }
	    await page.goto(`http://127.0.0.1:${server.address().port}/${query}`);
	    if (name.startsWith('invalid-')) {
		await page.waitForFunction(() => aetherRuntime.status === 'failed' && aetherRuntime.gameplayStopped === true);
		const state = await page.evaluate(() => aetherRuntime);
		assert.equal(state.ticks, 0);
		assert.ok(state.diagnostics.length > 0);
		if (name === 'invalid-font')
		    assert.ok(state.diagnostics.some(d => /font/i.test(d.message)));
		item.status = 'passed';
		continue;
	    }
	    if (name === 'browser-key-latch') {
		await page.waitForFunction(() => aetherRuntime.scene === 'ReferenceStart');
		await page.locator('#start').click();
		await page.waitForFunction(() => aetherRuntime.audioReady);
		await page.locator('#canvas').focus();
		await page.evaluate(() => {
		    globalThis.__slowFrames = true;
		    globalThis.__pulse = true;
		});
		await page.waitForFunction(() => /^Score [1-9]/.test(aetherRuntime.uiText), undefined, {timeout : 5000});
		item.status = 'passed';
		continue;
	    }
	    if (name === 'browser-ui-order') {
		await page.waitForFunction(() => aetherRuntime.scene === 'BrowserFeatures');
		const image = await capture(page);
		assert.ok(pixel(image, 270, 20)[1] > 180, 'Child image draws over opaque parent');
		assert.ok(pixel(image, 340, 20)[0] > 180 && pixel(image, 340, 20)[2] > 180, 'Canvas sort bias wins over entity order');
		item.status = 'passed';
		continue;
	    }
	    if (name === 'browser-gamepad') {
		item.controllerSource = 'Injected navigator Gamepad API; no physical controller qualification';
		await page.waitForFunction(() => aetherRuntime.scene === 'BrowserFeatures');
		await page.locator('#start').click();
		await page.waitForFunction(() => aetherRuntime.audioReady);
		await page.locator('#canvas').click({position : {x : 400, y : 400}});
		await page.waitForFunction(() => aetherRuntime.gamepadConnected === true);
        const clickTick = await page.evaluate(() => aetherRuntime.ticks);
        await page.waitForFunction(tick => aetherRuntime.ticks > tick + 2, clickTick);
		await page.evaluate(() => {__pad.buttons[13] = { pressed: true, touched: true, value: 1 }});
		await page.waitForFunction(() => aetherRuntime.focusedName === 'Toggle');
		await page.evaluate(() => {__pad.buttons[13] = { pressed: false, touched: false, value: 0 }});
		await page.evaluate(() => { __pad.buttons[0] = {pressed : true, touched : true, value : 1}; });
		await page.waitForFunction(() => aetherRuntime.toggleOn === true && aetherRuntime.padA === true);
		await page.evaluate(() => {__pad.axes[0] = 0.8});
		await page.waitForFunction(() => aetherRuntime.padLeftX > 0.6);
		await page.evaluate(() => {
		    __pad.buttons[0] = {pressed : false, touched : false, value : 0};
		    __pad.axes[0] = 0;
		});
		await page.waitForFunction(() => aetherRuntime.padA === false);
		await page.evaluate(() => {__pad = null});
		await page.waitForFunction(() => aetherRuntime.gamepadConnected === false);
		item.status = 'passed';
		continue;
	    }
	    if (name === 'browser-focus-resume') {
		item.visibilityTestMode = 'Controlled DOM visibility events; real canvas focus transfer';
		await page.waitForFunction(() => aetherRuntime.frames > 1);
		await page.locator('#canvas').click();
		await page.keyboard.down('ArrowRight');
		await page.waitForFunction(() => aetherRuntime.x > 0.1);
		await page.locator('#capture').focus();
		const focused = await page.evaluate(() => ({ticks : aetherRuntime.ticks, x : aetherRuntime.x}));
		await page.waitForFunction(ticks => aetherRuntime.ticks > ticks + 3, focused.ticks);
		assert.ok(Math.abs(await page.evaluate(() => aetherRuntime.x) - focused.x) < 0.001);
		await page.evaluate(() => {
		    globalThis.__visibilityTest = true;
		    Object.defineProperty(document, 'hidden', {configurable : true, get : () => __visibilityTest});
		    document.dispatchEvent(new Event('visibilitychange'));
		});
		const paused = await page.evaluate(() => ({ticks : aetherRuntime.ticks, x : aetherRuntime.x}));
		await new Promise(resolve => setTimeout(resolve, 300));
		assert.equal(await page.evaluate(() => aetherRuntime.ticks), paused.ticks);
		await page.evaluate(() => {
		    __visibilityTest = false;
		    document.dispatchEvent(new Event('visibilitychange'));
		});
		await page.waitForFunction(ticks => aetherRuntime.ticks > ticks, paused.ticks);
		assert.ok(Math.abs(await page.evaluate(() => aetherRuntime.x) - paused.x) < 0.001);
		assert.ok(await page.evaluate(ticks => aetherRuntime.ticks - ticks < 10, paused.ticks));
		item.status = 'passed';
		continue;
	    }
	    if (name === 'browser-features') {
		await page.waitForFunction(() => aetherRuntime.scene === 'BrowserFeatures' || aetherRuntime.status === 'failed');
		assert.equal(await page.evaluate(() => aetherRuntime.scene), 'BrowserFeatures',
			     JSON.stringify(await page.evaluate(() => aetherRuntime)));
		await page.locator('#start').click();
		await page.waitForFunction(() => aetherRuntime.audioReady);
		await page.locator('#canvas').click({position : {x : 32, y : 20}});
		await page.waitForFunction(() => aetherRuntime.toggleOn === true, undefined, {timeout : 5000});
		await page.locator('#canvas').click({position : {x : 145, y : 68}});
		await page.waitForFunction(() => aetherRuntime.sliderValue > 0.75);
		// GPU readback completes asynchronously; sample actual pixels across a full animation cycle.
        let red, blue;
        for (let attempt = 0; attempt < 30 && (!red || !blue); ++attempt) {
            const image = await capture(page);
            const [r, g, b] = pixel(image, 171, 256);
            if (r > 180 && g < 60 && b < 60) red = image;
            if (b > 180 && r < 60 && g < 60) blue = image;
        }
        assert.ok(red && blue, 'Both animation colors reach GPU readback');
		assert.ok(await page.evaluate(() => aetherRuntime.tileSprites >= 4));
		assert.ok(pixel(blue, 128, 384)[0] > 180, 'Tile map reaches GPU readback');
		assert.ok(pixel(blue, 205, 20)[0] > 180, 'Authored UI image reaches GPU readback');
		item.status = 'passed';
		continue;
	    }
	    if (name === 'reference-gameplay') {
		await page.waitForFunction(() => aetherRuntime.scene === 'ReferenceStart' || aetherRuntime.status === 'failed');
		assert.equal(await page.evaluate(() => aetherRuntime.scene), 'ReferenceStart',
			     JSON.stringify(await page.evaluate(() => aetherRuntime)));
		await page.locator('#start').click();
		await page.waitForFunction(() => aetherRuntime.audioReady);
		await page.locator('#canvas').click();
		await page.keyboard.down('Space');
		await page.waitForFunction(() => aetherRuntime.scene === 'ReferenceResult' || aetherRuntime.status === 'failed');
		await page.keyboard.up('Space');
		const state = await page.evaluate(() => ({
						      runtime : aetherRuntime,
						      score : aetherPlatform.read('reference_score'),
						      label : aetherPlatform.read('reference_label')
						  }));
		assert.equal(state.runtime.scene, 'ReferenceResult', JSON.stringify(state));
		assert.match(state.score, /^[1-9]\d*,true,true$/);
		assert.equal(state.label, 'caf\u00e9\0agent');
		item.saveReadback = {score : state.score, label : state.label};
		assert.equal(state.runtime.platform.committedWrites, 2);
		assert.equal(state.runtime.platform.scheduledSounds, 1);
		assert.ok(state.runtime.platform.peak > 0.001);
		assert.ok(state.runtime.uiGlyphs > 0, 'UI glyphs must be submitted');
		const image = await capture(page);
		let white = 0;
		for (let i = 0; i < image.pixels.length; i += 4)
		    if (image.pixels[i] > 180 && image.pixels[i + 1] > 180 && image.pixels[i + 2] > 180)
			++white;
		assert.ok(white > 5, 'UI text must reach GPU readback');
		const fresh = await context.newPage();
		try {
		    await fresh.goto(page.url() + '&resume=1');
		    await fresh.waitForFunction(() => aetherRuntime.scene === 'ReferenceResult');
		    assert.match(await fresh.evaluate(() => aetherRuntime.uiText), /^Saved score [1-9]/);
		} finally {
		    await fresh.close();
		}
		item.status = 'passed';
		continue;
	    }
	    if (name.startsWith('platform-')) {
		if (name === 'platform-storage-denied') {
		    await page.waitForFunction(() => aetherRuntime.diagnostics.some(d => d.code === 'BROWSER_STORAGE'));
		    assert.equal(await page.evaluate(() => aetherRuntime.storageReady), false);
		} else {
		    await page.waitForFunction(() => aetherRuntime.storageReady);
		    if (name === 'platform-save-reload') {
			const pending = await page.evaluate(() => {
			    aetherPlatform.stage('reference_label', 'caf\u00e9\0agent');
			    aetherPlatform.stage('reference_score', '20,true,true');
			    aetherPlatform.flush();
			    return {busy : aetherPlatform.busy, committed : aetherPlatform.report.committedWrites};
			});
			assert.deepEqual(pending, {busy : true, committed : 0});
			await page.waitForFunction(() => aetherPlatform.report.committedWrites === 2);
			const fresh = await page.context().newPage();
			try {
			    await fresh.goto(page.url());
			    await fresh.waitForFunction(() => aetherRuntime.storageReady);
			    assert.equal(await fresh.evaluate(() => aetherPlatform.read('reference_label')), 'caf\u00e9\0agent');
			    assert.equal(await fresh.evaluate(() => aetherPlatform.read('reference_score')), '20,true,true');
			} finally {
			    await fresh.close();
			}
		    } else if (name === 'platform-storage-quota') {
			assert.equal(await page.evaluate(async () => {
			    IDBObjectStore.prototype.put =
				function() { throw new DOMException('Injected exhausted quota', 'QuotaExceededError') };
			    aetherPlatform.stage('quota', 'uncommitted');
			    return aetherPlatform.flush();
			}),
				     false);
			assert.equal(await page.evaluate(() => aetherPlatform.report.committedWrites), 0);
			assert.ok(
			    (await page.evaluate(() => aetherRuntime.diagnostics)).some(d => d.message.includes('QuotaExceededError')));
		    } else {
			await page.waitForFunction(() => !!aetherPlatform.audioBytes);
			await page.locator('#start').click();
			await page.waitForFunction(() => aetherRuntime.audioReady);
			assert.equal(await page.evaluate(() => aetherPlatform.play('project://assets/audio/reference.wav')), true);
			await page.waitForFunction(() => aetherPlatform.report.peak > 0.001);
			assert.equal(await page.evaluate(() => aetherPlatform.play('project://assets/audio/secondary.wav')), true);
			await page.evaluate(() => aetherPlatform.audio.suspend());
			await page.locator('#start').click();
			await page.waitForFunction(() => aetherPlatform.audio.state === 'running');
			assert.equal(await page.evaluate(() => aetherPlatform.report.scheduledSounds), 2);
		    }
		}
		item.status = 'passed';
		continue;
	    }
	    if (name.startsWith('assets-')) {
		if (name === 'assets-ready') {
		    await page.waitForFunction(() => aetherRuntime.assetsReady === true && aetherRuntime.frames > 0);
		    assert.ok((await page.evaluate(() => aetherRuntime.assetBytes)) > 0);
		} else if (name === 'assets-cancel') {
		    await page.waitForFunction(() => Object.hasOwn(aetherRuntime, 'assetsReady'));
		    await page.evaluate(() => Module._aether_web_stop());
		    await page.waitForFunction(() => aetherRuntime.state === 'stopped');
		} else {
		    await page.waitForFunction(() => aetherRuntime.status === 'failed');
		    const state = await page.evaluate(() => aetherRuntime);
		    assert.equal(state.assetsReady, false);
		    assert.equal(state.ticks, 0);
		    assert.ok(state.diagnostics.some(d => d.code === 'BROWSER_ASSETS'));
		}
		item.status = 'passed';
		continue;
	    }
	    if (name.startsWith('stop-') || name === 'drop-owner-init') {
		await page.waitForFunction(() => aetherRuntime.state === 'stopped', undefined, {timeout : 10000});
		const state = await page.evaluate(() => aetherRuntime);
		if (name !== 'drop-owner-init') {
		    assert.equal(state.gameplayStopped, true);
		}
		assert.equal(state.diagnostics.length, 0, JSON.stringify(state));
		if (name === 'stop-after-upload') {
		    assert.equal(state.uploadsCompleted, 2);
		}
		if (name === 'stop-in-flight') {
		    assert.ok(state.peakInFlight > 0);
		    assert.equal(state.inFlight, 0);
		    assert.ok(state.capture);
		}
	    } else if (name === 'unavailable' || name === 'limits') {
		await page.waitForFunction(() => aetherRuntime.status === 'failed');
		const result = await page.evaluate(() => aetherRuntime);
		assert.ok(result.diagnostics.some(d => d.code === (name === 'limits' ? 'WEBGPU_LIMITS' : 'WEBGPU_UNAVAILABLE')),
			  JSON.stringify(result));
		assert.equal(result.diagnostics.length, 1, JSON.stringify(result));
		item.result = result;
	    } else {
		await page.waitForFunction(() => aetherRuntime.rendererReady || aetherRuntime.status === 'failed', undefined,
					   {timeout : 30000});
		let state = await page.evaluate(() => aetherRuntime);
		assert.notEqual(state.status, 'failed', JSON.stringify(state));
		assert.ok(isHardwareAdapter(state.adapter), 'Hardware adapter identity required: ' + JSON.stringify(state.adapter));
		if (name === 'device-loss') {
		    await page.evaluate(() => Module._aether_web_lose_device());
		    await page.waitForFunction(() => aetherRuntime.terminalState === 'device-lost');
		    assert.ok((await page.evaluate(() => aetherRuntime.diagnostics)).some(d => d.code === 'WEBGPU_DEVICE_LOST'));
		} else if (name === 'responsive') {
		    let image = await capture(page);
		    assert.equal(image.width, await page.locator('#canvas').evaluate(canvas=>Math.round(canvas.getBoundingClientRect().width)));
		    const initialWidth=image.width;
		    await page.setViewportSize({width : 320, height : 640});
		    await page.waitForFunction(() => Module.canvas.width === Math.round(Module.canvas.getBoundingClientRect().width));
		    image = await capture(page);
		    assert.equal(image.width, await page.locator('#canvas').evaluate(canvas=>Math.round(canvas.getBoundingClientRect().width)));
		    assert.ok(image.width < initialWidth, 'Backing surface shrinks with the viewport');
		    assert.ok(image.width <= 320, 'Canvas stays inside the narrow viewport');
		    assert.equal(image.height, await page.locator('#canvas').evaluate(canvas=>Math.round(canvas.getBoundingClientRect().height)));
		    item.resize = {width : image.width, height : image.height};
		} else if (name === 'layers') {
		    const image = await capture(page);
		    const near = (actual, expected) =>
			actual.forEach((v, i) => assert.ok(Math.abs(v - expected[i]) <= 1, actual + ' != ' + expected));
		    near(pixel(image, 8, 8), [ 128, 128, 0, 255 ]);
		    near(pixel(image, 32, 32), [ 0, 128, 128, 255 ]);
		    item.pixelChecks = 2;
		} else if (name === 'batch-capacity') {
		    const image = await capture(page);
		    assert.deepEqual(pixel(image, 32, 32), [ 0, 0, 255, 255 ]);
		    const state = await page.evaluate(() => aetherRuntime);
		    assert.equal(state.instances, 1026);
		    assert.equal(state.draws, 3);
		    item.instances = state.instances;
		    item.draws = state.draws;
		} else {
		    const image = await capture(page);
		    const expected = [ [ 255, 0, 0, 255 ], [ 0, 255, 0, 255 ], [ 0, 0, 255, 255 ], [ 255, 255, 0, 255 ] ];
		    for (let y = 0; y < 64; ++y) {
			for (let x = 0; x < 64; ++x) {
			    assert.deepEqual(pixel(image, x, y), expected[(y >= 32 ? 2 : 0) + (x >= 32 ? 1 : 0)], `pixel ${x},${y}`)
			}
		    }
		    item.pixelChecks = 4096;
		    await page.locator('#canvas').click();
		    await page.keyboard.down('ArrowRight');
		    const before = await page.evaluate(() => ({x : aetherRuntime.x, ticks : aetherRuntime.ticks}));
		    await page.waitForFunction(ticks => aetherRuntime.ticks >= ticks + 15, before.ticks);
		    await page.keyboard.up('ArrowRight');
		    state = await page.evaluate(() => aetherRuntime);
		    assert.ok(state.x > before.x + .2, JSON.stringify({x : state.x, ticks : state.ticks, before}));
		    item.movement = {before : before.x, after : state.x};
		    const moved = await capture(page);
		    assert.deepEqual(pixel(moved, 0, 32), [ 0, 0, 0, 255 ]);
		    await page.evaluate(() => Module._aether_web_resize(96, 48));
		    const resized = await capture(page);
		    assert.equal(resized.width, 96);
		    assert.equal(resized.height, 48);
		    item.resize = {width : resized.width, height : resized.height};
		    await page.screenshot({path : path.join(path.dirname(reportPath), 'runtime.png')});
		}
	    }
	    if (!['unavailable', 'limits', 'device-loss', 'drop-owner-init'].includes(name)) {
		await page.evaluate(() => Module._aether_web_stop());
	    }
	    await page.waitForFunction(() => aetherRuntime.state === 'stopped', undefined, {timeout : 10000});
	    const {capture : unused, ...result} = await page.evaluate(() => aetherRuntime);
	    item.result = result;
	    if (name !== 'drop-owner-init') {
		assert.equal(result.gameplayStopped, true);
	    }
	    assert.equal(item.pageErrors.length, 0, item.pageErrors.join(';'));
	    if ([ 'sprite-input-resize-shutdown', 'layers', 'batch-capacity', 'stop-during-init', 'stop-in-flight', 'responsive' ].includes(
		    name)) {
		assert.equal(result.diagnostics.length, 0, JSON.stringify(result));
		assert.equal(item.console.filter(m => m.type === 'error').length, 0, JSON.stringify(item.console));
	    }
	    item.status = 'passed';
	} catch (error) {
	    item.error = error.stack;
	    try {
		item.failureState = await page.evaluate(() => {
		    const {capture, ...state} = aetherRuntime;
		    return state
		});
	    } catch {
	    }
	    console.log(name, error.message, JSON.stringify(item.failureState))
	} finally {
	    try {
		const state = await page.evaluate(() => {
		    const {capture, ...result} = aetherRuntime;
		    return result
		});
		item.result = state;
		if (item.status === 'passed') {
		    assert.equal(item.pageErrors.length, 0, item.pageErrors.join(';'));
		    const expectedFailure = [
			'unavailable', 'limits', 'device-loss', 'assets-missing', 'assets-corrupt', 'assets-version', 'assets-target',
			'platform-storage-denied', 'platform-storage-quota'
		    ].includes(name) || name.startsWith('invalid-');
		    if (!expectedFailure) {
			assert.equal(state.diagnostics.length, 0, JSON.stringify(state));
			assert.equal(item.console.filter(m => m.type === 'error').length, 0, JSON.stringify(item.console));
		    }
		    if (state.adapter)
			assert.ok(isHardwareAdapter(state.adapter), 'Hardware adapter required');
		}
	    } catch (error) {
		item.status = 'failed';
		item.error = error.stack;
	    }
	    await context.close()
	}
    }
    report.status = report.cases.every(c => c.status === 'passed') ? 'passed' : 'failed';
} catch (error) {
    if(!browser)report.status='blocked';
    report.error = error.stack
} finally {
    await browser?.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
    await mkdir(path.dirname(reportPath), {recursive : true});
    await writeFile(reportPath, JSON.stringify(report, null, 2));
    console.log(JSON.stringify(
	{status : report.status, cases : report.cases.map(({name, status, error}) => ({name, status, error})), error : report.error}));
    process.exitCode = report.status === 'passed' ? 0 : 1
}
