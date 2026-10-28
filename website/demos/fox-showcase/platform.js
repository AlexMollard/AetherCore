'use strict';

// Browser services are asynchronous. A staged write is visible to its writer,
// but only a completed transaction increments committedWrites.
class AetherBrowserPlatform {
    constructor(projectId) {
	this.projectId = projectId;
	this.records = new Map();
	this.pending = new Map();
	this.voices = new Set();
	this.audioSources = new Map();
	this.buffers = new Map();
	this.started = false;
	this.storageReady = false;
	this.audioReady = false;
	this.busy = false;
	this.stopped = false;
	this.failed = false;
	this.report = {pendingWrites : 0, committedWrites : 0, scheduledSounds : 0, peak : 0};
	aetherRuntime.platform = this.report;
	this.openStorage();
    }
    fail(code, error) {
	if (this.stopped || this.failed)
	    return;
	this.failed = true;
	aetherRuntime.status = 'failed';
	aetherRuntime.diagnostics.push({code, message : `${error.name || 'Error'}: ${error.message || error}`});
    }
    openStorage() {
	try {
	    const request = indexedDB.open(`AetherCore.${this.projectId}.v1`, 1);
	    request.onupgradeneeded = () => request.result.createObjectStore('records');
	    request.onerror = () => this.fail('BROWSER_STORAGE', request.error);
	    request.onblocked = () => this.fail('BROWSER_STORAGE', Error('Database upgrade blocked by another page'));
	    request.onsuccess = () => {
		const db = request.result;
		if (this.stopped || this.failed) {
		    db.close();
		    return;
		}
		this.db = db;
		db.onversionchange = () => {
		    db.close();
		    this.fail('BROWSER_STORAGE', Error('Database version changed in another page'));
		};
		try {
		    const transaction = db.transaction('records', 'readonly');
		    const store = transaction.objectStore('records');
		    const keys = store.getAllKeys(), values = store.getAll();
		    transaction.onabort = () => this.fail('BROWSER_STORAGE', transaction.error || Error('Save preload aborted'));
		    transaction.onerror = () => this.fail('BROWSER_STORAGE', transaction.error || Error('Save preload failed'));
		    transaction.oncomplete = () => {
			if (this.stopped || this.failed)
			    return;
			this.records = new Map(keys.result.map((key, i) => [key, values.result[i]]));
			this.storageReady = true;
			aetherRuntime.storageReady = true;
		    };
		} catch (error) {
		    this.fail('BROWSER_STORAGE', error);
		}
	    };
	} catch (error) {
	    this.fail('BROWSER_STORAGE', error);
	}
    }
    stage(key, value) {
	if (!this.storageReady || this.stopped || this.failed || !/^[a-zA-Z0-9_-]{1,64}$/.test(key) || typeof value !== 'string' ||
	    new TextEncoder().encode(value).length > 65536)
	    return false;
	this.pending.set(key, value);
	this.report.pendingWrites = this.pending.size;
	return true;
    }
    read(key) { return this.pending.has(key) ? this.pending.get(key) : this.records.get(key); }
    flush() {
	if (this.flushPromise)
	    return this.flushPromise;
	if (!this.storageReady || this.failed || this.stopped)
	    return Promise.resolve(false);
	if (!this.pending.size)
	    return Promise.resolve(true);
	const batch = new Map(this.pending);
	this.busy = true;
	this.flushPromise = new Promise(resolve => {
				let transaction;
				const fail = error => {
				    this.fail('BROWSER_STORAGE', error);
				    resolve(false);
				};
				try {
				    transaction = this.db.transaction('records', 'readwrite', {durability : 'strict'});
				    this.transaction = transaction;
				    transaction.oncomplete = () => {
					if (this.stopped || this.failed) {
					    resolve(false);
					    return;
					}
					for (const [key, value] of batch) {
					    this.records.set(key, value);
					    if (this.pending.get(key) === value)
						this.pending.delete(key);
					}
					this.report.committedWrites += batch.size;
					this.report.pendingWrites = this.pending.size;
					resolve(true);
				    };
				    transaction.onabort = () => fail(transaction.error || Error('Save transaction aborted'));
				    transaction.onerror = () => fail(transaction.error || Error('Save transaction failed'));
				    const store = transaction.objectStore('records');
				    for (const [key, value] of batch)
					store.put(value, key);
				} catch (error) {
				    if (transaction)
					transaction.abort();
				    fail(error);
				}
			    }).finally(() => {
	    this.busy = false;
	    this.flushPromise = null;
	    this.transaction = null;
	});
	return this.flushPromise;
    }
    prepareAudio(path, bytes) { this.audioSources.set(path, bytes); }
    finishAudioPreparation() {
	this.audioPrepared = true;
	this.audioBytes = true;
    }
    async prepareFont(bytes) {
	try {
	    const face = await new FontFace('AetherRoboto', bytes).load();
	    if (this.stopped || this.failed)
		return;
	    document.fonts.add(face);
	    this.fontFace = face;
	    const canvas = new OffscreenCanvas(1024, 1024);
	    const context = canvas.getContext('2d', {willReadFrequently : true});
	    context.font = '48px AetherRoboto';
	    context.fillStyle = 'white';
	    this.advances = new Float32Array(256);
	    let ascent = 0;
	    let descent = 0;
	    for (let code = 32; code < 256; ++code) {
		if (code >= 127 && code < 160)
		    continue;
		const metrics = context.measureText(String.fromCodePoint(code));
		if (metrics.width > 64)
		    throw Error('Font exceeds the 64-pixel glyph cell limit');
		this.advances[code] = metrics.width;
		ascent = Math.max(ascent, metrics.actualBoundingBoxAscent);
		descent = Math.max(descent, metrics.actualBoundingBoxDescent);
	    }
	    // Round outward so rasterization cannot spill into the adjacent atlas row.
	    this.fontAscent = Math.ceil(ascent);
	    this.fontDescent = Math.ceil(descent);
	    this.fontBaseline = this.fontAscent;
	    if (this.fontAscent + this.fontDescent > 64)
		throw Error('Font exceeds the 64-pixel glyph cell height');
	    for (let code = 32; code < 256; ++code) {
		if (code >= 127 && code < 160)
		    continue;
		context.fillText(String.fromCodePoint(code), (code % 16) * 64,
		                 Math.floor(code / 16) * 64 + this.fontBaseline);
	    }
	    this.fontAtlas = context.getImageData(0, 0, 1024, 1024).data;
	    this.fontReady = true;
	    aetherRuntime.fontReady = true;
	} catch (error) {
	    this.fail('BROWSER_FONT', error);
	}
    }
    async startAudio() {
	if (this.stopped || this.failed || !this.audioPrepared)
	    return false;
	if (!this.audioSources.size) {
	    this.started = true;
	    return true;
	}
	try {
	    if (!this.audio) {
		this.audio = new AudioContext();
		this.audio.onstatechange = () => {
		    this.audioReady = !this.stopped && !this.failed && this.audioSources.size > 0 &&
				      this.buffers.size === this.audioSources.size && this.audio.state === 'running';
		    aetherRuntime.audioReady = this.audioReady;
		};
		this.analyser = this.audio.createAnalyser();
		this.analyser.fftSize = 256;
		this.analyser.connect(this.audio.destination);
		this.samples = new Float32Array(256);
	    }
	    // Invoked directly by the trusted start button, before the first await.
	    await this.audio.resume();
	    for (const [path, bytes] of this.audioSources) {
		if (this.buffers.has(path))
		    continue;
		const buffer = await this.audio.decodeAudioData(bytes.slice(0));
		if (this.stopped || this.failed)
		    return false;
		if (buffer.duration > 120 || buffer.length * buffer.numberOfChannels * 4 > 64 * 1024 * 1024)
		    throw Error('Audio clip exceeds the 120-second / 64 MiB decoded limit: ' + path);
		this.buffers.set(path, buffer);
	    }
	    if (this.stopped || this.failed)
		return false;
	    this.audioReady = this.audio.state === 'running';
	    this.started = this.audioReady;
	    aetherRuntime.audioReady = this.audioReady;
	    if (!this.meter)
		this.meter = setInterval(() => {
		    this.analyser.getFloatTimeDomainData(this.samples);
		    for (const value of this.samples)
			this.report.peak = Math.max(this.report.peak, Math.abs(value));
		}, 5);
	    return this.audioReady;
	} catch (error) {
	    this.fail('BROWSER_AUDIO', error);
	    return false;
	}
    }
    play(path) {
	if (!this.audioReady || this.audio.state !== 'running' || this.stopped || this.failed || !this.buffers.has(path) ||
	    this.voices.size >= 32)
	    return false;
	const source = this.audio.createBufferSource();
	source.buffer = this.buffers.get(path);
	source.connect(this.analyser);
	this.voices.add(source);
	source.onended = () => {
	    source.disconnect();
	    this.voices.delete(source);
	};
	source.start(this.audio.currentTime + 0.01);
	++this.report.scheduledSounds;
	return true;
    }
    stop() {
	if (this.stopped)
	    return;
	this.stopped = true;
	if (this.transaction) {
	    try {
		this.transaction.abort();
	    } catch {
	    }
	}
	if (this.db)
	    this.db.close();
	if (this.meter)
	    clearInterval(this.meter);
	for (const voice of this.voices)
	    voice.stop();
	this.voices.clear();
	this.audioSources.clear();
	this.buffers.clear();
	this.fontAtlas = null;
	this.advances = null;
	this.audioReady = false;
	aetherRuntime.audioReady = false;
	if (this.audio && this.audio.state !== 'closed')
	    this.audio.close().catch(() => {});
	if (this.fontFace)
	    document.fonts.delete(this.fontFace);
    }
}

