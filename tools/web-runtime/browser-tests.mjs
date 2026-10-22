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
const lightingScenes = {
    'lighting-default':'BrowserDefaultLighting',
    'lighting-solid':'BrowserSolid', 'lighting-gradient':'BrowserGradient', 'lighting-sky':'BrowserSky',
    'lighting-hdr':'BrowserHdr', 'lighting-sun':'BrowserSun', 'lighting-point':'BrowserPoint',
    'lighting-spot':'BrowserSpot', 'lighting-pbr-maps':'BrowserPbrMaps', 'lighting-2d-alpha':'BrowserLight2D',
    'lighting-nonfinite':'BrowserBadLight', 'lighting-budget':'BrowserLight2D'
};
for (const kind of ['Sun','Point','Spot','Mask','Skin']) lightingScenes['lighting-shadow-'+kind.toLowerCase()]='BrowserShadow'+kind+'On';
lightingScenes['lighting-shadow-no-cast']='BrowserShadowMaskOn';
const effectScenes = {
    'effects-object-lights':'BrowserObjectLights', 'effects-baked':'BrowserBaked',
    'effects-gem':'BrowserGem', 'effects-sheen':'BrowserSheen', 'effects-character':'BrowserCharacter',
    'effects-fog':'BrowserFog', 'effects-clouds':'BrowserClouds', 'effects-physical-sky':'BrowserPhysicalSky',
    'effects-darkness':'BrowserDarkness', 'effects-dof':'BrowserDofOn',
    'effects-particles-2d':'BrowserParticles2D', 'effects-particles-3d':'BrowserParticles3D'
};
// Independent CPU evaluation of the native Hill ACES fit and display gamma, for neutral HDR probes.
function neutralAces(value) {
    const fit = value => (value * (value + 0.0245786) - 0.000090537) / Math.max(value * (0.983729 * value + 0.432951) + 0.238081, 1e-6);
    return Math.round(255 * Math.pow(Math.min(1, Math.max(0, fit(value))), 1 / 2.2));
}
try {
    await mkdir(path.dirname(reportPath), {recursive : true});
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
			: name === 'mesh-depth-resize' ? '?scene=mesh3d'
			: name === 'mesh-invalid-frame' ? '?scene=mesh3d&badMesh=1'
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
	    if (name === 'authored-models' || name === 'capture-backpressure')
		query = '?reference=1&scene=BrowserModels';
	    if (name === 'authored-dependencies')
		query = '?reference=1&scene=BrowserDependencies';
	    if (name === 'authored-materials')
		query = '?reference=1&scene=BrowserMaterials';
	    if (name === 'authored-skin')
		query = '?reference=1&scene=BrowserSkin&automation=1';
	    if (lightingScenes[name])
		query = '?reference=1&scene=' + lightingScenes[name] + '&automation=1';
	    if (effectScenes[name])
		query = '?reference=1&scene=' + effectScenes[name] + '&automation=1';
	    if (name === 'browser-features' || name === 'browser-gamepad' || name === 'browser-ui-order')
		query = '?reference=1&scene=BrowserFeatures';
	    if (name.startsWith('invalid-'))
		query = '?reference=1&scene=' + ({
			    'invalid-font' : 'BrowserBadFont',
			    'invalid-scene-version' : 'BrowserBadVersion',
			    'invalid-atlas-version' : 'BrowserBadAtlas',
			    'invalid-audio-component' : 'BrowserBadAudio',
			    'invalid-particle-rate' : 'BrowserBadParticleRate',
			    'invalid-particle-capacity' : 'BrowserBadParticleCapacity',
			    'invalid-custom-pass-shader' : 'BrowserBadCustomPassShader',
			    'invalid-custom-pass-budget' : 'BrowserBadCustomPassBudget'
			})[name];
	    if (name !== 'responsive' && name !== 'browser-features' && name !== 'browser-gamepad' && name !== 'browser-ui-order') {
		query += (query ? '&' : '?') + 'test=1';
	    }
	    await page.goto(`http://127.0.0.1:${server.address().port}/${query}`);
	    if (effectScenes[name]) {
		const readEffect = async scene => {
		    await page.waitForFunction(scene => aetherRuntime.scene === scene || aetherRuntime.status === 'failed',scene);
		    assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
		    await page.locator('#start').click();
		    await page.locator('#canvas').focus();
		    await page.waitForFunction(()=>aetherRuntime.frames>2 || aetherRuntime.status==='failed');
		    assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
		    await page.evaluate(()=>aetherTest.step(10));
		    const image=await page.evaluate(()=>aetherTest.capture());
		    assert.deepEqual(await page.evaluate(()=>aetherRuntime.diagnostics),[]);
		    await page.evaluate(()=>aetherTest.stop());
		    await page.waitForFunction(()=>aetherRuntime.state==='stopped' && aetherRuntime.inFlight===0);
		    return image;
		};
		const on=await readEffect(effectScenes[name]);
		await writeFile(path.join(path.dirname(reportPath),name+'-on-readback.json'),JSON.stringify(on));
		if(name.includes('particles')) {
		    const background=pixel(on,4,4);
		    let active=0;
		    for(let y=16;y<48;y++) for(let x=16;x<48;x++)
		        if(pixel(on,x,y).slice(0,3).some((v,c)=>v-background[c]>25)) active++;
		    assert.ok(active>=8,`Seeded tick10 particle burst must cover at least eight actual pixels: ${active}`);
		    item.effect={activeParticlePixels:active,seed:123456,ticks:10};
		} else {
		    const control=effectScenes[name].replace(/On$/,'')+'Off';
		    await page.goto(`http://127.0.0.1:${server.address().port}/?reference=1&scene=${control}&automation=1&test=1`);
		    const off=await readEffect(control);
		    await writeFile(path.join(path.dirname(reportPath),name+'-off-readback.json'),JSON.stringify(off));
		    const material=['effects-object-lights','effects-baked','effects-gem','effects-sheen','effects-character'].includes(name);
		    const sky=['effects-clouds','effects-physical-sky','effects-dof'].includes(name);
		    const lo=material?23:sky?4:12,hi=material?41:sky?60:52;
		    let changed=0,peak=0;
		    for(let y=lo;y<hi;y++) for(let x=lo;x<hi;x++) {
		        const a=pixel(on,x,y),b=pixel(off,x,y);
		        const delta=Math.max(...a.slice(0,3).map((v,c)=>Math.abs(v-b[c])));
		        if(delta>3) changed++;
		        peak=Math.max(peak,delta);
		    }
		    assert.ok(changed>=4,`Authored effect must change at least four pixels against its single-field control: ${changed}, peak ${peak}`);
		    if(material) assert.ok(Math.max(...pixel(on,32,32).slice(0,3))>40,'Specialized surface must remain visible');
		    if(name==='effects-darkness') assert.ok(pixel(off,20,32)[0]-pixel(on,20,32)[0]>15,'Authored darkness volume must darken its interior');
		    if(name==='effects-dof') {
		        assert.ok(pixel(on,61,32)[0]-pixel(off,61,32)[0]>3,'Far defocus must spread energy beyond the square silhouette');
		        assert.ok(Math.abs(pixel(on,15,32)[0]-pixel(off,15,32)[0])<8,'Near focus2m retains its sharp square center');
		    }
		    item.effect={changedPixels:changed,peakDifference:peak,control,ticks:10};
		}
		item.scope='Fixed authored camera, seed where applicable, ten explicit simulation ticks; actual GPU effect/control readback and clean retirement';
		item.status='passed';
		continue;
	    }
	    if (lightingScenes[name]) {
		if (name === 'lighting-nonfinite') {
		    await page.waitForFunction(() => aetherRuntime.status === 'failed' && (aetherRuntime.inFlight || 0) === 0);
		    const state = await page.evaluate(() => aetherRuntime);
		    assert.equal(state.frames || 0, 0, 'Invalid light never reaches GPU submission');
		    assert.ok(state.diagnostics.some(d => /finite|invalid/i.test(d.message)), JSON.stringify(state.diagnostics));
		    item.scope = 'Authored NaN sun intensity rejected before frame allocation/submission';
		    item.status = 'passed';
		    continue;
		}
		await page.waitForFunction(scene => aetherRuntime.scene === scene || aetherRuntime.status === 'failed', lightingScenes[name]);
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		await page.locator('#start').click();
		await page.locator('#canvas').focus();
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		if (name === 'lighting-budget') {
		    await page.evaluate(() => Module._aether_web_resize(8193, 64));
		    await page.waitForFunction(() => aetherRuntime.status === 'failed' && aetherRuntime.inFlight === 0);
		    const state = await page.evaluate(() => aetherRuntime);
		    assert.ok(state.diagnostics.some(d => /budget|viewport|extent/i.test(d.message)), JSON.stringify(state.diagnostics));
		    item.scope = 'Oversized 2D lighting target rejected before target allocation; prior frames retire';
		    item.status = 'passed';
		    continue;
		}
		await page.evaluate(() => aetherTest.step(1));
		const image = await page.evaluate(() => aetherTest.capture());
		const near = (x, y, expected, tolerance, reason) => {
		    const actual = pixel(image, x, y);
		    assert.ok(expected.every((channel, i) => Math.abs(actual[i] - channel) <= tolerance), `${reason}: ${actual} vs ${expected}`);
		};
		if (name.startsWith('lighting-shadow-')) {
		    await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath),name+'-on.png')});
		    await page.evaluate(() => aetherTest.stop());
		    await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		    const offScene=name==='lighting-shadow-no-cast' ? 'BrowserShadowNoCast' : lightingScenes[name].replace(/On$/,'Off');
		    await page.goto(`http://127.0.0.1:${server.address().port}/?reference=1&scene=${offScene}&automation=1&test=1`);
		    await page.waitForFunction(scene=>aetherRuntime.scene===scene || aetherRuntime.status==='failed',offScene);
		    assert.notEqual(await page.evaluate(()=>aetherRuntime.status),'failed',JSON.stringify(await page.evaluate(()=>aetherRuntime.diagnostics)));
		    await page.locator('#start').click();
		    await page.locator('#canvas').focus();
		    await page.waitForFunction(()=>aetherRuntime.frames>2);
		    await page.evaluate(()=>aetherTest.step(1));
		    const unshadowed=await page.evaluate(()=>aetherTest.capture());
		    let shadowPixels=0,peakDifference=0;
		    for(let y=20;y<44;y++) for(let x=4;x<29;x++) {
		        const difference=pixel(unshadowed,x,y)[0]-pixel(image,x,y)[0];
		        peakDifference=Math.max(peakDifference,difference);
		        if(difference>20) shadowPixels++;
		    }
		    assert.ok(shadowPixels>=6,`Actual ${name} caster darkens at least six receiver pixels outside its camera silhouette: ${shadowPixels}, peak ${peakDifference}`);
		    if(name==='lighting-shadow-no-cast') {
		        const caster=pixel(unshadowed,32,30), receiver=pixel(unshadowed,20,30), background=pixel(unshadowed,48,30);
		        assert.ok(receiver[0]>100,`castShadows=false leaves the receiver lit: ${receiver}`);
		        assert.ok(background[0]-caster[0]>30,`Disabled shadow caster remains visible as an opaque upper-half mesh: ${caster}, ${background}`);
		    }
		    if(['lighting-shadow-mask','lighting-shadow-point','lighting-shadow-spot'].includes(name)) {
		        await writeFile(path.join(path.dirname(reportPath),name+'-readback.json'),JSON.stringify({on:image,off:unshadowed}));
		        // A unit caster covers y[-.5,.5]. SunY=0 preserves this vertical range;
		        // local lights at z0 project it twice as tall onto the receiver at z-4.
		        // Both place y[+.1875,-.1875] well inside the two artwork half regions.
		        const top=pixel(unshadowed,20,30)[0]-pixel(image,20,30)[0];
		        const bottom=pixel(unshadowed,20,33)[0]-pixel(image,20,33)[0];
		        assert.ok(top>bottom+15,`Alpha-cutout shadow preserves upper opaque/lower transparent artwork: ${top}, ${bottom}`);
		    }
		    item.shadow={shadowPixels,peakDifference,comparison:name==='lighting-shadow-no-cast' ? 'Same camera, lights, receiver and visible alpha caster; only meshRenderer.castShadows differs' : 'Same camera, lights, receiver and fixed-pose caster; only receiver.receiveShadows differs'};
		    await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath),name+'-off.png')});
		} else if (name === 'lighting-default') {
		    const surface=pixel(image,32,32), sky=pixel(image,32,4);
		    assert.ok(surface[0]>80 && surface[0]<250, `Default rough white mesh receives native startup lighting instead of unlit white: ${surface}`);
		    assert.ok(sky[2]>sky[1] && sky[1]>sky[0] && sky[0]>20, `Default camera renders the native blue sky without an environment block: ${sky}`);
		} else if (name === 'lighting-solid') {
		    for (const [x,y] of [[0,0],[32,32],[63,63]]) near(x,y,[51,102,153,255],1,'Camera solid display color remains WYSIWYG through HDR');
		} else if (name === 'lighting-gradient') {
		    for (const y of [4,32,59]) near(32,y,[Math.round(255*(1-(y+.5)/64)),0,Math.round(255*(y+.5)/64),255],1,'Camera gradient samples exact authored display-space stops');
		} else if (name === 'lighting-hdr') {
		    const half = neutralAces(.5), bright = neutralAces(8);
		    near(16,32,[half,half,half,255],4,'Half radiance uses native Hill ACES plus gamma');
		    near(48,32,[bright,bright,bright,255],2,'Eight radiance survives RGBA16F until native tonemap');
		    assert.ok(pixel(image,48,32)[0] - pixel(image,16,32)[0] > 50, 'HDR values remain distinct');
		} else if (name === 'lighting-sky') {
		    const top=pixel(image,32,8),bottom=pixel(image,32,55);
		    assert.ok(top[2]>top[1] && top[1]>top[0], `Authored blue atmosphere: ${top}`);
		    assert.ok(Math.abs(top[2]-bottom[2])>30, `Sky and ground differ across the horizon: ${top}, ${bottom}`);
		} else if (name === 'lighting-pbr-maps') {
		    const base=pixel(image,16,16),normal=pixel(image,48,16),ao=pixel(image,16,48),metal=pixel(image,48,48);
		    assert.ok(base[0]-normal[0]>10, `Tangent normal turns the surface away from the sun: ${base}, ${normal}`);
		    assert.ok(base[0]-ao[0]>5, `Occlusion map reduces ambient contribution: ${base}, ${ao}`);
		    assert.ok(metal.slice(0,3).some((v,i)=>Math.abs(v-base[i])>15), `Metal/roughness maps change the BRDF: ${base}, ${metal}`);
		} else if (name === 'lighting-2d-alpha') {
		    const blocked=pixel(image,52,20),open=pixel(image,52,44);
		    assert.ok(open[0]-blocked[0]>25, `Tile artwork's transparent lower half passes light while opaque upper half casts: ${blocked}, ${open}`);
		    assert.ok(open[0]>neutralAces(.02)+20,'Open ray includes radial light above authored ambient');
		} else {
		    const center=pixel(image,32,32), edge=pixel(image,53,32);
		    assert.ok(center[0]>100 && center[0]<240, `Rough dielectric receives finite direct lighting through ACES: ${center}`);
		    if (name === 'lighting-point') assert.ok(center[0]-edge[0]>20, `Inverse-square point falloff: ${center}, ${edge}`);
		    if (name === 'lighting-spot') assert.ok(center[0]-edge[0]>60, `Spot cone rejects off-axis surface: ${center}, ${edge}`);
		}
		item.probes={center:pixel(image,32,32),left:pixel(image,16,32),right:pixel(image,48,32)};
		await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath),name+'.png')});
		assert.deepEqual(await page.evaluate(() => aetherRuntime.diagnostics), []);
		await page.evaluate(() => aetherTest.stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.scope='Fixed camera, native exposure1/Hill ACES, one explicit simulation tick, actual GPU readback and clean retirement';
		item.status='passed';
		continue;
	    }
	    if (name === 'capture-backpressure') {
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		await page.evaluate(() => { aetherTestInternal.rejectCaptureOnce = true; });
		const image = await capture(page);
		assert.deepEqual(pixel(image, 32, 32), [255, 0, 0, 255]);
		assert.equal(await page.evaluate(() => aetherRuntime.captureDeferred), 1);
		assert.deepEqual(await page.evaluate(() => aetherRuntime.diagnostics), []);
		await page.evaluate(() => Module._aether_web_stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.scope = 'A capture survives a declined frame and reads the next submitted GPU frame';
		item.status = 'passed';
		continue;
	    }
	    if (name === 'authored-skin') {
		await page.waitForFunction(() => aetherRuntime.scene === 'BrowserSkin' || aetherRuntime.status === 'failed');
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		await page.locator('#start').click();
		await page.locator('#canvas').focus();
		await page.waitForFunction(() => globalThis.aetherTest?.readiness().ready && aetherRuntime.frames > 2);
		const initialTicks = await page.evaluate(() => aetherRuntime.ticks);
		const before = await page.evaluate(() => aetherTest.capture());
		assert.equal(before.width, 64);
		assert.equal(before.height, 64);
		assert.deepEqual(pixel(before, 16, 32), [255, 0, 0, 255], 'First native-cooked primitive uses local joint zero and its red material');
		assert.deepEqual(pixel(before, 40, 32), [0, 255, 255, 255], 'Second native-cooked primitive shares the authored skin and cyan material');
		assert.deepEqual(pixel(before, 24, 32), [0, 0, 0, 255]);
		assert.deepEqual(pixel(before, 48, 32), [0, 0, 0, 255]);
		await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath), 'authored-skin-before.png')});
		const stepped = await page.evaluate(() => aetherTest.step(30));
		assert.equal(stepped.ticks - initialTicks, 30, 'Animation advances exactly thirty fixed simulation ticks');
		const after = await page.evaluate(() => aetherTest.capture());
		assert.deepEqual(pixel(after, 16, 32), [0, 0, 0, 255], 'Red primitive leaves its original position');
		assert.deepEqual(pixel(after, 40, 32), [0, 0, 0, 255], 'Cyan primitive leaves its original position');
		assert.deepEqual(pixel(after, 24, 32), [255, 0, 0, 255], 'Half-second joint translation moves the red primitive eight pixels right');
		assert.deepEqual(pixel(after, 48, 32), [0, 255, 255, 255], 'Both primitives receive the same half-unit GPU skin translation');
		assert.equal(await page.evaluate(() => aetherRuntime.meshDraws), 2);
		await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath), 'authored-skin-after.png')});
		item.skinning = {fixedTicks:30, elapsedSeconds:0.5, worldDisplacement:[0.5,0,0], pixelDisplacement:[8,0], primitives:2};
		await page.evaluate(() => aetherTest.stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.status = 'passed';
		continue;
	    }
	    if (name === 'authored-models') {
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		const image = await capture(page);
		assert.deepEqual(pixel(image, 32, 32), [255, 0, 0, 255], 'Saved model uses the linked material with authored overrides');
		assert.deepEqual(pixel(image, 20, 32), [0, 0, 255, 255], 'Saved perspective camera and primitive share mesh depth');
		assert.equal(await page.evaluate(() => aetherRuntime.meshDraws), 2);
		await page.screenshot({path:path.join(path.dirname(reportPath), 'authored-models.png')});
		await page.evaluate(() => Module._aether_web_stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.status = 'passed';
		continue;
	    }
	    if (name === 'authored-dependencies') {
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		const image = await capture(page);
		assert.equal(image.width, 64);
		assert.equal(image.height, 64);
		assert.deepEqual(pixel(image, 16, 16), [255, 0, 0, 255], 'Saved include contributes its red primitive');
		assert.deepEqual(pixel(image, 32, 16), [0, 255, 0, 255], 'Linked prefab root keeps instance placement and receives its GUID material override');
		assert.deepEqual(pixel(image, 48, 16), [0, 0, 255, 255], 'Prefab child receives the root world-space delta once');
		assert.deepEqual(pixel(image, 16, 48), [0, 0, 0, 255], 'Removed prefab GUID does not reach the GPU');
		assert.deepEqual(pixel(image, 48, 48), [0, 255, 255, 255], 'Instance-local added primitive keeps its authored world-space position');
		assert.deepEqual(pixel(image, 32, 32), [0, 0, 0, 255], 'Prefab source root position remains empty after placement');
		assert.equal(await page.evaluate(() => aetherRuntime.meshDraws), 4);
		await page.locator('#canvas').screenshot({path:path.join(path.dirname(reportPath), 'authored-dependencies.png')});
		await page.evaluate(() => Module._aether_web_stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.scope = 'Saved include and linked prefab primitive composition, GUID material override, root world-space delta, removed GUID and instance-local addition';
		item.status = 'passed';
		continue;
	    }
	    if (name === 'authored-materials') {
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		assert.notEqual(await page.evaluate(() => aetherRuntime.status), 'failed', JSON.stringify(await page.evaluate(() => aetherRuntime.diagnostics)));
		const image = await capture(page);
		assert.equal(image.width, 64);
		assert.equal(image.height, 64);
		const nearPixel = (x, y, expected, message) => {
		    const actual = pixel(image, x, y);
		    assert.ok(actual.every((value, i) => Math.abs(value - expected[i]) <= 1), `${message}: ${actual} vs ${expected}`);
		};
		nearPixel(8, 16, [128, 128, 128, 255], 'sRGB albedo 128 decodes and encodes back to 128');
		nearPixel(24, 16, [128, 128, 128, 255], 'sRGB emissive texture contributes to a black base');
		nearPixel(32, 32, [32, 32, 32, 255], 'Minified stripes use the shared native cooked mip filter');
		assert.deepEqual(pixel(image, 16, 32), [255, 0, 0, 255], 'Linked cooked binary material retains its native base color');
		assert.deepEqual(pixel(image, 37, 13).slice(0, 3), [0, 0, 255], 'Transparent alpha checker reveals the blue backing');
		assert.deepEqual(pixel(image, 43, 13).slice(0, 3), [255, 0, 0], 'Opaque alpha checker writes the red cutout');
		assert.deepEqual(pixel(image, 37, 19).slice(0, 3), [255, 0, 0], 'Second checker row retains the opposite cell');
		assert.deepEqual(pixel(image, 43, 19).slice(0, 3), [0, 0, 255], 'Second checker row discards the opposite cell');
		assert.deepEqual(pixel(image, 56, 16), [0, 255, 0, 255], 'Mirrored front face remains visible with backface culling');
		nearPixel(8, 48, [128, 0, 64, 255], 'Transparent textured layers sort far blue before near red');
		nearPixel(24, 48, [128, 0, 255, 255], 'Additive textured red retains the opaque blue backing');
		assert.deepEqual(pixel(image, 40, 48), [255, 0, 255, 255], 'Double-sided material renders its back face');
		assert.deepEqual(pixel(image, 56, 48), [0, 0, 0, 255], 'Single-sided material culls its back face');
		assert.equal(await page.evaluate(() => aetherRuntime.meshDraws), 13);
		await page.screenshot({path:path.join(path.dirname(reportPath), 'authored-materials.png')});
		await page.evaluate(() => Module._aether_web_stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		item.scope = 'Authored unlit textures/materials, sRGB, cooked mip filter, cutout, alpha/additive order and mirrored/backface handling; not PBR/HDR or arbitrary glTF samplers';
		item.status = 'passed';
		continue;
	    }
	    if (name === 'mesh-invalid-frame') {
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.gameplayStopped);
		const state = await page.evaluate(() => aetherRuntime);
		assert.ok(state.diagnostics.some(item => item.code === 'WEBGPU_MESH' && /unregistered/.test(item.message)));
		assert.equal(state.frames || 0, 0);
		assert.equal(state.inFlight || 0, 0);
		item.status = 'passed';
		continue;
	    }
	    if (name === 'mesh-depth-resize') {
		await page.waitForFunction(() => aetherRuntime.frames > 2 || aetherRuntime.status === 'failed');
		let image = await capture(page);
		assert.deepEqual(pixel(image, 32, 32), [255, 0, 0, 255], 'Near geometry wins despite far geometry being drawn later');
		assert.deepEqual(pixel(image, 20, 32), [0, 0, 255, 255], 'Perspective exposes the larger far quad');
		assert.deepEqual(pixel(image, 32, 20), [0, 255, 0, 255], 'Sprite overlay stays visible over nearer mesh geometry');
		assert.deepEqual(pixel(image, 2, 2), [0, 0, 0, 255], 'Geometry clips to its projected bounds');
		await page.evaluate(() => Module._aether_web_resize(96, 48));
		image = await capture(page);
		assert.equal(image.width, 96);
		assert.equal(image.height, 48);
		assert.deepEqual(pixel(image, 48, 24), [255, 0, 0, 255]);
		await page.screenshot({path:path.join(path.dirname(reportPath), 'mesh-foundation.png')});
		await page.evaluate(() => Module._aether_web_stop());
		await page.waitForFunction(() => aetherRuntime.state === 'stopped' && aetherRuntime.inFlight === 0);
		assert.equal(await page.evaluate(() => aetherRuntime.meshDraws), 3);
		item.scope = 'Unlit native-cooked glTF and shared primitive geometry, perspective, depth, resize and retirement; not full 3D export or effect parity';
		item.status = 'passed';
		continue;
	    }
	    if (name === 'invalid-custom-pass-budget') {
		// Scripts only submit after a fixed tick, so earlier frames may legitimately render with no custom draws.
		await page.waitForFunction(() => aetherRuntime.scene === 'BrowserBadCustomPassBudget');
		await page.locator('#start').click();
		await page.locator('#canvas').focus();
		await page.waitForFunction(() => aetherRuntime.status === 'failed' && (aetherRuntime.inFlight || 0) === 0);
		const state = await page.evaluate(() => aetherRuntime);
		assert.equal(state.customPassDraws || 0, 0, 'Over-budget custom passes never reach GPU submission');
		assert.ok(state.diagnostics.some(d => /WEB_CUSTOM_PASS_BUDGET/.test(d.message)), JSON.stringify(state.diagnostics));
		item.scope = 'Seventeen full-size custom-pass submissions exceed the per-frame float4 budget and fail with an explicit diagnostic before GPU submission';
		item.status = 'passed';
		continue;
	    }
	    if (name.startsWith('invalid-')) {
		await page.waitForFunction(() => aetherRuntime.status === 'failed' && aetherRuntime.gameplayStopped === true);
		const state = await page.evaluate(() => aetherRuntime);
		assert.equal(state.ticks, 0);
		assert.ok(state.diagnostics.length > 0);
		if (name === 'invalid-font')
		    assert.ok(state.diagnostics.some(d => /font/i.test(d.message)));
		if (name === 'invalid-custom-pass-shader') {
		    assert.equal(state.frames || 0, 0, 'A missing custom-pass shader must fail before GPU submission');
		    assert.ok(state.diagnostics.some(d => /WEB_CUSTOM_PASS_SHADER/.test(d.message) && /missing_custom_pass_shader/.test(d.message)), JSON.stringify(state.diagnostics));
		    item.scope = 'Registering an uncooked custom-pass shader fails explicitly before simulation and GPU submission';
		}
		if(name.startsWith('invalid-particle-')) {
		    assert.equal(state.frames||0,0,'Invalid emitter must fail before GPU submission');
		    assert.equal(state.inFlight||0,0,'Invalid emitter must own no submitted frame');
		    assert.ok(state.diagnostics.some(d=>/particle|emitter/i.test(d.message)),JSON.stringify(state.diagnostics));
		    item.scope='Invalid authored emitter rejected before simulation and GPU submission';
		}
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
		// The scene switch lands inside the tick that requests it, as on desktop; the two saves from that
		// tick commit asynchronously while simulation is paused, so wait for them rather than racing.
		await page.waitForFunction(() => aetherPlatform.report.committedWrites === 2 || aetherRuntime.status === 'failed');
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
			'lighting-nonfinite', 'lighting-budget',
			'mesh-invalid-frame', 'unavailable', 'limits', 'device-loss', 'assets-missing', 'assets-corrupt', 'assets-version', 'assets-target',
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
