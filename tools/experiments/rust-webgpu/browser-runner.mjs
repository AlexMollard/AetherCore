import { createServer } from 'node:http';
import { readFile, stat, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';
import { isHardwareAdapter, validateResult } from './browser-checks.mjs';

const require = createRequire(import.meta.url);
const options = Object.fromEntries(process.argv.slice(2).map((arg) => {
	const index = arg.indexOf('=');
	if (!arg.startsWith('--') || index < 3) {
		throw new Error('Arguments use --name=value');
	}
	return [arg.slice(2, index), arg.slice(index + 1)];
}));
const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const root = path.resolve(options.root || path.join(repoRoot, 'artifacts/rust-webgpu'));
const reportPath = path.resolve(options.report || path.join(repoRoot, 'logs/rust-webgpu/browser-report.json'));
const screenshotPath = path.join(path.dirname(reportPath), 'webgpu-sprite.png');
const timeout = Number(options.timeout || 60000);
const report = { schemaVersion: 1, status: 'failed', phase: 'browser-feasibility', cases: [], artifacts: { screenshot: screenshotPath }, startedAt: new Date().toISOString() };
let browser;
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.wgsl': 'text/plain', '.json': 'application/json' };
const server = createServer(async (request, response) => {
	try {
		const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
		if (pathname === '/favicon.ico') {
			response.writeHead(204).end();
			return;
		}
		const requested = path.resolve(root, '.' + pathname);
		const relative = path.relative(root, requested);
		if (relative.startsWith('..') || path.isAbsolute(relative)) {
			response.writeHead(403).end('Outside artifact root');
			return;
		}
		const file = (await stat(requested)).isDirectory() ? path.join(requested, 'index.html') : requested;
		response.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
		response.end(await readFile(file));
	} catch {
		response.writeHead(404).end('Artifact not found');
	}
});

try {
	if (!Number.isFinite(timeout) || timeout <= 0) {
		throw new Error('Timeout must be a positive number');
	}
	await mkdir(path.dirname(reportPath), { recursive: true });
	const playwrightModule = options.playwright || process.env.AETHER_PLAYWRIGHT_MODULE || 'playwright';
	const playwright = require(playwrightModule);
	report.playwrightVersion = require(path.join(playwrightModule, 'package.json')).version;
	report.nodeVersion = process.version;
	report.platform = process.platform;
	await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
	const baseUrl = `http://127.0.0.1:${server.address().port}`;
	browser = await playwright.chromium.launch({
		...(options.executable ? { executablePath: options.executable } : { channel: 'chrome' }),
		headless: true,
		args: ['--enable-gpu'],
	});
	report.browserVersion = browser.version();
	const context = await browser.newContext({ viewport: { width: 640, height: 480 } });
	const cases = [
		{ name: 'rust-cpp-wasm', url: '/interop/wasm/index.html' },
		{ name: 'cpp-library-box2d', url: '/cpp/cpp-probe.html' },
		{ name: 'webgpu-textured-sprite', url: '/webgpu/index.html', requireHardware: true },
		{ name: 'webgpu-unavailable', url: '/webgpu/index.html?forceNoAdapter=1', expectedCode: 'WEBGPU_UNAVAILABLE' },
		{ name: 'webgpu-invalid-shader', url: '/webgpu/index.html?invalidShader=1', expectedCode: 'WEBGPU_SHADER', requireShaderError: true },
	].filter((entry) => !options.only || options.only.split(',').includes(entry.name));
	if (!cases.length) { throw new Error('No requested test cases matched'); }
	for (const entry of cases) {
		console.log(JSON.stringify({ case: entry.name, phase: 'starting' }));
		const page = await context.newPage();
		const item = { name: entry.name, status: 'failed', console: [], pageErrors: [] };
		report.cases.push(item);
		page.on('console', (message) => item.console.push({ type: message.type(), text: message.text() }));
		page.on('pageerror', (error) => item.pageErrors.push(error.message));
		const started = performance.now();
		try {
			await page.goto(baseUrl + entry.url, { timeout, waitUntil: 'load' });
			await page.waitForFunction(() => ['passed', 'failed'].includes(globalThis.feasibilityResult?.status), undefined, { timeout });
			if (entry.requireShaderError) {
				await page.waitForFunction(() => globalThis.feasibilityResult?.shader?.errors > 0, undefined, { timeout });
			}
			item.result = await page.evaluate(() => globalThis.feasibilityResult);
			console.log(JSON.stringify({ case: entry.name, phase: 'result', status: item.result.status }));
			validateResult(item.result, entry.expectedCode);
			if (item.pageErrors.length) {
				throw new Error('Uncaught page errors: ' + item.pageErrors.join('; '));
			}
			if (!entry.expectedCode && item.console.some((message) => message.type === 'error')) {
				throw new Error('Unexpected browser console error');
			}
			if (entry.requireHardware) {
				item.adapter = item.result.adapter;
				if (!isHardwareAdapter(item.adapter)) {
					throw new Error('Real-GPU qualification unavailable: ' + JSON.stringify(item.adapter));
				}
				await page.waitForFunction(() => document.getElementById('result')?.textContent.includes('"status": "passed"'), undefined, { timeout: 5000 });
				await page.screenshot({ path: screenshotPath, timeout: 15000 });
			}
			item.status = 'passed';
		} catch (error) {
			item.error = error.message;
		} finally {
			item.elapsedMs = Math.round(performance.now() - started);
			await page.close();
		}
	}
	report.status = report.cases.every((item) => item.status === 'passed') ? 'passed' : 'failed';
} catch (error) {
	report.error = error.message;
} finally {
	await browser?.close();
	server.closeAllConnections();
	await new Promise((resolve) => server.close(resolve));
	report.finishedAt = new Date().toISOString();
	await mkdir(path.dirname(reportPath), { recursive: true });
	await writeFile(reportPath, JSON.stringify(report, null, 2) + '\r\n');
	console.log(JSON.stringify({ status: report.status, report: reportPath, cases: report.cases.map(({ name, status, error }) => ({ name, status, error })), error: report.error }));
	process.exitCode = report.status === 'passed' ? 0 : 1;
}
