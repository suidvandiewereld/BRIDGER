import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

import golden


def bmp(path, pixels, top_down=False):
    width, height = 1, len(pixels)
    rows = pixels if top_down else list(reversed(pixels))
    data = b''.join(bytes(row) + b'\0' for row in rows)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height,
                          1, 24, 0, len(data), 0, 0, 0, 0)
    path.write_bytes(header + data)


class GoldenTest(unittest.TestCase):
    def test_padding_orientation_and_tolerance(self):
        with tempfile.TemporaryDirectory() as root:
            a, b = Path(root) / 'a.bmp', Path(root) / 'b.bmp'
            bmp(a, [(10, 20, 30), (40, 50, 60)])
            bmp(b, [(10, 20, 33), (40, 50, 60)], top_down=True)
            self.assertTrue(golden.compare(a, b, 3, 0)['passed'])
            report = golden.compare(a, b, 2, 0)
            self.assertFalse(report['passed'])
            self.assertEqual(report['bad_pixels'], 1)
            self.assertTrue(golden.compare(a, b, 2, 0.5)['passed'])
            b.write_bytes(b.read_bytes()[:-1])
            with self.assertRaisesRegex(ValueError, 'truncated'):
                golden.read_bmp(b)

    def test_accept_and_missing_baseline(self):
        with tempfile.TemporaryDirectory() as root, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            a, b = Path(root) / 'capture.bmp', Path(root) / 'golden.bmp'
            bmp(a, [(1, 2, 3)])
            args = [str(a), '--baseline', str(b)]
            self.assertEqual(golden.main(args), 2)
            self.assertEqual(golden.main(args + ['--accept', '--note', 'test fixture']), 0)
            self.assertEqual(golden.main(args), 0)
            self.assertEqual(golden.main(args + ['--accept', '--note', 'overwrite']), 2)
            bmp(a, [(1, 2, 3), (1, 2, 3)])
            self.assertEqual(golden.main(args), 2)


if __name__ == '__main__':
    unittest.main()
