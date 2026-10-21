import struct
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unity_shader_identity import identity, add_aliases


class UnityIdentityTest(unittest.TestCase):
    def test_runtime_identifier_is_ignored_but_code_and_metadata_are_not(self):
        control = bytearray(0x880)
        struct.pack_into('<I', control, 0x704, 8)
        original = identity(control, b'12345678')
        control[0x7d0:0x7d8] = b'changed!'
        self.assertEqual(original, identity(control, b'12345678'))
        self.assertNotEqual(original, identity(control, b'12345679'))
        control[0x714] = 1
        self.assertNotEqual(original, identity(control, b'12345678'))

    def test_truncated_or_mismatched_input_is_rejected(self):
        with self.assertRaises(ValueError):
            identity(bytes(8), bytes(8))
        with self.assertRaises(ValueError):
            identity(bytes(0x880), bytes(8))

    def test_alias_generation_checks_translated_semantics(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / 'raw'
            cache = Path(directory) / 'cache'
            raw.mkdir(); cache.mkdir()
            manifest = dict(format=1, shaders={})
            for number in range(2):
                key = f'{number:016x}'
                control = bytearray(0x880)
                struct.pack_into('<I', control, 0x704, 8)
                control[0x7d0] = number
                (raw / f'{key}.control').write_bytes(control)
                (raw / f'{key}.code').write_bytes(b'12345678')
                (cache / f'{key}.spv').write_bytes(b'identical compiled shader')
                reflection = json.dumps(dict(key=key, stage='vs', notes=[]))
                (cache / f'{key}.res.json').write_text(reflection)
                manifest['shaders'][key] = dict(
                    spv=hashlib.sha256(b'identical compiled shader').hexdigest(),
                    reflection=hashlib.sha256(reflection.encode()).hexdigest())
            def publish():
                manifest.pop('id', None)
                manifest['id'] = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
                (cache / 'manifest.json').write_text(json.dumps(manifest))
            publish()
            count, _ = add_aliases(raw, cache)
            self.assertEqual(count, 1)
            key = '0000000000000001'
            reflection = json.dumps(dict(key=key, stage='ps', notes=[]))
            (cache / f'{key}.res.json').write_text(reflection)
            manifest['shaders'][key]['reflection'] = hashlib.sha256(reflection.encode()).hexdigest()
            publish()
            with self.assertRaisesRegex(ValueError, 'Ambiguous'):
                add_aliases(raw, cache)


if __name__ == '__main__':
    unittest.main()
