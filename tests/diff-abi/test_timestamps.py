"""Timestamp normalization preserves observable changes, not wall-clock identity."""
import struct
import unittest
from timestamps import normalize_timestamps
from harness import normalize


def packet(before=(90, 0, 90, 0, 90, 0), after=(90, 0, 101, 0, 101, 0),
           low=(100, 0), high=(102, 0), parent_before=(0,) * 6,
           parent_after=(0,) * 6):
    return struct.pack('<28q', *(low + high + before + after + parent_before + parent_after)).hex()


class TimestampTests(unittest.TestCase):
    def test_full_records_normalize_time_but_keep_syscall_observations(self):
        def record(payload, values='1 0 1 1 0'):
            return normalize(f'ABI BEGIN 1\nABI time.write {values} {payload}\nABI END 1\n', ['time.write'])
        original = packet()
        shifted = packet(before=(990, 0, 990, 0, 990, 0), after=(990, 0, 1001, 0, 1001, 0),
                         low=(1000, 0), high=(1002, 0))
        baseline = record(original)
        self.assertEqual(baseline, record(shifted))
        for values in ('-14 14 1 1 0', '1 0 2 1 0', '1 0 1 2 0', '1 0 1 1 7'):
            with self.subTest(values=values):
                self.assertNotEqual(baseline, record(shifted, values))
        with self.assertRaises(ValueError):
            record(packet(after=(90, 0, 103, 0, 103, 0)))

    def test_absolute_clock_identity_does_not_define_equivalence(self):
        original = packet()
        shifted = packet(before=(990, 0, 990, 0, 990, 0), after=(990, 0, 1001, 0, 1001, 0),
                         low=(1000, 0), high=(1002, 0))
        self.assertEqual(normalize_timestamps(original), normalize_timestamps(shifted))

    def test_missing_update_and_broken_cmtime_pair_differ(self):
        good = normalize_timestamps(packet())
        self.assertNotEqual(good, normalize_timestamps(packet(after=(90, 0, 90, 0, 90, 0))))
        self.assertNotEqual(good, normalize_timestamps(packet(after=(90, 0, 101, 0, 101, 1))))

    def test_time_outside_operation_window_fails(self):
        for after in ((90, 0, 99, 999999999, 101, 0), (90, 0, 102, 1, 101, 0)):
            with self.subTest(after=after), self.assertRaisesRegex(ValueError, 'interval'):
                normalize_timestamps(packet(after=after))

    def test_bad_clock_nanoseconds_and_length_fail(self):
        for data in (packet(low=(103, 0)), packet(high=(102, 1000000000)),
                     packet(after=(90, -1, 101, 0, 101, 0)), '00', '-'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                normalize_timestamps(data)

    def test_parent_changes_are_observed(self):
        baseline = packet(parent_before=(89, 0, 89, 0, 89, 0), parent_after=(89, 0, 101, 0, 101, 0))
        absent = packet()
        unchanged = packet(parent_before=(89, 0, 89, 0, 89, 0), parent_after=(89, 0, 89, 0, 89, 0))
        self.assertNotEqual(normalize_timestamps(baseline), normalize_timestamps(absent))
        self.assertNotEqual(normalize_timestamps(baseline), normalize_timestamps(unchanged))

    def test_new_inode_timestamps_must_be_inside_window(self):
        normalize_timestamps(packet(before=(0,) * 6, after=(101, 0, 101, 0, 101, 0)))
        with self.assertRaises(ValueError):
            normalize_timestamps(packet(before=(0,) * 6, after=(90, 0, 101, 0, 101, 0)))

    def test_relatime_preconditions_are_observed(self):
        stale = packet(before=(90, 0, 90, 0, 90, 0), after=(101, 0, 90, 0, 90, 0))
        fresh = packet(before=(91, 0, 90, 0, 90, 0), after=(101, 0, 90, 0, 90, 0))
        self.assertNotEqual(normalize_timestamps(stale), normalize_timestamps(fresh))

if __name__ == '__main__':
    unittest.main()
