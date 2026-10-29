"""Semantic probes for the 512x512 BrowserUiParity reference scene.

Probes are deliberately well inside authored shapes; a visually blank renderer must
not pass merely because two backends agree. The control is BrowserSolid, whose
camera and clear colour are identical to the UI fixture.
"""
from __future__ import annotations


def _pixel(image, width, x, y):
    offset = (y * width + x) * 4
    return tuple(image[offset:offset + 3])


def scene_features(image, control, width, height):
    if (width, height) != (512, 512) or len(image) != width * height * 4 or len(control) != len(image):
        raise ValueError('UI semantic qualification requires paired 512x512 RGBA captures')
    locations = {
        'buttonCorner': (33, 33), 'buttonBody': (50, 50),
        'maskOutside': (290, 50), 'maskOuter': (304, 50),
        'maskInner': (420, 90), 'maskChild': (340, 90),
        'progressBody': (80, 120), 'toggleBody': (70, 178),
        'sliderBody': (80, 236), 'placeholderBody': (45, 300),
        'passwordBody': (45, 360),
    }
    pixels = {name: _pixel(image, width, *point) for name, point in locations.items()}
    controls = {name: _pixel(control, width, *point) for name, point in locations.items()}
    difference = lambda a, b: max(abs(x - y) for x, y in zip(a, b))
    dominant = lambda pixel, channel: pixel[channel] > max(pixel[(channel + 1) % 3], pixel[(channel + 2) % 3]) + 20
    checks = {
        'roundedButtonCornerPreservesBackground': difference(pixels['buttonCorner'], controls['buttonCorner']) <= 12,
        'buttonBodyVisible': difference(pixels['buttonBody'], controls['buttonBody']) > 20,
        'maskOutsidePreservesBackground': difference(pixels['maskOutside'], controls['maskOutside']) <= 12,
        'outerMaskRed': dominant(pixels['maskOuter'], 0),
        'innerMaskGreen': dominant(pixels['maskInner'], 1),
        'descendantDrawnOverParents': dominant(pixels['maskChild'], 2)
            and difference(pixels['maskChild'], controls['maskChild']) > 20
            and difference(pixels['maskChild'], pixels['maskInner']) > 20,
        'progressVisible': difference(pixels['progressBody'], controls['progressBody']) > 20,
        'toggleVisible': difference(pixels['toggleBody'], controls['toggleBody']) > 20,
        'sliderVisible': difference(pixels['sliderBody'], controls['sliderBody']) > 20,
        'placeholderFieldVisible': difference(pixels['placeholderBody'], controls['placeholderBody']) > 20,
        'passwordFieldVisible': difference(pixels['passwordBody'], controls['passwordBody']) > 20,
    }
    return dict(passed=all(checks.values()), checks=checks, pixels=pixels, controlPixels=controls, locations=locations)

def shader_features(image, control, width, height):
    if (width, height) != (512, 512) or len(image) != width * height * 4 or len(control) != len(image):
        raise ValueError('UI shader qualification requires paired 512x512 RGBA captures')
    locations = {'backgroundEffect': (100, 80), 'overlayEffect': (100, 208),
                 'elementMaterial': (100, 336), 'outside': (450, 450)}
    pixels = {name: _pixel(image, width, *point) for name, point in locations.items()}
    controls = {name: _pixel(control, width, *point) for name, point in locations.items()}
    differences = {name: max(abs(a - b) for a, b in zip(pixels[name], controls[name])) for name in locations}
    checks = {name + 'Visible': differences[name] > 20 for name in ('backgroundEffect', 'overlayEffect', 'elementMaterial')}
    checks['outsideUnchanged'] = differences['outside'] <= 12
    glyph_changed = sum(
        max(abs(image[(y * width + x) * 4 + c] - control[(y * width + x) * 4 + c]) for c in range(3)) > 20
        for y in range(416, 472) for x in range(32, 300)
    )
    checks['glyphMaterialVisible'] = glyph_changed >= 10
    return dict(passed=all(checks.values()), checks=checks, pixels=pixels,
                controlPixels=controls, differences=differences, glyphChangedPixels=glyph_changed, locations=locations)
