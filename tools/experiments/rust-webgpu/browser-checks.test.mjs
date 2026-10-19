import assert from 'node:assert/strict';
import test from 'node:test';
import { isHardwareAdapter, validateResult } from './browser-checks.mjs';

test('software or unknown adapters cannot satisfy the real GPU gate', () => {
	assert.equal(isHardwareAdapter(null), false);
	assert.equal(isHardwareAdapter({ vendor: 'Google', description: 'SwiftShader' }), false);
	assert.equal(isHardwareAdapter({ vendor: 'nvidia', isFallbackAdapter: true }), false);
	assert.equal(isHardwareAdapter({ vendor: 'nvidia', description: 'RTX 5070', isFallbackAdapter: false }), true);
});

test('negative cases must fail for the requested diagnostic', () => {
	validateResult({ status: 'failed', code: 'WEBGPU_UNAVAILABLE' }, 'WEBGPU_UNAVAILABLE');
	assert.throws(() => validateResult({ status: 'passed' }, 'WEBGPU_UNAVAILABLE'));
	assert.throws(() => validateResult({ status: 'failed', code: 'WASM_ABORT' }, 'WEBGPU_UNAVAILABLE'));
	assert.throws(() => validateResult({ status: 'failed' }));
});
