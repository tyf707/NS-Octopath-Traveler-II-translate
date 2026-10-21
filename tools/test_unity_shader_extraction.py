import struct
import unittest
from extract_unity_shaders import unity_source, compute_programs, MAGIC
from types import SimpleNamespace as NS


class UnitySourceTest(unittest.TestCase):
    def test_compute_flat_and_variant_kernels(self):
        control = bytearray(0x880);control[:len(MAGIC)] = MAGIC
        struct.pack_into('<III', control, 0x700, 16, 16, 0)
        struct.pack_into('<I', control, 0x714, 5)
        code = bytes(range(16))
        kernel = NS(code=list(struct.pack('<II', len(control), len(code)) + control + code))
        for shader in (NS(kernels=[kernel], variants=None), NS(kernels=None, variants=[NS(kernels=[kernel])])):
            self.assertEqual(list(compute_programs(shader)), [(bytes(control), code, b'')])
        self.assertEqual(list(compute_programs(NS(kernels=[NS(code=[0]*32)]))), [])

    def test_extensionless_serialized_file(self):
        header = struct.pack('>IIII', 761, 362104, 21, 4096) + bytes(32)
        self.assertTrue(unity_source(header, 362104))
        self.assertFalse(unity_source(header, 362103))
        self.assertFalse(unity_source(bytes(48), 362104))

    def test_extended_header_and_bundle(self):
        header = bytearray(48)
        struct.pack_into('>IIII', header, 0, 0, 0, 22, 0)
        struct.pack_into('>IQQ', header, 20, 200, 20000, 4096)
        self.assertTrue(unity_source(header, 20000))
        self.assertFalse(unity_source(header[:32], 20000))
        self.assertTrue(unity_source(b'UnityFS\0', 100))


if __name__ == '__main__':
    unittest.main()
