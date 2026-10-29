"""Semantic controls shared with the JS material-effect qualification module."""
from __future__ import annotations

import json
from pathlib import Path

CONFIGURATION = json.loads(Path(__file__).with_name('material-effect-oracles.json').read_text(encoding='utf-8'))
SCENES = tuple(CONFIGURATION['scenes'])
PAIRS = tuple(CONFIGURATION['pairs'])
GRAPHICS = CONFIGURATION['graphics']
LIMITS = CONFIGURATION['limits']


def validate_selection(scenes, graphics):
    selected = set(scenes)
    for pair in PAIRS:
        if (pair['on'] in selected) != (pair['off'] in selected):
            raise ValueError('Material qualification requires both feature and control scenes: ' + pair['name'])
    if any(scene.startswith('BrowserContact') for scene in selected) and not graphics.get('contactShadows', False):
        raise ValueError('Contact-shadow qualification requires contactShadows=true for both backends')
    if any(scene.startswith('BrowserFoliage') for scene in selected) and graphics.get('foliageSkyLook', 0) <= 0:
        raise ValueError('Foliage qualification requires foliageSkyLook>0 for both backends')
    if any(scene.startswith('BrowserGraphBackface') for scene in selected) and 'BrowserGraph' not in selected:
        raise ValueError('Backface qualification also requires BrowserGraph to stage and verify the native custom shader')
    mask = {'BrowserCrossMaskOn', 'BrowserCrossMaskOff', 'BrowserCrossMaskSolid'}
    if selected & mask and not mask <= selected:
        raise ValueError('Cutout-shadow qualification requires masked, receive-off and solid controls')
    for pair in ({'BrowserCrossSkinOn', 'BrowserCrossSkinOff'},
                 {'BrowserCrossMirror', 'BrowserCrossControl'}):
        if selected & pair and not pair <= selected:
            raise ValueError('Cross-product qualification requires both controls: ' + ', '.join(sorted(pair)))


def specular_filter_features(on, off, width, height, region):
    """Compare the same fixture at specularFilter=1 and =0, on one backend."""
    metrics = region_metrics(on, off, width, height, region)
    checks = {'filterChangesHighlight': metrics['meanAbsoluteDifference'] >= 1.0}
    return dict(passed=all(checks.values()), checks=checks, **metrics)


def mirrored_normal_features(mirrored, control, width, height, region):
    """The +X tangent-space normal faces the +X light only without reflection."""
    metrics = region_metrics(control, mirrored, width, height, region)
    checks = {'unmirroredFacesLight': metrics['meanSignedDifference'] >= 2.0}
    return dict(passed=all(checks.values()), checks=checks, **metrics)


def cutout_shadow_features(masked, unshadowed, solid, width, height, hole, opaque):
    """A cutout hole stays lit while the adjacent opaque part casts a shadow."""
    hole_mask = region_metrics(masked, unshadowed, width, height, hole)
    hole_solid = region_metrics(solid, unshadowed, width, height, hole)
    opaque_mask = region_metrics(masked, unshadowed, width, height, opaque)
    checks = {
        'holeRemainsLit': hole_mask['meanAbsoluteDifference'] <= 2.0,
        'solidCastsIntoHole': hole_solid['meanSignedDifference'] <= -2.0,
        'opaquePartCasts': opaque_mask['meanSignedDifference'] <= -2.0,
    }
    return dict(passed=all(checks.values()), checks=checks,
                holeMasked=hole_mask, holeSolid=hole_solid, opaqueMasked=opaque_mask)


def region_metrics(on, off, width, height, region):
    if len(on) != width * height * 4 or len(off) != len(on):
        raise ValueError('Material control capture extent/payload mismatch')
    x0, y0 = int(region[0] * width), int(region[1] * height)
    x1, y1 = max(x0 + 1, int(region[2] * width)), max(y0 + 1, int(region[3] * height))
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise ValueError('Material control region is outside capture')
    absolute = signed = changed = samples = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 4
            differences = [on[offset+c] - off[offset+c] for c in range(3)]
            absolute += sum(abs(delta) for delta in differences)
            signed += sum(differences)
            changed += max(abs(delta) for delta in differences) > 3
            samples += 1
    return dict(meanAbsoluteDifference=absolute / (samples * 3), meanSignedDifference=signed / (samples * 3), changedPixels=changed, samples=samples)


def pair_features(pair, on, off, width, height):
    metrics = region_metrics(on, off, width, height, pair['region'])
    normal = mirrored_normal_features(on, off, width, height, pair['region']) if pair['on'] == 'BrowserCrossMirror' else None
    checks = dict(authoredReceiverChanges=metrics['meanAbsoluteDifference'] >= pair['minDifference'])
    if pair.get('direction') == 'darker':
        checks['receiverDarkens'] = metrics['meanSignedDifference'] < -pair['minDifference']
    if pair.get('direction') == 'brighter':
        checks['receiverBrightens'] = metrics['meanSignedDifference'] > pair['minDifference']
    if pair.get('direction') == 'unchanged':
        checks['receiverPreserved'] = metrics['meanAbsoluteDifference'] <= pair['maxDifference']
    corner = region_metrics(on, off, width, height, [.02, .02, .08, .08])
    checks['independentCornerUnchanged'] = corner['meanAbsoluteDifference'] <= 1
    if normal:
        checks.update(normal['checks'])
    return dict(passed=all(checks.values()), checks=checks, **metrics, cornerDifference=corner['meanAbsoluteDifference'],
                receiverRegion=pair['region'], minimumMeanDifference=pair['minDifference'])


def scene_features(scene, image, width, height, sample):
    # Individual scenes must contain real visible geometry. The paired controls
    # below additionally reject implementations that omit the requested effect.
    # Procedural probes sit on a lit cell of the fixed-tick pattern. Molten's moved
    # when its noise hash became bit-identical across backends; its 3x3 neighbourhood
    # is >138 in both molten scenes on both backends.
    centre = ((44, 32) if 'Foliage' in scene else (35, 34) if 'Plasma' in scene else (35, 41) if 'Molten' in scene
              else (48, 32) if scene == 'BrowserCrossMaskSolid' else (16, 32) if 'Blob' in scene else (32, 32))
    probes = dict(center=sample(*centre), left=sample(16, 32), right=sample(48, 32))
    if scene == 'BrowserParticlesDisplayOnly':
        background = sample(4, 4)
        active = sum(max(image[(y*width+x)*4+c] - background[c] for c in range(3)) > 25
                     for y in range(height//4, 3*height//4) for x in range(width//4, 3*width//4))
        checks = dict(seededDisplayOnlyBurstVisible=active >= 8)
        probes['burst'] = sample(32, 30)
    else:
        checks = dict(authoredMaterialReceiverVisible=max(probes['center']) > 30,
                      spatialFixtureVisible=max(max(image[c::4]) - min(image[c::4]) for c in range(3)) > 5)
    return dict(passed=all(checks.values()), checks=checks, probes=probes, centerProbeSquarePixels=list(centre))
