#!/usr/bin/env python3
"""Check complete timed work and reject measurements with changed semantics."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('accounting_cost',HERE/'cost.py')
cost=importlib.util.module_from_spec(spec);spec.loader.exec_module(cost)

class CostTests(unittest.TestCase):
    def test_legacy_measurements_keep_workload_checks(self):
        raw=('ACCOUNT COST compute ns=10 check=3953732515821365571\n'
             'ACCOUNT COST tmpfs-small-io ns=20 check=640000\n'
             'ACCOUNT COST sqlite-memory ns=30 check=1250025000\n'
             'ACCOUNT COST sqlite-commit ns=40 check=12502540\nACCOUNT END 4\n')
        self.assertEqual(cost.measurements(raw),{'compute':10,'tmpfs-small-io':20,'sqlite-memory':30,'sqlite-commit':40})
        with self.assertRaises(RuntimeError):cost.measurements(raw.replace('check=12502540','check=0'))

    def test_timer_parser_preserves_cpu_and_signal_measurements(self):
        raw=('ACCOUNT TIMER BEGIN mode=off cpus=1\n'
             'ACCOUNT TIMER getpid wall_ns=1000000 user_ns=200000 system_ns=700000 signals=0 check=1000000 workers=1 cpu_mask=1 overlap=0\n'
             'ACCOUNT TIMER END 1\n')
        row=cost.timer_measurements(raw,'off',['getpid'],1)['getpid']
        self.assertEqual(row,dict(wall_ns=1000000,user_ns=200000,system_ns=700000,
                                 signals=0,check=1000000,workers=1,cpu_mask=1,overlap=0))
        for broken in (raw.replace('signals=0','signals=1'),raw.replace('check=1000000','check=999999'),
                       raw.replace('ACCOUNT TIMER END 1',''),raw+raw,
                       raw.replace('cpu_mask=1','cpu_mask=3')):
            with self.assertRaises(RuntimeError):cost.timer_measurements(broken,'off',['getpid'],1)

    def test_armed_and_periodic_measurements_enforce_different_signal_behavior(self):
        row=('ACCOUNT TIMER BEGIN mode=prof-periodic cpus=1\n'
             'ACCOUNT TIMER getpid wall_ns=200000000 user_ns=20000000 system_ns=140000000 signals=7 check=1000000 workers=1 cpu_mask=1 overlap=0\n'
             'ACCOUNT TIMER END 1\n')
        self.assertEqual(cost.timer_measurements(row,'prof-periodic',['getpid'],1)['getpid']['signals'],7)
        with self.assertRaises(RuntimeError):cost.timer_measurements(row.replace('signals=7','signals=0'),'prof-periodic',['getpid'],1)
        with self.assertRaises(RuntimeError):cost.timer_measurements(row.replace('prof-periodic','prof-armed'),'prof-armed',['getpid'],1)

    def test_run_order_rotates_without_dropping_control_or_modes(self):
        first=cost.timer_run_plan(['before','after','control'],'after',0)
        second=cost.timer_run_plan(['before','after','control'],'after',1)
        self.assertEqual(set(first),{('before','off'),('control','off'),('after','off'),
            ('after','virtual-armed'),('after','prof-armed'),('after','virtual-periodic'),('after','prof-periodic')})
        self.assertEqual(len(first),7);self.assertEqual(second,first[1:]+first[:1])

    def test_native_workload_executes_requested_mode_and_fixed_work(self):
        with tempfile.TemporaryDirectory(prefix='boaros-cost-') as folder:
            binary=Path(folder)/'cost'
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-pthread',str(HERE/'cost.c'),'-o',str(binary)],check=True)
            for mode in ('off','virtual-armed','prof-armed','prof-periodic'):
                done=subprocess.run([str(binary),'--timer-mode',mode,'--workload','getpid'],capture_output=True,text=True,timeout=15)
                self.assertEqual(done.returncode,42,done.stdout+done.stderr)
                rows=cost.timer_measurements(done.stdout,mode,['getpid'],1)
                self.assertEqual(rows['getpid']['check'],1000000)
                self.assertGreater(rows['getpid']['user_ns']+rows['getpid']['system_ns'],0)
            for mode,name,cpus in (('virtual-periodic','compute',1),('prof-periodic','compute-threads',1),
                                   ('prof-periodic','compute-processes',1),('off','compute-threads',2),
                                   ('off','compute-processes',2)):
                done=subprocess.run([str(binary),'--timer-mode',mode,'--workload',name,'--cpus',str(cpus)],capture_output=True,text=True,timeout=15)
                self.assertEqual(done.returncode,42,done.stdout+done.stderr)
                rows=cost.timer_measurements(done.stdout,mode,[name],cpus)
                self.assertGreater(rows[name]['user_ns'],100000000)
                if mode.endswith('-periodic'):self.assertGreater(rows[name]['signals'],0)
            bad=subprocess.run([str(binary),'--timer-mode','invalid'],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(bad.returncode,42)

if __name__=='__main__':unittest.main()
