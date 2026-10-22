import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

// These fixtures are capture inputs, not qualified output. Run the same scene
// and tick on Vulkan and hardware WebGPU before adding them to a parity report.
const configuration = JSON.parse(readFileSync(new URL('./material-effect-oracles.json', import.meta.url), 'utf8'));
export const materialEffectScenes = Object.freeze(configuration.scenes);
export const materialEffectGraphics = Object.freeze(configuration.graphics);
export const materialEffectCapture = Object.freeze(configuration.capture);
export const materialEffectPairs = Object.freeze(configuration.pairs);

function validate(image) {
    assert.ok(Number.isInteger(image.width) && image.width > 0);
    assert.ok(Number.isInteger(image.height) && image.height > 0);
    assert.equal(image.pixels.length, image.width * image.height * 4);
}

export function regionMetrics(on, off, region) {
    validate(on); validate(off);
    assert.equal(on.width, off.width); assert.equal(on.height, off.height);
    const x0 = Math.floor(region[0] * on.width), y0 = Math.floor(region[1] * on.height);
    const x1 = Math.max(x0 + 1, Math.floor(region[2] * on.width));
    const y1 = Math.max(y0 + 1, Math.floor(region[3] * on.height));
    assert.ok(x0 >= 0 && y0 >= 0 && x1 <= on.width && y1 <= on.height);
    let absolute = 0, signed = 0, changed = 0, samples = 0;
    for (let y = y0; y < y1; ++y) {
        for (let x = x0; x < x1; ++x) {
            const offset = (y * on.width + x) * 4;
            let maximum = 0;
            for (let channel = 0; channel < 3; ++channel) {
                const difference = on.pixels[offset + channel] - off.pixels[offset + channel];
                absolute += Math.abs(difference); signed += difference;
                maximum = Math.max(maximum, Math.abs(difference));
            }
            changed += maximum > 3 ? 1 : 0; ++samples;
        }
    }
    return {meanAbsoluteDifference: absolute / (samples * 3), meanSignedDifference: signed / (samples * 3), changedPixels: changed, samples};
}

export function checkMaterialEffectPair(pair, on, off) {
    const metrics = regionMetrics(on, off, pair.region);
    assert.ok(metrics.meanAbsoluteDifference >= pair.minDifference,
        `${pair.name} must change its authored receiver region: ${JSON.stringify(metrics)}`);
    if (pair.direction === 'darker') assert.ok(metrics.meanSignedDifference < -pair.minDifference, `${pair.name} must darken its receiver`);
    if (pair.direction === 'brighter') assert.ok(metrics.meanSignedDifference > pair.minDifference, `${pair.name} must brighten its receiver`);
    if (pair.direction === 'unchanged') assert.ok(metrics.meanAbsoluteDifference <= pair.maxDifference, `${pair.name} must preserve the visible receiver`);
    const background = regionMetrics(on, off, [.02, .02, .08, .08]);
    assert.ok(background.meanAbsoluteDifference <= 1, `${pair.name} changed the independent corner control`);
    return {name: pair.name, ...metrics, cornerDifference: background.meanAbsoluteDifference};
}

export const materialEffectLimits = Object.freeze(configuration.limits);
