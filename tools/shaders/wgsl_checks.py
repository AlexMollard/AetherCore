"""Reject WGSL that Slang 2026.8 miscompiles.

Slang lowers a negated vector expression to `vecN<T>(0) - expr` without
parenthesising expr, so `-(a + b)` becomes `vec3<f32>(0) - a + b` and
`-(a - b)` becomes `vec3<f32>(0) - a - b`. Both compile and silently flip a
term. The emitted text cannot distinguish that from a correct `-a + b`, so any
zero-minus vector operand followed by another additive term is rejected;
rewrite the source as a single product (`-(a + b) * m` -> `-(sum * m)`) or
reorder (`b - a`). Scalars emit `- (expr)` and are unaffected.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

_ZERO_MINUS = re.compile(r'vec[234]<[a-z0-9]+>\(0\) - ')


def find_negated_vector_sums(wgsl: str) -> list[tuple[int, str]]:
    """Return (line, snippet) for every ambiguous zero-minus vector expression."""
    findings = []
    for match in _ZERO_MINUS.finditer(wgsl):
        depth = 0
        index = match.end()
        while index < len(wgsl):
            char = wgsl[index]
            if char in '([':
                depth += 1
            elif char in ')]':
                if depth == 0:
                    break
                depth -= 1
            elif char in ',;' and depth == 0:
                break
            elif depth == 0 and wgsl.startswith((' + ', ' - '), index - 1):
                line = wgsl.count('\n', 0, match.start()) + 1
                findings.append((line, wgsl[match.start():index + 40].replace('\n', ' ')))
                break
            index += 1
    return findings


def validate_negated_vector_sums(wgsl: str, name: str = 'shader') -> None:
    findings = find_negated_vector_sums(wgsl)
    if findings:
        detail = '; '.join(f'{name}:{line}: {snippet}' for line, snippet in findings[:4])
        raise ValueError('WEB_SHADER_NEGATED_SUM: Slang emitted an unparenthesised negated vector sum '
                         '(-(a + b) became 0 - a + b); rewrite the source as one product or reorder: ' + detail)


def main(paths: list[str]) -> int:
    status = 0
    for path in paths:
        try:
            validate_negated_vector_sums(Path(path).read_text(encoding='utf-8'), path)
        except ValueError as error:
            print(error, file=sys.stderr)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
