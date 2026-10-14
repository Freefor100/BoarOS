import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('instruction_sync', Path(__file__).with_name('instruction-sync.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class InstructionSyncTests(unittest.TestCase):
    def graph(self, body):
        return module.parse_disassembly(body)

    def test_reachable_helper_and_local_debug_labels(self):
        graph = self.graph('''Disassembly of section .text.entry:
0000000000000000 <entry>:
0000000000000010 <.LM12>:
  10: 00000097 auipc ra,0
  10: R_RISCV_CALL_PLT helper
Disassembly of section .text.helper:
0000000000000000 <helper>:
   0: 0000100f fence.i
''')
        self.assertTrue(module.reaches_fence(graph, 'entry'))

    def test_unreachable_fence_does_not_satisfy_entry(self):
        graph = self.graph('''Disassembly of section .text.entry:
0000000000000000 <entry>:
   0: R_RISCV_CALL_PLT missing
Disassembly of section .text.unrelated:
0000000000000000 <unrelated>:
   0: 0000100f fence.i
''')
        self.assertFalse(module.reaches_fence(graph, 'entry'))
        self.assertFalse(module.reaches_fence(graph, 'absent'))

    def test_cycles_and_noncall_relocations_do_not_supply_fence(self):
        graph = self.graph('''Disassembly of section .text.entry:
0000000000000000 <entry>:
   0: R_RISCV_CALL_PLT next
   4: R_RISCV_64 unrelated
Disassembly of section .text.next:
0000000000000000 <next>:
   0: R_RISCV_CALL entry
Disassembly of section .text.unrelated:
0000000000000000 <unrelated>:
   0: 0000100f fence.i
''')
        self.assertFalse(module.reaches_fence(graph, 'entry'))

if __name__ == '__main__':
    unittest.main()
