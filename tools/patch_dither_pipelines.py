"""Find every G-buffer pipeline carrying the near-camera dither and write a patched pixel shader.

    python tools/patch_dither_pipelines.py <dumps/pipelines dir> <output dir>

A pipeline qualifies when its pixel shader disassembly has exactly one discard and the dither
signature: a noise sample subtracted from a value and scaled by 256 (`l(256.000000)`). Each
qualifying ps.dxbc gets its discard replaced by no-ops (see dxbc_nop_discard.py) and is written as
gear_nodither_<pipeline>.dxbc, which freecam loads as a pixel stage replacement in first person.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from dxbc_nop_discard import patch


def main() -> None:
    dumps, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    matched = skipped = 0
    for name in sorted(os.listdir(dumps)):
        folder = os.path.join(dumps, name)
        ps_txt = os.path.join(folder, 'ps.txt')
        ps_dxbc = os.path.join(folder, 'ps.dxbc')
        meta = os.path.join(folder, 'pipeline.txt')
        if not (os.path.isfile(ps_txt) and os.path.isfile(ps_dxbc)):
            continue
        if os.path.isfile(meta):
            text = open(meta, encoding='utf-8', errors='replace').read()
            targets = re.search(r'targets?\D*(\d+)', text)
            if targets and int(targets.group(1)) < 4:
                continue
        lines = open(ps_txt, encoding='utf-8', errors='replace').read().splitlines()
        discards = [l for l in lines if re.match(r'\s*discard', l)]
        dither = any('l(256.000000)' in l and 'mul_sat' in l for l in lines)
        if len(discards) != 1 or not dither:
            skipped += 1
            continue
        data = bytearray(open(ps_dxbc, 'rb').read())
        if patch(data) != 1:
            skipped += 1
            continue
        open(os.path.join(out, f'gear_nodither_{name}.dxbc'), 'wb').write(data)
        matched += 1
    print(f'patched {matched} pipelines, skipped {skipped}')


if __name__ == '__main__':
    main()
