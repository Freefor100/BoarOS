"""Repair only identified ELF instructions; incomplete guest evidence must fail."""
import hashlib
import struct
import unittest

from user_runtime_probe import repair_instruction, brk_observations, native_statuses, edit, cyclic_group


class RuntimeRepairTests(unittest.TestCase):
    def elf(self):
        data = bytearray(256)
        data[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HHIQQQIHHHHHH', data, 16,
                         3, 243, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 128, 0x1000, 0, 128, 128, 4096)
        data[144:146] = bytes.fromhex('0125')
        return bytes(data)

    def repair(self, data, **changes):
        self.assertIsNotNone(repair_instruction, 'identified ELF repair is missing')
        args = dict(expected_sha=hashlib.sha256(data).hexdigest(), machine=243,
                    address=0x1010, before=bytes.fromhex('0125'), after=bytes.fromhex('0100'))
        args.update(changes)
        return repair_instruction(data, **args)

    def test_preserves_elf_and_all_bytes_except_the_identified_instruction(self):
        original = self.elf()
        repaired = self.repair(original)
        self.assertEqual(repaired, original[:144] + bytes.fromhex('0100') + original[146:])
        self.assertEqual(original[144:146], bytes.fromhex('0125'))

    def test_refuses_unknown_identity_wrong_machine_or_changed_instruction(self):
        data = self.elf()
        for changes in [{'expected_sha': '0'*64}, {'machine': 258},
                        {'before': b'xx'}, {'after': b'\x00'}]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.repair(data, **changes)

    def test_rejects_unbacked_and_ambiguous_load_addresses(self):
        data = self.elf()
        with self.assertRaises(ValueError):
            self.repair(data, address=0x1090)
        broken = bytearray(data)
        struct.pack_into('<H', broken, 56, 2)
        broken[120:176] = broken[64:120]
        with self.assertRaises(ValueError):
            self.repair(bytes(broken), address=0x1000)

    def test_brk_evidence_requires_high_address_and_preserved_adapted_return(self):
        self.assertIsNotNone(brk_observations, 'raw/user brk comparison is missing')
        trace = ('BRK CALL requested=0\nBRK RAW mode=adapted value=3564c2e000\n'
                 'BRK USER value=3564c2e000\n')
        self.assertEqual(brk_observations(trace)[0]['raw'], 0x3564c2e000)
        for bad in [trace.replace('3564c2e000', '64c2e000'),
                    trace.replace('BRK USER value=3564c2e000', 'BRK USER value=64c2e000'),
                    trace.replace('BRK USER value=3564c2e000\n', '')]:
            with self.subTest(trace=bad), self.assertRaises(ValueError): brk_observations(bad)

    def test_native_failure_and_missing_wait_cannot_become_success(self):
        self.assertIsNotNone(native_statuses, 'native wait validation is missing')
        expected = {'original': 256, 'adapted': 0}
        text = 'RUNTIME WAIT original status=256\nRUNTIME WAIT adapted status=0\n'
        self.assertEqual(native_statuses(text, expected), expected)
        for bad in [text.replace('adapted status=0', 'adapted status=256'),
                    text.splitlines()[0], text + 'RUNTIME WAIT adapted status=0\n',
                    text + 'RUNTIME EXEC adapted errno=2\n']:
            with self.subTest(text=bad), self.assertRaises(ValueError): native_statuses(bad, expected)

    def test_reference_disk_is_never_a_writable_diagnostic_target(self):
        from pathlib import Path
        import tempfile
        self.assertIsNotNone(edit)
        with tempfile.TemporaryDirectory() as directory:
            disk=Path(directory)/'reference.img'
            disk.write_bytes(b'fixture reference contents')
            with self.assertRaises(ValueError): edit(disk,'mkdir /must-not-be-created')
            self.assertEqual(disk.read_bytes(),b'fixture reference contents')

    def test_cyclic_group_checks_child_exit_and_keeps_zero_samples_visible(self):
        self.assertIsNotNone(cyclic_group)
        text=''
        for name,counts in [('NO_STRESS_P1',[1]),('NO_STRESS_P8',[1]*8),
                            ('STRESS_P1',[1]),('STRESS_P8',[1]*7+[0])]:
            text+=f'====== cyclictest {name} begin ======\n'
            text+=''.join(f'T: {i} P:99 C: {count} Max: 5\n' for i,count in enumerate(counts))
            text+=f'====== cyclictest {name} end: success ======\n'
        observed=cyclic_group(text)
        self.assertFalse(observed['all_threads_sampled'])
        self.assertEqual(observed['cases']['STRESS_P8']['zero_sample_threads'],[7])
        for bad in [text.replace('end: success','end: fail',1),
                    text.replace('====== cyclictest STRESS_P8 end: success ======\n','')]:
            with self.subTest(text=bad),self.assertRaises(ValueError):cyclic_group(bad)


if __name__ == '__main__': unittest.main()
