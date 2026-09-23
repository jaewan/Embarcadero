#!/usr/bin/env python3
"""Reject invalid native workload geometry before opening any broker connection."""
import os
import subprocess
import sys
import unittest


class ClientCliValidation(unittest.TestCase):
    def test_invalid_workload_sizes(self):
        binary, config = sys.argv[1:3]
        cases = [
            ('zero message', ['-m', '0', '-s', '1024', '-t', '2'], {}, 'Message size and total size'),
            ('zero total', ['-m', '16', '-s', '0', '-t', '2'], {}, 'Message size and total size'),
            ('nonmultiple', ['-m', '16', '-s', '1025', '-t', '2'], {}, 'exact multiple'),
            ('short latency', ['-m', '8', '-s', '1024', '-t', '2'], {}, 'Latency messages require'),
            ('short indexed audit', ['-m', '7', '-s', '1022', '-t', '1', '-o', '5'],
             {'EMBAR_VALIDATE_ORDER': '1'}, 'Indexed ordered-delivery audit requires'),
            ('zero threads', ['-m', '16', '-s', '1024', '-n', '0'], {}, 'Thread and client counts'),
            ('zero clients', ['-m', '16', '-s', '1024', '-p', '0'], {}, 'Thread and client counts'),
        ]
        for name, args, overrides, expected in cases:
            with self.subTest(name=name):
                completed = subprocess.run([binary, '--config', config, *args],
                    env={**os.environ, **overrides}, text=True, capture_output=True, timeout=5)
                self.assertEqual(completed.returncode, 1, completed.stderr)
                self.assertIn(expected, completed.stderr)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
