'use strict';

// Copied only into explicit automation builds. Commands never write ECS state.
(() => {
    const keys = new Uint8Array(512);
    const inputs = [];
    let manual = new URLSearchParams(globalThis.location?.search || '').has('automation');
    let pending = null;
    let fault = false;
    const snapshot = () => {
	const {capture, ...state} = aetherRuntime;
	return structuredClone({...state, bridgeVersion : 1});
    };
    const fail = message => {
	keys.fill(0);
	inputs.length = 0;
	if (pending) {
	    clearTimeout(pending.timer);
	    pending.reject(Error(message));
	    pending = null;
	}
    };
    const bridge = {
	version : 1,
	snapshot,
	readiness() {
	    const state = snapshot();
	    return {
		ready : state.status === 'running' && state.rendererReady && state.assetsReady && state.storageReady &&
			    !!globalThis.aetherPlatform?.started,
		terminal : [ 'failed', 'stopped' ].includes(state.status),
		state
	    };
	},
	logs : () => structuredClone(aetherRuntime.diagnostics),
	// Read-only copy of the last stepped tick's observable entity records (same schema as native state.json).
	entities : () => {
	    const internal = globalThis.aetherTestInternal;
	    return structuredClone({
		tick : internal.entityTick ?? 0,
		scene : aetherRuntime.scene ?? null,
		entities : internal.entityJson ? JSON.parse(internal.entityJson) : []
	    });
	},
	input(command) {
	    if (!command || Object.keys(command).some(key => !['key', 'down'].includes(key)))
		throw TypeError('Unknown input field');
	    if (!Number.isInteger(command.key) || command.key < 0 || command.key >= 512)
		throw RangeError('Invalid key');
	    if (typeof command.down !== 'boolean')
		throw TypeError('down must be boolean');
	    if (inputs.length >= 128)
		throw RangeError('Input queue is full');
	    inputs.push({...command});
	},
	step(ticks, timeoutMs = 10000) {
	    if (!Number.isInteger(ticks) || ticks < 1 || ticks > 600)
		throw RangeError('Invalid tick count (1..600)');
	    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 120000)
		throw RangeError('Invalid timeout');
	    if (pending)
		throw Error('A step command is already pending');
	    if (bridge.readiness().terminal)
		throw Error('Runtime is terminal');
	    manual = true;
	    return new Promise((resolve, reject) => {
		pending = {remaining : ticks, resolve, reject, timer : setTimeout(() => fail('Fixed-step command timed out'), timeoutMs)};
	    });
	},
	async capture(timeoutMs = 10000) {
	    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 120000)
		throw RangeError('Invalid timeout');
	    if (manual && Number.isInteger(aetherRuntime.lastRenderedFrameIndex)) {
		const desired = {tick:aetherRuntime.ticks || 0, generation:aetherRuntime.renderSceneGeneration,
		    width:Module.canvas.width,height:Module.canvas.height};
		const matches = image => image && image.frameIndex === aetherRuntime.lastRenderedFrameIndex &&
		    image.simulationTick === desired.tick && image.generation === desired.generation &&
		    image.width === desired.width && image.height === desired.height;
		// Every accepted manual tick already queued its own readback. Waiting for
		// that immutable frame preserves motion/history and cannot advance exposure.
		if (!aetherRuntime.lastRenderedFrameCaptured) {
		    Module._aether_web_capture();
		}
		const deadline=performance.now()+timeoutMs;
		while (!matches(aetherRuntime.capture)) {
		    if (bridge.readiness().terminal || performance.now()>=deadline)
			throw Error('Accepted-frame capture failed or timed out');
		    await new Promise(resolve=>setTimeout(resolve,10));
		}
		return structuredClone(aetherRuntime.capture);
	    }
	    delete aetherRuntime.capture;
	    Module._aether_web_capture();
	    const deadline = performance.now() + timeoutMs;
	    while (!aetherRuntime.capture) {
		if (bridge.readiness().terminal || performance.now() >= deadline)
		    throw Error('Capture failed or timed out');
		await new Promise(resolve => setTimeout(resolve, 10));
	    }
	    return structuredClone(aetherRuntime.capture);
	},
	loseDevice() { Module._aether_web_lose_device(); },
	panic() {
	    if (!manual || !pending)
		throw Error('Panic requires a pending fixed-step command');
	    fault = true;
	},
	stop() {
	    keys.fill(0);
	    inputs.length = 0;
	    fail('Runtime stopped');
	    Module._aether_web_stop();
	}
    };
    globalThis.aetherTestInternal = {
	keys,
	key : key => !!keys[key],
	manual : () => manual,
	beginFrame() {
	    if (bridge.readiness().terminal) {
		keys.fill(0);
		fail('Runtime is terminal');
		return 0;
	    }
	    if (!pending || pending.remaining === 0)
		return 0;
	    for (const command of inputs.splice(0))
		keys[command.key] = command.down ? 1 : 0;
	    return 1;
	},
	takeFault() {
	    const result = fault;
	    fault = false;
	    return result;
	},
	afterTick() {
	    if (pending && pending.remaining > 0)
		--pending.remaining;
	},
	endFrame() {
	    if (bridge.readiness().terminal) {
		fail('Runtime is terminal');
		return;
	    }
	    if (pending?.remaining === 0) {
		const completed = pending;
		pending = null;
		clearTimeout(completed.timer);
		completed.resolve(snapshot());
	    }
	}
    };
    const release = () => {
	keys.fill(0);
	inputs.length = 0;
    };
    globalThis.addEventListener?.('blur', release);
    globalThis.document?.getElementById('canvas')?.addEventListener('blur', release);
    globalThis.document?.addEventListener('visibilitychange', () => {
	if (document.hidden)
	    release();
    });
    globalThis.aetherTest = Object.freeze(bridge);
})();
