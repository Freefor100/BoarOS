"""Protect observation integrity; an incomplete child is never a passing record."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('inventory', Path(__file__).with_name('run.py'))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class ObservationTests(unittest.TestCase):
    @staticmethod
    def complete_stream():
        lines = ['INVENTORY BEGIN 1']
        for name in inventory.CASES:
            lines += [f'INVENTORY OUTPUT {name} .',
                      f'INVENTORY STATUS {name} {256 if name == "false" else 0}']
        return '\n'.join(lines + ['INVENTORY END 6', ''])

    def test_complete_success_requires_clean_process_exit(self):
        raw = self.complete_stream()
        self.assertEqual(inventory.run_status(raw, 0)['status'], 'pass')
        self.assertEqual(inventory.run_status(raw, 0, ['pass', 'semantic-mismatch'])['status'],
                         'semantic-mismatch')
        for code, expected in [('timeout', 'timeout'), (3, 'crash')]:
            with self.subTest(code=code):
                self.assertEqual(inventory.run_status(raw, code)['status'], expected)
                self.assertEqual(len(inventory.observations(raw)), 6)

    def test_boaros_must_finish_resource_cleanup(self):
        raw = self.complete_stream()
        self.assertEqual(inventory.run_status(raw, 0, kernel='boaros')['status'], 'crash')
        raw += 'BoarOS: PID 1 exited status=0x2a pages=0x123 heap-live=0x0; shutting down\n'
        self.assertEqual(inventory.run_status(raw, 0, kernel='boaros')['status'], 'pass')
        self.assertEqual(inventory.run_status(raw.replace('heap-live=0x0', 'heap-live=0x1'),
                                              0, kernel='boaros')['status'], 'crash')

    def test_missing_and_duplicate_end_are_protocol_failures(self):
        raw = self.complete_stream()
        for broken in (raw.replace('INVENTORY END 6\n', ''),
                       raw + 'INVENTORY END 6\n',
                       raw.replace('INVENTORY BEGIN 1\n', ''),
                       raw.replace('INVENTORY STATUS true 0\n', '')):
            with self.subTest(broken=broken):
                self.assertEqual(inventory.run_status(broken, 0)['status'], 'protocol-error')

    def test_unknown_malformed_and_reordered_records_fail_run(self):
        raw = self.complete_stream()
        for broken in (raw.replace('INVENTORY END 6', 'INVENTORY OUTPUT unknown .\nINVENTORY END 6'),
                       raw.replace('INVENTORY OUTPUT true .', 'INVENTORY OUTPUT true f.'),
                       raw.replace('INVENTORY OUTPUT true .', 'INVENTORY OUTPUT true .\nINVENTORY OUTPUT true .'),
                       raw.replace('INVENTORY STATUS true 0', 'INVENTORY DRIVER_ERROR true 5')):
            with self.subTest(broken=broken):
                self.assertEqual(inventory.run_status(broken, 0)['status'], 'protocol-error')
        reordered = raw.replace('INVENTORY OUTPUT true .\nINVENTORY STATUS true 0',
                                'INVENTORY STATUS true 0\nINVENTORY OUTPUT true .')
        self.assertEqual(inventory.run_status(reordered, 0)['status'], 'protocol-error')
        self.assertEqual(inventory.observations('INVENTORY OUTPUT true f.\nINVENTORY STATUS true 0\n'), {})

    def test_preserves_nonzero_exit_and_exact_output(self):
        raw = 'INVENTORY OUTPUT false 00ff0a.\nINVENTORY STATUS false 256\n'
        self.assertEqual(inventory.observations(raw),
                         {'false': {'wait_status': 256, 'output_hex': '00ff0a'}})

    def test_missing_status_is_not_a_completed_case(self):
        self.assertEqual(inventory.observations('INVENTORY OUTPUT true .\n'), {})

    def test_duplicate_output_cannot_be_a_success(self):
        raw = 'INVENTORY OUTPUT true .\n' * 2 + 'INVENTORY STATUS true 0\n'
        self.assertEqual(inventory.observations(raw), {})

    def test_nonhex_or_unknown_case_is_not_accepted(self):
        raw = ('INVENTORY OUTPUT true not-hex.\nINVENTORY STATUS true 0\n'
               'INVENTORY OUTPUT unknown .\nINVENTORY STATUS unknown 0\n')
        self.assertEqual(inventory.observations(raw), {})


if __name__ == '__main__':
    unittest.main()