globalThis.aetherInput = {
    x : -1000000,
    y : -1000000,
    buttons : 0,
    pressed : 0,
    dx : 0,
    dy : 0,
    scroll : 0
};
const inputCanvas = document.getElementById('canvas');
const pointer = event => {
    const rect = inputCanvas.getBoundingClientRect();
    aetherInput.x = (event.clientX - rect.left) * inputCanvas.width / Math.max(rect.width, 1);
    aetherInput.y = (event.clientY - rect.top) * inputCanvas.height / Math.max(rect.height, 1);
    // DOM buttons are left/right/middle; the shared input uses left/right/middle too.
    aetherInput.buttons = (event.buttons & 1) | ((event.buttons & 2)) | ((event.buttons & 4));
};
inputCanvas.addEventListener('pointermove', event => {
    if (document.activeElement === inputCanvas && aetherInput.buttons) {
        aetherInput.dx += event.movementX;
        aetherInput.dy += event.movementY;
    }
    pointer(event);
});
inputCanvas.addEventListener('pointerdown', event => {
    inputCanvas.focus();
    inputCanvas.setPointerCapture(event.pointerId);
    pointer(event);
    aetherInput.pressed |= aetherInput.buttons
});
inputCanvas.addEventListener('pointerup', pointer);
inputCanvas.addEventListener('wheel', event => {
    if (document.activeElement === inputCanvas && document.hasFocus() && !document.hidden) {
        aetherInput.scroll -= event.deltaY / (event.deltaMode === 1 ? 3 : event.deltaMode === 2 ? 1 : 100);
        event.preventDefault();
    }
}, {passive: false});
inputCanvas.addEventListener('lostpointercapture', () => {aetherInput.buttons = 0});
window.addEventListener('blur', () => {
    aetherInput.buttons = 0;
    aetherInput.pressed = 0;
    aetherInput.dx = aetherInput.dy = aetherInput.scroll = 0;
});
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
	aetherInput.buttons = 0;
	aetherInput.pressed = 0;
	aetherInput.dx = aetherInput.dy = aetherInput.scroll = 0;
    }
});
inputCanvas.addEventListener('blur', () => {
    aetherInput.buttons = 0;
    aetherInput.pressed = 0;
    aetherInput.dx = aetherInput.dy = aetherInput.scroll = 0;
});
globalThis.aetherPlatform = null;
if (globalThis.aetherProject || new URLSearchParams(location.search).has('reference') || new URLSearchParams(location.search).has('platform')) {
    globalThis.aetherPlatform = new AetherBrowserPlatform(globalThis.aetherProject?.id || 'AgentPipelineRust');
}
document.getElementById('start').onclick = () => {
    inputCanvas.focus();
    aetherPlatform?.startAudio();
};

if (aetherPlatform && !globalThis.aetherProject) {
    const scene = new URLSearchParams(location.search).get('scene');
    const instructions = {
	EmberRun: 'WASD / arrows: move · Space: dash · R: retry. Collect amber embers and dodge red hazards.',
	BrowserFeatures: 'Start audio, then click the canvas toggle and slider. The sprite animates above the tile map.',
	ReferenceStart: 'Hold Space to score. Your run saves automatically.',
    }[scene];
    if (instructions)
	document.getElementById('instructions').textContent = instructions;
} else if (!aetherPlatform) {
    document.getElementById('start').hidden = true;
}
