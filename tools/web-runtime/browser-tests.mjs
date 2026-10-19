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
				   ({'.js' : 'text/javascript', '.html' : 'text/html', '.wasm' : 'application/wasm', '.json' : 'application/json'})[path.extname(file)] ||
					   'application/octet-stream',
			   'Cache-Control' : 'no-store'
		   })
			.end(await readFile(file));
	} catch {
		res.writeHead(404).end()
	}
});
let browser;
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
	const pins=JSON.parse(await readFile(new URL('../experiments/rust-webgpu/toolchain.json',import.meta.url),'utf8'));
	assert.equal(report.playwrightVersion,pins.playwright,'Browser qualification requires pinned Playwright');
	await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
	browser = await chromium.launch(
		{...(options.executable ? {executablePath : options.executable} : {channel : 'chrome'}), headless : true, args : [ '--enable-gpu' ]});
	report.browserVersion = browser.version();
	for (const name of ['sprite-input-resize-shutdown', 'unavailable', 'limits', 'device-loss', 'stop-during-init', 'stop-in-flight', 'layers',
						'batch-capacity', 'responsive', 'drop-owner-init', 'stop-after-upload']
			 .filter(name => !options.only || options.only === name)) {
		const item = {name, status : 'failed', console : [], pageErrors : []};
		report.cases.push(item);
		const page = await browser.newPage();
		page.on('console', m => item.console.push({type : m.type(), text : m.text()}));
		page.on('pageerror', e => item.pageErrors.push(e.message));
		try {
			let query = name === 'unavailable'		  ? '?forceNoAdapter=1'
						: name === 'limits'			  ? '?forceLimits=1'
						: name === 'stop-during-init' ? '?stopBeforeReady=1'
						: name === 'stop-in-flight'	  ? '?stopAfterSubmit=1'
						: name === 'layers'			  ? '?scene=layers'
						: name === 'batch-capacity'	  ? '?scene=capacity'
						: name === 'drop-owner-init'  ? '?dropOwner=1'
: name === 'stop-after-upload' ? '?stopAfterUpload=1'
													  : '';
			if (name !== 'responsive') {
				query += (query ? '&' : '?') + 'test=1';
			}
			await page.goto(`http://127.0.0.1:${server.address().port}/${query}`);
			if (name.startsWith('stop-') || name === 'drop-owner-init') {
				await page.waitForFunction(() => aetherRuntime.state === 'stopped', undefined, {timeout : 10000});
				const state = await page.evaluate(() => aetherRuntime);
				if (name !== 'drop-owner-init') {
					assert.equal(state.gameplayStopped, true);
				}
				assert.equal(state.diagnostics.length, 0, JSON.stringify(state));
				if (name === 'stop-after-upload') { assert.equal(state.uploadsCompleted,2); }
				if (name === 'stop-in-flight') {
					assert.ok(state.peakInFlight > 0);
					assert.equal(state.inFlight, 0);
					assert.ok(state.capture);
				}
			} else if (name === 'unavailable' || name === 'limits') {
				await page.waitForFunction(() => aetherRuntime.status === 'failed');
				const result = await page.evaluate(() => aetherRuntime);
				assert.ok(result.diagnostics.some(d => d.code === (name === 'limits' ? 'WEBGPU_LIMITS' : 'WEBGPU_UNAVAILABLE')), JSON.stringify(result));
				assert.equal(result.diagnostics.length, 1, JSON.stringify(result));
				item.result = result;
			} else {
				await page.waitForFunction(() => aetherRuntime.rendererReady || aetherRuntime.status === 'failed', undefined, {timeout : 30000});
				let state = await page.evaluate(() => aetherRuntime);
				assert.notEqual(state.status, 'failed', JSON.stringify(state));
				assert.ok(isHardwareAdapter(state.adapter), 'Hardware adapter identity required: ' + JSON.stringify(state.adapter));
				if (name === 'device-loss') {
					await page.evaluate(() => Module._aether_web_lose_device());
					await page.waitForFunction(() => aetherRuntime.terminalState === 'device-lost');
					assert.ok((await page.evaluate(() => aetherRuntime.diagnostics)).some(d => d.code === 'WEBGPU_DEVICE_LOST'));
				} else if (name === 'responsive') {
					let image = await capture(page);
					assert.equal(image.width, 512);
					await page.setViewportSize({width : 320, height : 640});
					await page.waitForFunction(() => Module.canvas.width === Math.round(Module.canvas.getBoundingClientRect().width));
					image = await capture(page);
					assert.equal(image.width, 288);
					item.resize = {width : image.width, height : image.height};
				} else if (name === 'layers') {
					const image = await capture(page);
					const near = (actual, expected) => actual.forEach((v, i) => assert.ok(Math.abs(v - expected[i]) <= 1, actual + ' != ' + expected));
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
					assert.ok(state.x > before.x + .2, JSON.stringify({x:state.x,ticks:state.ticks,before}));
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
			if ([ 'sprite-input-resize-shutdown', 'layers', 'batch-capacity', 'stop-during-init', 'stop-in-flight', 'responsive' ].includes(name)) {
				assert.equal(result.diagnostics.length, 0, JSON.stringify(result));
				assert.equal(item.console.filter(m => m.type === 'error').length, 0, JSON.stringify(item.console));
			}
			item.status = 'passed';
		} catch (error) {
			item.error = error.stack;
			console.log(name, error.message)
		} finally {
			await page.close()
		}
	}
	report.status = report.cases.every(c => c.status === 'passed') ? 'passed' : 'failed';
} catch (error) {
	report.error = error.stack
} finally {
	await browser?.close();
	server.closeAllConnections();
	await new Promise(resolve => server.close(resolve));
	await mkdir(path.dirname(reportPath), {recursive : true});
	await writeFile(reportPath, JSON.stringify(report, null, 2));
	console.log(JSON.stringify({status : report.status, cases : report.cases.map(({name, status, error}) => ({name, status, error})), error : report.error}));
	process.exitCode = report.status === 'passed' ? 0 : 1
}
