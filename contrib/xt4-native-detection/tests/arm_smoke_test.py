import importlib.util
from pathlib import Path
import struct
import unittest
p = Path(__file__).resolve().parents[1] / 'tools/run_arm_smoke.py'
spec = importlib.util.spec_from_file_location('smoke', p)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

def bits(x):
    return struct.pack('>f', x).hex()

class SmokeTest(unittest.TestCase):
    def test_valid(self):
        box = {'score': .9, 'x0': 1, 'y0': 2, 'x1': 3, 'y1': 4}
        output = 'CASE face 1\nBOX ' + ' '.join(bits(box[k]) for k in box) + '\nPEAK 100\nSTACK_HIGH_WATER 32\nPASS native ARM face inference\n'
        result = m.validate_output(output, 0, {'face': [box]})
        self.assertEqual(result['cases'][0]['count'], 1)
        for bad in [output.replace('CASE face 1', 'CASE face 0'), output.replace(bits(.9), bits(.1)),
                    output.replace('PASS native ARM face inference', ''), output.replace('PEAK 100', 'PEAK 99999999'),
                    output.replace('STACK_HIGH_WATER 32', 'STACK_HIGH_WATER 131072'), output + 'CASE extra 0\n']:
            with self.assertRaises(ValueError):
                m.validate_output(bad, 0, {'face': [box]})
        with self.assertRaises(ValueError):
            m.validate_output(output, 1, {'face': [box]})
        with self.assertRaises(ValueError):
            m.validate_output(output.replace(bits(.9), bits(float('nan'))), 0, {'face': [box]})

if __name__ == '__main__':
    unittest.main()
