"""Compare readback BMPs. Uses only the Python standard library."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


DEFAULT_BASELINE = Path(__file__).resolve().parents[2] / 'captures/golden/fxdemo.bmp'


def read_bmp(path):
    data = Path(path).read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('not a BMP with a complete header')
    offset, header = struct.unpack_from('<II', data, 10)
    width, height, planes, bits, compression = struct.unpack_from('<iiHHI', data, 18)
    if header < 40 or offset < 14 + header or planes != 1 or bits != 24 or compression != 0:
        raise ValueError('expected an uncompressed 24 bit readback BMP')
    if width <= 0 or height == 0:
        raise ValueError('invalid image size')
    stride = (width * 3 + 3) & ~3
    if offset + stride * abs(height) > len(data):
        raise ValueError('truncated BMP pixels')
    rows = range(abs(height) - 1, -1, -1) if height > 0 else range(-height)
    pixels = b''.join(data[offset + y * stride:offset + y * stride + width * 3] for y in rows)
    return width, abs(height), pixels


def compare(baseline, capture, tolerance, max_bad_fraction):
    w, h, expected = read_bmp(baseline)
    cw, ch, actual = read_bmp(capture)
    if (w, h) != (cw, ch):
        raise ValueError(f'image size differs: baseline {w}x{h}, capture {cw}x{ch}')
    bad = total = peak = 0
    for i in range(0, len(expected), 3):
        delta = [abs(expected[i + c] - actual[i + c]) for c in range(3)]
        error = max(delta)
        bad += error > tolerance
        total += sum(delta)
        peak = max(peak, error)
    fraction = bad / (w * h)
    return dict(passed=fraction <= max_bad_fraction, width=w, height=h,
                bad_pixels=bad, bad_fraction=fraction, max_channel_error=peak,
                mean_channel_error=total / len(expected), tolerance=tolerance,
                max_bad_fraction=max_bad_fraction)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--baseline', type=Path, default=DEFAULT_BASELINE)
    parser.add_argument('--tolerance', type=int, default=3, help='allowed channel error, 0 to 255')
    parser.add_argument('--max-bad-fraction', type=float, default=0.001)
    parser.add_argument('--accept', action='store_true', help='create a new baseline, never overwrite')
    parser.add_argument('--note', default='', help='scene, camera, resolution, DLSS and mod settings')
    args = parser.parse_args(argv)
    if not 0 <= args.tolerance <= 255 or not math.isfinite(args.max_bad_fraction) or not 0 <= args.max_bad_fraction <= 1:
        parser.error('tolerance must be 0 to 255 and max bad fraction must be 0 to 1')
    try:
        if args.accept:
            if not args.note.strip():
                raise ValueError('--accept needs --note with the capture settings')
            w, h, _ = read_bmp(args.capture)
            data = args.capture.read_bytes()
            manifest = args.baseline.with_suffix('.json')
            if args.baseline.exists() or manifest.exists():
                raise ValueError('baseline exists; use a new baseline path for a new scene')
            args.baseline.parent.mkdir(parents=True, exist_ok=True)
            with args.baseline.open('xb') as out:
                out.write(data)
            with manifest.open('x', encoding='utf-8') as out:
                json.dump(dict(source=str(args.capture.resolve()), width=w, height=h,
                               sha256=hashlib.sha256(data).hexdigest(), note=args.note), out, indent=2)
            print(f'Baseline saved: {args.baseline}')
            return 0
        result = compare(args.baseline, args.capture, args.tolerance, args.max_bad_fraction)
        print(json.dumps(result, indent=2))
        return 0 if result['passed'] else 1
    except (OSError, ValueError, struct.error) as error:
        print(f'Golden capture unavailable: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
