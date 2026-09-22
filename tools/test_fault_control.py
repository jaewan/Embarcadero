"""Fault packet parser checks; no processes or broker mappings."""
import unittest
from unittest import mock
from fault_control import FaultControl

class FaultHitTests(unittest.TestCase):
    def test_native_thread_identity_is_preserved(self):
        control = object.__new__(FaultControl)
        with mock.patch.object(control, "wait", return_value="HIT 1 partial 7 3 9 1 8192 123"):
            self.assertEqual(control.hit(1), {"id": 1, "name": "partial", "client": 7,
                "epoch": 3, "batch": 9, "detail0": 1, "detail1": 8192, "tid": 123})

    def test_missing_or_invalid_tid_cannot_qualify(self):
        control = object.__new__(FaultControl)
        for packet in ("HIT 1 partial 7 3 9 1 8192", "HIT 1 partial 7 3 9 1 8192 0"):
            with self.subTest(packet=packet), mock.patch.object(control, "wait", return_value=packet):
                with self.assertRaises(RuntimeError):
                    control.hit(1)

if __name__ == "__main__":
    unittest.main()
