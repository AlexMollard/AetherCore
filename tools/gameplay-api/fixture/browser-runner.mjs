import { createServer } from 'node:http';
import { readFile, writeFile } from 'node:fs/promises';
import { resolve, sep, extname } from 'node:path';
import { createRequire } from 'node:module';

const args = Object.fromEntries(process.argv.slice(2).map(value => {
  const index = value.indexOf('=');
  return [value.slice(2, index), value.slice(index + 1)];
}));
const root = resolve(args.root);
const require = createRequire(import.meta.url);
const report = {schemaVersion: 1, status: 'failed', errors: []};
let browser;
const server = createServer(async (request, response) => {
  try {
    const file = resolve(root, '.' + new URL(request.url, 'http://localhost').pathname);
    if (!file.startsWith(root + sep)) { response.writeHead(403).end(); return; }
    const content = await readFile(file);
    response.setHeader('Content-Type', ({'.html': 'text/html', '.js': 'application/javascript', '.wasm': 'application/wasm'})[extname(file)] ?? 'application/octet-stream');
    response.end(content);
  } catch (_) { response.writeHead(404).end(); }
});
try {
  const {chromium} = require(args.playwright || 'playwright');
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  browser = await chromium.launch(args.executable ? {executablePath: args.executable, headless: true} : {channel: 'chrome', headless: true});
  report.browserVersion = browser.version();
  const page = await browser.newPage();
  page.on('pageerror', error => report.errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.address().port}/index.html`);
  await page.waitForFunction(() => globalThis.gameplayAbiResult !== undefined, undefined, {timeout: 60000});
  report.result = await page.evaluate(() => globalThis.gameplayAbiResult);
  if (report.result.status !== 'passed' || report.result.pointerBits !== 32 || report.result.layoutChecks < 80 || report.errors.length) {
    throw new Error('Browser gameplay ABI contract failed');
  }
  report.status = 'passed';
} catch (error) { report.error = error.stack; }
finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
  await writeFile(args.report, JSON.stringify(report, null, 2));
}
console.log(JSON.stringify(report));
if (report.status !== 'passed') process.exitCode = 1;
