'use strict';

// Browser services are asynchronous. A staged write is visible to its writer,
// but only a completed transaction increments committedWrites.
class AetherBrowserPlatform {
    constructor(projectId) {
	this.projectId = projectId;
	this.records = new Map();
	this.pending = new Map();
	this.audioSources = new Map();
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
    prepareAudio(path) { this.audioSources.set(path, true); }
    finishAudioPreparation() {
	this.audioPrepared = true;
	this.audioBytes = true;
    }
    async startAudio() {
	if (this.stopped || this.failed || !this.audioPrepared)
	    return false;
	if (this.startingAudio)
	    return this.startingAudio;
	this.startingAudio = this.initializeAudio().finally(() => { this.startingAudio = null; });
	return this.startingAudio;
    }
    async initializeAudio() {
	try {
	    if (!this.audio) {
		this.audio = new AudioContext({sampleRate: 48000});
		if (this.audio.sampleRate !== 48000)
		    throw Error('Audio device cannot create a 48000 Hz context');
		const context = this.audio;
		this.audio.onstatechange = () => {
		    this.audioReady = !this.stopped && !this.failed && !!this.sink && context.state === 'running';
		    aetherRuntime.audioReady = this.audioReady;
		};
		this.analyser = this.audio.createAnalyser();
		this.analyser.fftSize = 256;
		this.analyser.connect(this.audio.destination);
		this.samples = new Float32Array(256);
	    }
	    const context = this.audio;
	    // Invoked directly by the trusted start button, before the first await.
	    await context.resume();
	    if (this.stopped || this.failed || this.audio !== context)
		return false;
	    if (!this.sink) {
		await context.audioWorklet.addModule('audio-worklet.js');
		if (this.stopped || this.failed || this.audio !== context)
		    return false;
		this.freeAudioBuffers = Array.from({length: 8}, () => new Float32Array(2048));
		this.sink = new AudioWorkletNode(context, 'aether-pcm-sink', {outputChannelCount: [2]});
		const sink = this.sink;
		sink.port.onmessage = event => {
		    if (!this.stopped && this.sink === sink)
			this.freeAudioBuffers.push(event.data);
		};
		this.sink.connect(this.analyser);
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
	    if (!this.stopped)
		this.fail('BROWSER_AUDIO', error);
	    return false;
	}
    }
    audioBufferAvailable() {
	// Eight buffers bound ownership, but at most three (~64 ms) are in flight.
	return this.audioReady && !this.stopped && !this.failed && this.freeAudioBuffers.length > 5;
    }
    submitAudio(samples) {
	const buffer = this.freeAudioBuffers.pop();
	buffer.set(samples);
	this.sink.port.postMessage(buffer, [buffer.buffer]);
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
	if (this.meter) {
	    clearInterval(this.meter);
	    this.meter = null;
	}
	this.audioSources.clear();
	this.audioReady = false;
	aetherRuntime.audioReady = false;
	const sink = this.sink;
	this.sink = null;
	if (sink) {
	    sink.port.onmessage = null;
	    sink.port.postMessage({stop: true});
	    sink.disconnect();
	    sink.port.close();
	}
	this.freeAudioBuffers = [];
	if (this.analyser) {
	    this.analyser.disconnect();
	    this.analyser = null;
	}
	const context = this.audio;
	this.audio = null;
	if (context) {
	    context.onstatechange = null;
	    if (context.state !== 'closed')
		context.close().catch(() => {});
	}
    }
}

globalThis.aetherInput = {
    x : -1000000,
    y : -1000000,
    buttons : 0,
    pressed : 0,
    dx : 0,
    dy : 0,
    scroll : 0,
    lockRequested: false,
    lockArmed: false,
    lockPending: false,
    readClipboardText: async () => navigator.clipboard.readText(),
    writeClipboardText: async text => navigator.clipboard.writeText(text),
    pasteText: null,
    typedText: '',
    hapticsAvailable() {
        return Array.from(navigator.getGamepads?.() || []).some(pad =>
            pad?.connected && typeof pad.vibrationActuator?.playEffect === 'function');
    },
    rumble(low, high, milliseconds) {
        let supported = 0;
        for (const pad of navigator.getGamepads?.() || []) {
            const actuator = pad?.connected && pad.vibrationActuator;
            if (!actuator || typeof actuator.playEffect !== 'function') continue;
            ++supported;
            const operation = milliseconds > 0
                ? actuator.playEffect('dual-rumble', {duration: milliseconds, strongMagnitude: low, weakMagnitude: high})
                : actuator.reset();
            Promise.resolve(operation).then(result => {
                if (result !== 'complete' && result !== 'preempted')
                    console.warn('Gamepad haptics refused:', result);
            }).catch(error => console.warn('Gamepad haptics failed:', error));
        }
        if (!supported && milliseconds > 0)
            console.warn('Gamepad rumble unavailable: no supported physical actuator');
        return supported;
    },
    setLockRequested(requested) {
        if (requested !== this.lockRequested) {
            this.lockRequested = requested;
            this.lockArmed = requested;
        }
        if (!requested && document.pointerLockElement === inputCanvas)
            document.exitPointerLock();
    }
};
const inputCanvas = document.getElementById('canvas');
inputCanvas.addEventListener('paste', event => {
    aetherInput.pasteText = event.clipboardData.getData('text/plain');
    event.preventDefault();
});
inputCanvas.addEventListener('keydown', event => {
    if (!event.ctrlKey && !event.metaKey && !event.altKey && !event.isComposing &&
        Array.from(event.key).length === 1)
        aetherInput.typedText += event.key;
});
const pointer = event => {
    const rect = inputCanvas.getBoundingClientRect();
    aetherInput.x = (event.clientX - rect.left) * inputCanvas.width / Math.max(rect.width, 1);
    aetherInput.y = (event.clientY - rect.top) * inputCanvas.height / Math.max(rect.height, 1);
    // DOM buttons are left/right/middle; the shared input uses left/right/middle too.
    aetherInput.buttons = (event.buttons & 1) | ((event.buttons & 2)) | ((event.buttons & 4));
};
inputCanvas.addEventListener('pointermove', event => {
    if (document.pointerLockElement === inputCanvas || (document.activeElement === inputCanvas && aetherInput.buttons)) {
        aetherInput.dx += event.movementX;
        aetherInput.dy += event.movementY;
    }
    pointer(event);
});
inputCanvas.addEventListener('pointerdown', event => {
    inputCanvas.focus();
    if (aetherInput.lockRequested && aetherInput.lockArmed && !aetherInput.lockPending && event.isTrusted) {
        aetherInput.lockArmed = false;
        aetherInput.lockPending = true;
        try {
            const result = inputCanvas.requestPointerLock();
            if (result && typeof result.catch === 'function')
                result.catch(error => { aetherInput.lockPending = false; console.warn('Pointer lock refused:', error); });
        } catch (error) {
            aetherInput.lockPending = false;
            console.warn('Pointer lock refused:', error);
        }
    } else if (document.pointerLockElement !== inputCanvas) {
        inputCanvas.setPointerCapture(event.pointerId);
    }
    pointer(event);
    aetherInput.pressed |= aetherInput.buttons
});
inputCanvas.addEventListener('pointerup', pointer);
document.addEventListener('pointerlockchange', () => {
    aetherInput.lockPending = false;
    aetherInput.dx = aetherInput.dy = 0;
});
document.addEventListener('pointerlockerror', () => {
    aetherInput.lockPending = false;
    aetherInput.lockArmed = false;
    console.warn('Pointer lock failed');
});
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
