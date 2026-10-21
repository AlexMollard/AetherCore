export const legacyCaseNames = [
    'mesh-depth-resize',
    'mesh-invalid-frame',
    'sprite-input-resize-shutdown',
    'unavailable',
    'limits',
    'device-loss',
    'stop-during-init',
    'stop-in-flight',
    'layers',
    'batch-capacity',
    'responsive',
    'drop-owner-init',
    'stop-after-upload',
    'assets-ready',
    'assets-missing',
    'assets-corrupt',
    'assets-version',
    'assets-target',
    'assets-cancel',
    'platform-save-reload',
    'platform-storage-denied',
    'platform-storage-quota',
    'platform-audio-resume',
    'reference-gameplay',
    'browser-features',
    'browser-gamepad',
    'browser-focus-resume',
    'invalid-font',
    'invalid-scene-version',
    'invalid-atlas-version',
    'invalid-audio-component',
    'browser-key-latch',
    'browser-ui-order'
];

export const requiredBridgeCases = [
    'fixed-reference', 'protocol-bounds', 'rust-panic', 'no-adapter', 'fetch-failure', 'save-denied', 'lost-device', 'focus-reset',
    'production-boundary', 'oversize-package'
];

export function qualify(required, cases) {
    if (!required.length)
	return 'blocked';
    if (cases.some(item => item.status === 'failed'))
	return 'failed';
    for (const name of required) {
	const found = cases.filter(item => item.name === name);
	if (found.length > 1)
	    return 'failed';
	if (found.length !== 1 || found[0].status !== 'passed' || found[0].hardware === false)
	    return 'blocked';
    }
    return 'passed';
}
