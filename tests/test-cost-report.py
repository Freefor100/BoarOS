import unittest
from cost_report import parse, schema, percentile, validate_expected, validate_replicas

class CostReportTest(unittest.TestCase):
    def valid(self):
        fields = dict(irq_user_prefix_instructions='7',irq_supervisor_prefix_instructions='6',irq_sret_suffix_instructions='11',irq_c_enable_suffix_min_instructions='9',version='1', epoch='3', state='complete', mode='user', owner='2',
            timebase_hz='10000000', resolution_ns_numerator='1000000000',
            resolution_ns_denominator='10000000', start_ticks='100', end_ticks='200',
            storage_bytes='4096', task_bytes='40', overflow='0', inflight='0')
        for lane in ('foreground', 'background', 'observer'):
            for name, unit, hist in schema():
                prefix = lane + '.' + name + '.'
                fields.update({prefix+'unit':unit, prefix+'value':'0', prefix+'samples':'0', prefix+'max':'0'})
                if hist:
                    fields.update({prefix+'bucket.'+str(i):'0' for i in range(65)})
        return fields
    def render(self, fields): return ''.join(k+'='+v+'\n' for k,v in fields.items())
    def test_valid(self): self.assertEqual(parse(self.render(self.valid()), 3)['epoch'], 3)
    def test_reject_corrupt(self):
        for key, value in [('version','2'), ('epoch','2'), ('state','incomplete'),
            ('overflow','1'), ('inflight','1'), ('task_bytes','65'), ('storage_bytes','65537'),
            ('foreground.operations.unit','bytes'), ('foreground.operations.value','-1'),
            ('foreground.operation_ticks.samples','1')]:
            with self.subTest(key=key):
                fields=self.valid(); fields[key]=value
                with self.assertRaises(ValueError): parse(self.render(fields), 3)
    def test_missing_duplicate_unknown(self):
        text=self.render(self.valid())
        for broken in (text.replace('epoch=3\n',''),text+'epoch=3\n',text+'madeup=0\n'):
            with self.assertRaises(ValueError): parse(broken,3)
    def test_independent_expected(self):
        snapshot=parse(self.render(self.valid()),3)
        for expected in ({'foreground.operations.value':128}, {'foreground.run_ticks.value':99}, {'foreground.mprotect_resident_visits.value':8192}, {'foreground.mprotect_pte_visits.value':6}):
            with self.assertRaises(ValueError): validate_expected(snapshot, expected)
        snapshot['background.operations.value']=128
        with self.assertRaises(ValueError): validate_expected(snapshot, {'foreground.operations.value':128})
    def test_replica_completeness(self):
        def row(i): return dict(case='write',cost_diagnostics=0,transport='modern',cache='writeback',
            replica=i,replicas=3,acceptance=True,kernel_sha256='k',elf_sha256='e',
            source_sha256='s',fixture_sha256=str(i),snapshots=[],timings_ns={'small':1})
        validate_replicas([row(i) for i in range(3)])
        for rows in ([row(0)], [row(0),row(0),row(2)], [row(0),row(1),row(2),row(3)]):
            with self.assertRaises(ValueError): validate_replicas(rows)
        rows=[row(i) for i in range(3)]; rows[2]['elf_sha256']='changed'
        with self.assertRaises(ValueError): validate_replicas(rows)
    def test_bucket_intervals(self):
        bins=[0]*65; bins[0]=1; bins[5]=2; bins[64]=1
        self.assertEqual(percentile(bins, .5),(16,31))
        self.assertEqual(percentile(bins,1),(1<<63,(1<<64)-1))
        self.assertEqual(percentile([0]*65,.5),None)

if __name__ == '__main__': unittest.main()
