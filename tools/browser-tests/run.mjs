import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdir, readFile, realpath, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

import {isHardwareAdapter} from '../experiments/rust-webgpu/browser-checks.mjs';

import {stopTree} from './processes.mjs';
import {legacyCaseNames, qualify, requiredBridgeCases} from './report.mjs';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const options = Object.fromEntries(process.argv.slice(2).map(arg => {
    const split = arg.indexOf('=');
    if (split < 3)
	throw Error('Expected --name=value');
    return [ arg.slice(2, split), arg.slice(split + 1) ];
}));
const reportFile = path.resolve(options.report || 'logs/browser-report.json');
const artifactRoot = path.join(path.dirname(reportFile), 'browser-artifacts');
const root = path.resolve(options.root || 'build/webgpu-browser/web');
const productionRoot = path.resolve(options.production || 'build/webgpu-production/web');
const project = path.resolve(options.project || 'projects/AgentPipelineRust');
const timeout = Number(options.timeout || 240000);
const report = {
    schemaVersion : 1,
    operation : 'browser-test',
    target : 'web',
    profile : 'webgpu-2d-v1',
    language : 'rust',
    project,
    status : 'blocked',
    phase : 'preflight',
    bridgeVersion : 1,
    startedAt : new Date().toISOString(),
    environment : {os : os.platform(), release : os.release(), architecture : os.arch()},
    cases : [],
    diagnostics : [],
    artifacts : []
};
const started = performance.now();
let browser, server, legacyChild, cancelled = false, deadline, treeCleanup;
const cancel = () => {
    cancelled = true;
    if (legacyChild && !treeCleanup)
	treeCleanup = stopTree(legacyChild).then(() => true, error => {
	    diagnostic('BROWSER_CLEANUP', error);
	    return false;
	});
    void browser?.close();
};
process.once('SIGINT', cancel);
process.once('SIGTERM', cancel);
function diagnostic(code, error) { report.diagnostics.push({code, phase : report.phase, message : String(error.message || error)}); }
function boundedLegacy(args) {
    return new Promise((resolve, reject) => {
	legacyChild =
	    spawn(process.execPath, args,
		  {cwd : repo, windowsHide : true, detached : process.platform !== 'win32', stdio : [ 'ignore', 'pipe', 'pipe' ]});
	let output = '';
	legacyChild.stdout.on('data', bytes => { output = (output + bytes).slice(-1024 * 1024); });
	legacyChild.stderr.on('data', bytes => { output = (output + bytes).slice(-1024 * 1024); });
	legacyChild.once('error', reject);
	legacyChild.once('exit', async code => {
	    if (!cancelled)
		legacyChild = null;
	    await writeFile(path.join(artifactRoot, 'legacy.log'), output);
	    if (cancelled)
		reject(Error('Browser run cancelled or timed out'));
	    else
		resolve(code);
	});
    });
}
async function serve() {
    server = createServer(async (request, response) => {
	try {
	    const url = new URL(request.url, 'http://localhost');
	    if (url.pathname === '/oversize.pak') {
		response.writeHead(200, {'Content-Type' : 'application/octet-stream', 'Content-Length' : String(300 * 1024 * 1024)});
		response.write(Buffer.alloc(64));
		return;
	    }
	    if (url.pathname === '/favicon.ico') {
		response.writeHead(204).end();
		return;
	    }
	    const production = url.pathname.startsWith('/production/');
	    const base = production ? productionRoot : root;
	    const filename = path.resolve(base, '.' + (production ? url.pathname.slice('/production'.length) : url.pathname));
	    const target = (await stat(filename)).isDirectory() ? path.join(filename, 'index.html') : filename;
	    const relative = path.relative(await realpath(base), await realpath(target));
	    if (relative.startsWith('..') || path.isAbsolute(relative)) {
		response.writeHead(403).end();
		return;
	    }
	    const mime = {'.js' : 'text/javascript', '.html' : 'text/html', '.wasm' : 'application/wasm'}[path.extname(target)] ||
			 'application/octet-stream';
	    response.writeHead(200, {'Content-Type' : mime, 'Cache-Control' : 'no-store'}).end(await readFile(target));
	} catch {
	    response.writeHead(404).end();
	}
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
}
async function snapshot(page) {
    return page.evaluate(() => globalThis.aetherTest ? aetherTest.snapshot() : structuredClone(aetherRuntime));
}
try {
    await mkdir(artifactRoot, {recursive : true});
    if (!Number.isInteger(timeout) || timeout < 1000 || timeout > 1200000)
	throw Error('Timeout must be 1000..1200000 ms');
    deadline = setTimeout(cancel, timeout);
    if (project !== path.join(repo, 'projects/AgentPipelineRust'))
	throw Error('This regression fixture is AgentPipelineRust; use Web publishing for selected Rust or C# projects');
    const require = createRequire(import.meta.url);
    const module = options.playwright || process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
    const {chromium} = require(module);
    const pins = JSON.parse(await readFile(path.join(repo, 'tools/experiments/rust-webgpu/toolchain.json'), 'utf8'));
    report.toolchain = pins;
    report.environment.playwrightVersion = require(path.join(module, 'package.json')).version;
    assert.equal(report.environment.playwrightVersion, pins.playwright, 'Pinned Playwright required');
    if (!options.only) {
	report.phase = 'legacy-browser-regressions';
	const legacyFile = path.join(artifactRoot, 'legacy.json');
	const legacyArgs = [
	    path.join(repo, 'tools/web-runtime/browser-tests.mjs'), `--root=${root}`, `--report=${legacyFile}`, `--playwright=${module}`
	];
	if (options.executable)
	    legacyArgs.push(`--executable=${options.executable}`);
	const legacyCode = await boundedLegacy(legacyArgs);
	report.legacy = JSON.parse(await readFile(legacyFile, 'utf8'));
	report.artifacts.push(legacyFile);
	if (legacyCode !== 0 || report.legacy.status !== 'passed' || qualify(legacyCaseNames, report.legacy.cases) !== 'passed') {
	    throw Error('Required legacy browser checks failed or did not execute');
	}
    }
    await serve();
    report.phase = 'browser-launch';
    browser = await chromium.launch({
	...(options.executable ? {executablePath : options.executable} : {channel : 'chrome'}),
	headless : true,
	timeout : 15000,
	args : [ '--enable-gpu' ]
    });
    report.environment.browserVersion = browser.version();
    for (const name of requiredBridgeCases) {
	if (options.only && options.only !== name)
	    continue;
	if (cancelled)
	    throw Error('Browser run cancelled or timed out');
	const item = {name, status : 'failed', console : [], pageErrors : [], bridgeVersion : 1};
	const before = performance.now();
	report.cases.push(item);
	report.phase = name;
	const context = await browser.newContext();
	const page = await context.newPage();
	page.setDefaultTimeout(15000);
	page.on('console', message => item.console.push({type : message.type(), text : message.text()}));
	page.on('pageerror', error => item.pageErrors.push(error.message));
	try {
	    if (name === 'save-denied')
		await page.addInitScript(
		    () => Object.defineProperty(globalThis, 'indexedDB', {get() { throw new DOMException('Denied', 'SecurityError'); }}));
	    if (name === 'oversize-package')
		await page.route('**/engine.pak', route => route.continue({url : new URL('/oversize.pak', route.request().url()).href}));
	    if (name === 'fetch-failure')
		await page.route('**/game.pak', route => route.fulfill({status : 404, body : 'missing'}));
	    const production = name === 'production-boundary';
	    const query = production ? '/production/?reference=1&automation=1&forceNoAdapter=1&stopBeforeReady=1'
				     : `/?reference=1&automation=1${name === 'no-adapter' ? '&forceNoAdapter=1' : ''}`;
	    await page.goto(`http://127.0.0.1:${server.address().port}${query}`);
	    if (production) {
		await page.waitForFunction(() => aetherRuntime.rendererReady && aetherRuntime.scene === 'ReferenceStart');
		assert.equal(await page.evaluate(() => typeof globalThis.aetherTest), 'undefined');
		assert.equal(await page.evaluate(() => typeof Module._aether_web_lose_device), 'undefined');
		for (const filename of ['index.html', 'AetherGame.js', 'AetherGame.wasm']) {
		    const bytes = await readFile(path.join(productionRoot, filename));
		    for (const marker of ['aetherTestInternal', 'aetherTest', 'aether_web_lose_device', 'aether_browser_test_panic',
					  'test-bridge.js'])
			assert.equal(bytes.includes(Buffer.from(marker)), false, `${marker} leaked into production ${filename}`);
		}
		await assert.rejects(stat(path.join(productionRoot, 'test-bridge.js')), {code : 'ENOENT'});
	    } else if ([ 'no-adapter', 'fetch-failure', 'save-denied', 'oversize-package' ].includes(name)) {
		await page.waitForFunction(() => aetherRuntime.status === 'failed');
		const state = await snapshot(page);
		const code = {
		    'no-adapter' : 'WEBGPU_UNAVAILABLE',
		    'fetch-failure' : 'BROWSER_ASSETS',
		    'oversize-package' : 'BROWSER_ASSETS',
		    'save-denied' : 'BROWSER_STORAGE'
		}[name];
		assert.ok(state.diagnostics.some(d => d.code === code), JSON.stringify(state));
		assert.equal(state.ticks, 0);
		if (name === 'oversize-package')
		    await page.waitForFunction(() => aetherRuntime.gameplayStopped);
	    } else {
		await page.waitForFunction(() => aetherRuntime.scene === 'ReferenceStart' && aetherRuntime.fontReady);
		await page.locator('#start').click();
		await page.waitForFunction(() => aetherTest.readiness().ready && aetherRuntime.audioReady);
		await page.locator('#canvas').focus();
		assert.equal((await snapshot(page)).ticks, 0, 'Automation startup must not advance simulation');
		if (name === 'fixed-reference') {
		    await page.evaluate(() => {
			aetherTest.input({key : 32, down : true});
			return aetherTest.step(90);
		    });
		    item.state = await snapshot(page);
		    assert.equal(item.state.ticks, 90);
		    assert.equal(item.state.scene, 'ReferenceResult');
		    assert.equal(item.state.uiText, 'Saved score 60');
		    assert.equal(item.state.platform.committedWrites, 2);
		    assert.equal(item.state.platform.scheduledSounds, 1);
		    assert.ok(item.state.platform.peak > 0);
		    const image = await page.evaluate(() => aetherTest.capture());
		    let white = 0;
		    for (let i = 0; i < image.pixels.length; i += 4)
			if (image.pixels[i] > 180 && image.pixels[i + 1] > 180 && image.pixels[i + 2] > 180)
			    ++white;
		    assert.ok(white > 5, 'Tolerant glyph pixels must reach WebGPU');
		    item.whitePixels = white;
		    const fresh = await context.newPage();
		    try {
			await fresh.goto(page.url().replace('automation=1', 'automation=1&resume=1'));
			await fresh.waitForFunction(() => aetherRuntime.uiText === 'Saved score 60');
			item.freshPage = await snapshot(fresh);
		    } finally {
			await fresh.close();
		    }
		} else if (name === 'protocol-bounds') {
		    const results = await page.evaluate(() => {
			const errors = [];
			for (const command
				 of [() => aetherTest.step(0), () => aetherTest.step(601), () => aetherTest.input({key : 512, down : true}),
				    () => aetherTest.input({key : 32, down : true, eval : 'no'})]) {
			    try {
				command();
				errors.push(false);
			    } catch {
				errors.push(true);
			    }
			}
			const value = aetherTest.snapshot();
			value.ticks = 999;
			return {errors, ticks : aetherTest.snapshot().ticks};
		    });
		    assert.deepEqual(results, {errors : [ true, true, true, true ], ticks : 0});
		    await assert.rejects(page.evaluate(() => aetherTest.step(600, 1)), /timed out/);
		} else if (name === 'rust-panic') {
		    await page.evaluate(() => {
			globalThis.__panicResult = aetherTest.step(1).catch(e => String(e));
			aetherTest.panic();
		    });
		    await page.waitForFunction(() => aetherRuntime.status === 'failed');
		    assert.ok((await snapshot(page)).diagnostics.some(d => d.code === 'WASM_ABORT'));
		    assert.ok(item.console.some(message => /panic|Browser automation panic fixture/.test(message.text)));
		} else if (name === 'lost-device') {
		    await page.evaluate(() => aetherTest.loseDevice());
		    await page.waitForFunction(() => aetherRuntime.status === 'failed' && aetherRuntime.gameplayStopped);
		} else if (name === 'focus-reset') {
		    await page.evaluate(() => {
			aetherTest.input({key : 32, down : true});
			return aetherTest.step(3);
		    });
		    assert.equal((await snapshot(page)).uiText, 'Score 3');
		    await page.locator('#capture').focus();
		    await page.evaluate(() => aetherTest.step(3));
		    await page.locator('#canvas').focus();
		    await page.evaluate(() => aetherTest.step(3));
		    assert.equal((await snapshot(page)).uiText, 'Score 3');
		}
	    }
	    item.state = await snapshot(page);
	    if (item.state.adapter) {
		item.hardware = isHardwareAdapter(item.state.adapter);
		report.environment.gpu = item.state.adapter;
		report.environment.adapterKind = item.hardware ? 'hardware' : 'software-or-fallback';
		assert.ok(item.hardware, 'Hardware adapter required');
	    }
	    if (name !== 'rust-panic')
		assert.equal(item.pageErrors.length, 0, item.pageErrors.join(';'));
	    const expectedFailure =
		[ 'rust-panic', 'no-adapter', 'fetch-failure', 'save-denied', 'lost-device', 'oversize-package' ].includes(name);
	    if (!expectedFailure) {
		assert.equal(item.pageErrors.length, 0);
		assert.equal(item.state.diagnostics.length, 0, JSON.stringify(item.state));
		assert.equal(item.console.filter(m => m.type === 'error').length, 0);
	    }
	    const screenshot = path.join(artifactRoot, `${name}.png`);
	    await page.screenshot({path : screenshot});
	    item.screenshot = screenshot;
	    report.artifacts.push(screenshot);
	    item.status = 'passed';
	} catch (error) {
	    item.error = error.stack;
	    try {
		item.state = await snapshot(page);
	    } catch {
	    }
	} finally {
	    if (!item.screenshot) {
		try {
		    const screenshot = path.join(artifactRoot, `${name}.png`);
		    await page.screenshot({path : screenshot, timeout : 2000});
		    item.screenshot = screenshot;
		    report.artifacts.push(screenshot);
		} catch {
		}
	    }
	    item.durationMs = performance.now() - before;
	    await context.close();
	}
    }
    report.status = qualify(requiredBridgeCases, report.cases);
    report.phase = 'complete';
} catch (error) {
    diagnostic(cancelled							   ? 'BROWSER_CANCELLED'
	       : report.phase === 'preflight' || report.phase === 'browser-launch' ? 'BROWSER_UNAVAILABLE'
										   : 'BROWSER_VERIFICATION',
	       error);
    report.status = cancelled ? 'cancelled' : report.legacy?.cases?.some(item => item.status === 'failed') ? 'failed' : 'blocked';
} finally {
    clearTimeout(deadline);

    try {
	await browser?.close();
    } catch {
    }
    if (legacyChild && !treeCleanup)
	treeCleanup = stopTree(legacyChild).then(() => true, error => {
	    diagnostic('BROWSER_CLEANUP', error);
	    return false;
	});
    const treeClosed = treeCleanup ? await treeCleanup : !legacyChild;
    if (server) {
	server.closeAllConnections();
	await new Promise(resolve => server.close(resolve));
    }
    for (const name of requiredBridgeCases)
	if (!report.cases.some(item => item.name === name))
	    report.cases.push({name, status : 'blocked', reason : 'Required check did not execute', bridgeVersion : 1});
    report.durationMs = performance.now() - started;
    report.finishedAt = new Date().toISOString();
    report.cleanup = {
	browserClosed : !browser || !browser.isConnected(),
	serverClosed : !server || !server.listening,
	childClosed : treeClosed
    };
    if (Object.values(report.cleanup).some(value => !value)) {
	report.status = 'failed';
	diagnostic('BROWSER_CLEANUP', Error('Owned resources remain'));
    }
    await mkdir(path.dirname(reportFile), {recursive : true});
    await writeFile(reportFile, JSON.stringify(report, null, 2));
    console.log(JSON.stringify({
	status : report.status,
	report : reportFile,
	cases : report.cases.map(({name, status, error}) => ({name, status, error})),
	diagnostics : report.diagnostics
    }));
    process.exitCode = report.status === 'passed' ? 0 : 1;
}
