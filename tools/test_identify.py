import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from backup_identify import application, identify


def image():
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    payload = b'\x32\x54\xcd\xab' + b'TEST APPLICATION'
    data = header + struct.pack('<II', 0x40000000, len(payload)) + payload
    data.extend(b'\0' * (15-len(data) % 16))
    data.append(0)
    return bytes(data)


class IdentifyTests(unittest.TestCase):
    def test_requires_application_descriptor_and_complete_segments(self):
        app = image()
        self.assertEqual(application(app, 0), app)
        self.assertIsNone(application(app[:-1], 0))
        bad = app[:32] + b'NOPE' + app[36:]
        self.assertIsNone(application(bad, 0))

    def test_exact_match_ignores_settings_but_not_application_changes(self):
        app = image()
        flash = b'\xff'*0x10000 + app + b'USER SETTINGS'
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary)/'vendor.zip'
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('board/Burn files/test_New_Panel.bin', b'\0'*0x10000+app)
            result = identify(flash, archive)
            self.assertTrue(result['panel_revision'].startswith('New'))
            changed = bytearray(flash)
            changed[0x10000+40] ^= 1
            self.assertEqual(identify(changed, archive)['panel_revision'], 'unconfirmed')

    def test_unlabelled_factory_version_does_not_imply_old_panel(self):
        app = image()
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary)/'vendor.zip'
            with zipfile.ZipFile(archive, 'w') as output:
                output.writestr('board/Burn files/board-V2.7.bin', b'\0'*0x10000+app)
            result = identify(b'\0'*0x10000+app, archive)
            self.assertEqual(result['panel_revision'], 'unconfirmed')
            self.assertEqual(len(result['exact_matches']), 1)


if __name__ == '__main__':
    unittest.main()
