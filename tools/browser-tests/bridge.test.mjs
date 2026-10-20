import assert from 'node:assert/strict';
import {existsSync, readFileSync} from 'node:fs';
import {test} from 'node:test';
import vm from 'node:vm';

const filename = new URL('../../resources/web/test-bridge.js', import.meta.url);
test('test bridge exposes versioned snapshots and queues input until an engine tick', async () => {
    assert.ok(existsSync(filename), 'test-build bridge is missing');
    const runtime = {
	status : 'running',
	state : 'ready',
	rendererReady : true,
	assetsReady : true,
	storageReady : true,
	audioReady : true,
	ticks : 0,
	diagnostics : []
    };
    const context = vm.createContext({
	aetherRuntime : runtime,
	Module : {},
	aetherPlatform : {started : true},
	structuredClone,
	URLSearchParams,
	performance,
	setTimeout,
	clearTimeout,
	Map,
	Promise,
	Error,
	TypeError,
	RangeError
    });
    vm.runInContext(readFileSync(filename, 'utf8'), context);
    const bridge = context.aetherTest;
    assert.equal(bridge.version, 1);
    const snapshot = bridge.snapshot();
    snapshot.ticks = 999;
    assert.equal(runtime.ticks, 0);
    bridge.input({key : 32, down : true});
    assert.equal(context.aetherTestInternal.key(32), false);
    const step = bridge.step(2);
    assert.equal(context.aetherTestInternal.beginFrame(), 1);
    assert.equal(context.aetherTestInternal.key(32), true);
    context.aetherTestInternal.afterTick();
    runtime.ticks++;
    context.aetherTestInternal.endFrame();
    context.aetherTestInternal.beginFrame();
    context.aetherTestInternal.afterTick();
    runtime.ticks++;
    context.aetherTestInternal.endFrame();
    assert.equal((await step).ticks, 2);
    assert.throws(() => bridge.input({key : 512, down : true}), /key/);
    assert.throws(() => bridge.step(0), /tick/);
    assert.throws(() => bridge.input({key : 32, down : true, script : 'bad'}), /field/);
});

test('timed out steps cannot leave injected keys or queued input behind', async () => {
    const context = vm.createContext({
	aetherRuntime : {status : 'running', state : 'ready', diagnostics : []},
	Module : {},
	structuredClone,
	URLSearchParams,
	performance,
	setTimeout,
	clearTimeout
    });
    vm.runInContext(readFileSync(filename, 'utf8'), context);
    context.aetherTest.input({key : 32, down : true});
    const pending = context.aetherTest.step(1, 1);
    context.aetherTestInternal.beginFrame();
    assert.equal(context.aetherTestInternal.key(32), true);
    await assert.rejects(pending, /timed out/);
    assert.equal(context.aetherTestInternal.key(32), false);
});
