import assert from 'node:assert/strict';

export function validateResult(result, expectedCode = null) {
	assert.ok(result && typeof result === 'object', 'The page must publish a structured result');
	if (expectedCode) {
		assert.equal(result.status, 'failed', 'The negative case must actually fail');
		assert.ok(result.code === expectedCode || result.diagnostics?.some((item) => item.code === expectedCode), 'The negative case must fail for the intended reason: ' + JSON.stringify(result));
	} else {
		assert.equal(result.status, 'passed', JSON.stringify(result));
	}
}

export function isHardwareAdapter(info) {
	if (!info || info.isFallbackAdapter === true) {
		return false;
	}
	const identity = [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(' ');
	if (/swiftshader|llvmpipe|lavapipe|software|microsoft basic render/i.test(identity)) {
		return false;
	}
	return /nvidia|intel|amd|radeon|apple|qualcomm|adreno|mali|\barm\b/i.test(identity);
}
