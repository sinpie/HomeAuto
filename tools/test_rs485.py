import unittest
from rs485_log import checksum_ok, split_packets


class SdsPacketTests(unittest.TestCase):
    def test_checksum_examples_from_public_analysis(self):
        for packet in ['B0 41 00 71', 'B0 5A 00 6A', 'AD 41 00 6C', 'B0 2F 01 1E', 'B0 10 01 01 20']:
            self.assertTrue(checksum_ok(bytes.fromhex(packet)), packet)
        self.assertFalse(checksum_ok(bytes.fromhex('B0 41 00 70')))

    def test_split_stream(self):
        stream = bytes.fromhex('AD 41 00 6C B0 41 00 71 AE 7C 01 00 00 00 00 53 B0 10 01 01 20')
        packets = split_packets(stream)
        self.assertEqual([kind for kind, _ in packets], ['ok'] * 4)
        self.assertEqual(packets[2][1], bytes.fromhex('AE 7C 01 00 00 00 00 53'))

    def test_garbage_is_kept_separately(self):
        packets = split_packets(bytes.fromhex('12 34 AD 41 00 6C'))
        self.assertEqual(packets[0], ('junk', bytes.fromhex('12 34')))
        self.assertEqual(packets[1], ('ok', bytes.fromhex('AD 41 00 6C')))


if __name__ == '__main__':
    unittest.main()
