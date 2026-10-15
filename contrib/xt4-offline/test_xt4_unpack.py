"""Tests for recovered X-T4 token behavior; no camera code is executed."""
import unittest

try:
    from xt4_unpack import decode_tokens
except ImportError:
    decode_tokens = None


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(decode_tokens, "X-T4 token decoder is not implemented")

    def test_recovers_broadcom_build_name_from_real_match_pair(self):
        prefix = b"43430a1-roml/sdio-g-pool-p2p-idsu"
        packed = bytes([len(prefix)]) + prefix + b"\x80\x64\x04auth"
        self.assertEqual(decode_tokens(packed), prefix + b"p-idauth")

    def test_overlapping_copy_uses_newly_produced_bytes(self):
        self.assertEqual(decode_tokens(b"\x01A\x80\x1f"), b"A" * 16)

    def test_truncated_literal_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_tokens(b"\x04abc")

    def test_reference_before_output_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_tokens(b"\x80\x13")

    def test_expansion_limit_is_enforced(self):
        with self.assertRaises(ValueError):
            decode_tokens(b"\x01A\x80\x1f", max_output=15)


if __name__ == "__main__":
    unittest.main()
