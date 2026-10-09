import unittest
from cost_report import parse, schema, percentile, validate_expected, validate_replicas, validate_locking, validate_resume_selection

class CostReportTest(unittest.TestCase):
    def test_resume_selection_only_applies_to_consumer_manifests(self):
        for case in ('allocator','latency','locking'):
            validate_resume_selection(case, {}, ['consumer-musl-0'])
        validate_resume_selection('consumer', {'consumer_command_names':['consumer-musl-0']}, ['consumer-musl-0'])
        with self.assertRaisesRegex(ValueError,'selection'):
            validate_resume_selection('consumer', {}, ['consumer-musl-0'])
        with self.assertRaisesRegex(ValueError,'selection'):
            validate_resume_selection('metadata', {'consumer_command_names':['consumer-musl-0']}, ['consumer-musl-0'])

    def test_locking_checks_the_synchronous_contention_windows(self):
        names = {f'locking-{r}-0-{n}' for r in range(3) for n in (1,8,32)}
        names.update(f'locking-1-{op}-8' for op in (1,2,3,4))
        timings = {name: 100 for name in names}
        snapshots = [{'name':name,'values':{'foreground.lock15_acquired.value':1,
                     'foreground.lock15_blocks.value':int(name in ('locking-1-2-8','locking-1-4-8'))}}
                     for name in names]
        validate_locking(timings, snapshots, True)
        for name in ('locking-1-2-8','locking-1-4-8'):
            bad = [{**snap,'values':{**snap['values'],'foreground.lock15_blocks.value':0}}
                   if snap['name']==name else snap for snap in snapshots]
            with self.assertRaisesRegex(ValueError,'contention'):
                validate_locking(timings,bad,True)
        with self.assertRaisesRegex(ValueError,'coverage'):
            validate_locking(timings,snapshots[:-1],True)
        bad=[{**snap,'values':{**snap['values'],'foreground.lock15_acquired.value':0}}
             for snap in snapshots]
        with self.assertRaisesRegex(ValueError,'operation lock'):
            validate_locking(timings,bad,True)
        validate_locking(timings,[],False)

    def consumer_record(self):
        from cost_consumer import commands,ELFS,SCRIPTS,ORIGINAL_SHA,DEPENDENCIES
        lines=[]
        for libc in ELFS:
            for i in range(8):
                name=f'consumer-{libc}-{i}'
                lines += [f'COST COMMAND BEGIN {name}',
                          'Selected test not available on the version.' if i==7 else '',
                          self.consumer_method_output(i),f'COST RESULT {name} 123',f'COST COMMAND RESULT {name} 0 0']
        return dict(original_sha256=ORIGINAL_SHA,consumer_elf_sha256=ELFS,consumer_script_sha256=SCRIPTS,
                    consumer_dependencies=DEPENDENCIES,commands=commands('\n'.join(lines)+'\n'))

    def comparison_records(self):
        from copy import deepcopy
        import hashlib,json
        rows=[]
        # 比较输入的检错只需一个有效窗口，不依赖历史吞吐数据。
        for platform,on in (('boaros',0),('boaros',1),('linux',0)):
            for replica in range(3):
                row=deepcopy(self.consumer_record())
                row['commands']=row['commands'][:1]
                name=row['commands'][0]['name']
                row.update(case='consumer',platform=platform,cost_diagnostics=on,
                    transport='modern',cache='writeback',replica=replica,replicas=3,acceptance=True,
                    consumer_command_names=[name],consumer_timeout_ms=180000,
                    kernel_sha256=f'{platform}-{on}',elf_sha256='coordinator',source_sha256=platform,
                    fixture_sha256=f'{platform}-{on}-{replica}',firmware_sha256='firmware',
                    qemu_sha256='qemu',dtb_sha256='dtb',timebase_hz=10000000,
                    uname=dict(machine='riscv64',release='4.15.0'),snapshots=[],timings_ns={name:123})
                if on:
                    fields=self.valid();fields['epoch']='1'
                    row['snapshots']=[dict(name=name,values=parse(self.render(fields),1))]
                if platform=='linux':
                    row['linux_reference']=dict(inputs=dict(source=['linux','f4cdf7ca9a1fdcca413157df19753f388a5a224e']))
                row['input_keys']=['kernel_sha256','elf_sha256','source_sha256','fixture_sha256',
                                   'firmware_sha256','qemu_sha256','dtb_sha256','timebase_hz','consumer_timeout_ms']
                frozen={k:row[k] for k in row['input_keys']}
                row['input_sha256']=hashlib.sha256((json.dumps(frozen,sort_keys=True,separators=(',',':'))+'\n').encode()).hexdigest()
                rows.append(row)
        return rows

    def test_original_consumer_closure_rejects_startup_failure_and_censoring(self):
        from cost_consumer import validate_record,classify
        from copy import deepcopy
        row=self.consumer_record()
        validate_record(row,True)
        for raw,timeout,status in [('iozone test complete.',1,9),('FATAL: kernel too old',0,32512),
                                   ('still running',0,0)]:
            broken=deepcopy(row);c=broken['commands'][0]
            c.update(raw_output=raw,timeout=timeout,wait_status=status,**classify(raw,timeout,status))
            validate_record(broken,False)
            with self.assertRaises(ValueError):validate_record(broken,True)
        for field,value in [('timeout',1),('wait_status',32512),('raw_output','FATAL: kernel too old'),
                            ('command_timeout_ms',600000),('argv',['./iozone','--different-workload']),
                            ('outcome','blocked')]:
            broken=deepcopy(row);broken['commands'][0][field]=value
            with self.assertRaises(ValueError):validate_record(broken,True)
        broken=deepcopy(row);broken['consumer_dependencies']['/glibc/lib/libc.so.6']='wrong'
        with self.assertRaises(ValueError):validate_record(broken,True)

    @staticmethod
    def consumer_method_output(index):
        # Actual original report fields; completion alone is insufficient.
        methods=((),('initial writers','rewriters','readers','re-readers'),
                 ('initial writers','rewriters','random readers','random writers'),
                 ('initial writers','rewriters','reverse readers'),
                 ('initial writers','rewriters','stride readers'),
                 ('fwriters','freaders'),('pwrite writers','pread readers'),())
        data='4096 1 '+ ' '.join(['10']*13)+'\n' if index==0 else ''.join(
            'Children see throughput for 4 '+m+' = 40.00 kB/sec\n'
            'Max throughput per process = 10.00 kB/sec\n' for m in methods[index])
        return data+'iozone test complete.\n'

    def test_closure_requires_actual_requested_method_results(self):
        from cost_consumer import validate_record,classify
        from copy import deepcopy
        row=self.consumer_record()
        validate_record(row,True)
        for index in range(7):
            actual=row['commands'][index]['raw_output']
            missing=actual.replace('Max throughput per process','discarded throughput per process',1)
            if index==0:missing='iozone test complete.\n'
            valid=self.consumer_method_output(index)
            for raw in ('iozone test complete.\n',missing,valid+valid,
                        valid.replace('10','0'),valid.replace('10','nan'),valid.replace('10','inf')):
                broken=deepcopy(row);c=broken['commands'][index]
                c.update(raw_output=raw,reported_sections=[],**classify(raw,0,0))
                validate_record(broken,False)
                with self.assertRaises(ValueError):validate_record(broken,True)
        wrong=deepcopy(row);c=wrong['commands'][1]
        raw=self.consumer_method_output(2)
        c.update(raw_output=raw,**classify(raw,0,0))
        with self.assertRaises(ValueError):validate_record(wrong,True)

    def test_closure_rejects_different_comparison_inputs(self):
        import importlib.util,json,hashlib
        from copy import deepcopy
        spec=importlib.util.spec_from_file_location('closure','tests/iozone-closure.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        rows=self.comparison_records()
        m.validate(rows)
        for field,value in [('source_sha256','0'*64),('consumer_timeout_ms',1000000),
                            ('firmware_sha256','0'*64),('qemu_sha256','0'*64),('timebase_hz',20000000)]:
            broken=deepcopy(rows)
            for row in broken:
                if row['platform']=='boaros' and not row['cost_diagnostics']:
                    row[field]=value
                    if field=='consumer_timeout_ms':
                        for c in row['commands']:c['command_timeout_ms']=value
                    frozen={k:row[k] for k in row['input_keys']}
                    row['input_sha256']=hashlib.sha256((json.dumps(frozen,sort_keys=True,separators=(',',':'))+'\n').encode()).hexdigest()
            with self.subTest(field=field),self.assertRaises(ValueError):m.validate(broken)

    def test_consumer_budget_is_measured_policy_not_a_success_proxy(self):
        from cost_consumer import commands
        lines=['COST CONSUMER BUDGET 600000']
        for libc in ('musl','glibc'):
            for i in range(8):
                name=f'consumer-{libc}-{i}'
                lines += [f'COST COMMAND BEGIN {name}', 'iozone test complete.',
                          f'COST RESULT {name} 123', f'COST COMMAND RESULT {name} 1 9']
        output='\n'.join(lines)+'\n'
        rows=commands(output,expected_budget_ms=600000)
        self.assertEqual({r['command_timeout_ms'] for r in rows},{600000})
        self.assertTrue(all(r['reason']=='timeout' for r in rows))
        for invalid in (output.replace('600000','180000',1),
                        output.replace('600000','0',1),
                        output+'COST CONSUMER BUDGET 600000\n',
                        output.replace(lines[0]+'\n','')):
            with self.assertRaises(ValueError): commands(invalid,expected_budget_ms=600000)

    def test_summary_includes_windows_without_external_timing(self):
        import contextlib,importlib.util,io,json,tempfile
        from pathlib import Path
        from unittest.mock import patch
        spec=importlib.util.spec_from_file_location('summary','tests/cost-summary.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        snap=parse(self.render(self.valid()),3)
        rows=[dict(case='test',cost_diagnostics=1,transport='modern',cache='writeback',replica=i,
                   replicas=3,acceptance=True,kernel_sha256='k',elf_sha256='e',source_sha256='s',
                   fixture_sha256=str(i),snapshots=[dict(name='untimed',values=snap)],timings_ns={}) for i in range(3)]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'records.json';path.write_text(json.dumps(rows));output=io.StringIO()
            with patch('sys.argv',['cost-summary.py',str(path)]),contextlib.redirect_stdout(output):m.main()
        self.assertIn('untimed',output.getvalue())
        self.assertIn('uncovered ticks:',output.getvalue())

    def test_collect_fixture_inputs_preserves_frozen_identity(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('evidence','tests/cost-evidence.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        raw=dict(transport='modern',cache='writeback',source_tree='tree',kernel_sha256='kernel',
                 input_keys=['source_tree'],input_sha256='sealed',snapshots=[dict(name='pressure-io',
                 values=dict(mode='fixture',start_ticks=5,end_ticks=8,timebase_hz=10))])
        rows=m.collect([raw,raw,raw])
        self.assertEqual([r['replica'] for r in rows],[0,1,2])
        self.assertEqual(rows[0]['timings_ns'],{'pressure-io':300000000})
        self.assertEqual(rows[0]['input_keys'],raw['input_keys'])
        self.assertEqual(rows[0]['source_tree'],'tree')
        self.assertNotIn('case',raw)

    def test_consumer_per_command_completion(self):
        from cost_consumer import commands
        lines=[]
        for libc in ('musl','glibc'):
            for i in range(8):
                n=f'consumer-{libc}-{i}'
                lines.extend((f'COST COMMAND BEGIN {n}', 'iozone test complete.' if i==0 else 'failed before completion',f'COST RESULT {n} 1',f'COST COMMAND RESULT {n} 0 0'))
        text='\n'.join(lines)+'\n'
        rows=commands(text)
        self.assertEqual(sum(r['outcome']=='completed' for r in rows),2)
        for broken in (text.replace('COST COMMAND BEGIN consumer-musl-0\n',''),text+'COST COMMAND RESULT consumer-glibc-0 0 0\n'):
            with self.assertRaises(ValueError): commands(broken)

    def test_partial_consumer_output_keeps_timing_marker(self):
        from cost_consumer import framed_line
        marker='COST RESULT consumer-musl-0 180051153900'
        self.assertEqual(framed_line('            4096       1'+marker),marker)
        self.assertEqual(framed_line(marker),marker)
        self.assertEqual(framed_line('iozone ordinary output'),'iozone ordinary output')

    def test_unavailable_selected_test_is_not_acceptance(self):
        from cost_consumer import classify
        result=classify('Selected test not available on the version.\niozone test complete.\n',0,0)
        self.assertTrue(result['process_completed'])
        self.assertFalse(result['requested_tests_available'])
        self.assertEqual(result['outcome'],'blocked')
        self.assertEqual(result['reason'],'selected_tests_unavailable')

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

    def test_frozen_pre_allocator_schema_is_complete_without_zero_fill(self):
        current = schema()
        old = current[:next(i for i, metric in enumerate(current)
                            if metric[0] == 'page_meta_checked')]
        fields = self.valid()
        for lane in ('foreground', 'background', 'observer'):
            for name, _, _ in current[len(old):]:
                for key in list(fields):
                    if key.startswith(lane+'.'+name+'.'):
                        del fields[key]
        text = self.render(fields)
        parsed = parse(text, 3, old)
        self.assertNotIn('foreground.page_meta_checked.value', parsed)
        with self.assertRaises(ValueError): parse(text, 3)
        with self.assertRaises(ValueError): parse(text, 3, old[:-1])
        del fields['foreground.irq_off_ticks.max']
        with self.assertRaises(ValueError): parse(self.render(fields), 3, old)

    def test_frozen_pre_raw_schema_is_complete_without_zero_fill(self):
        current = schema()
        self.assertIn('raw_wait_ticks', {name for name, _, _ in current})
        old = current[:next(i for i, metric in enumerate(current) if metric[0] == 'raw_wait_ticks')]
        fields = self.valid()
        for lane in ('foreground', 'background', 'observer'):
            for name, _, _ in current[len(old):]:
                for key in list(fields):
                    if key.startswith(lane+'.'+name+'.'): del fields[key]
        parsed = parse(self.render(fields), 3, old)
        self.assertNotIn('foreground.raw_wait_ticks.value', parsed)
        with self.assertRaises(ValueError): parse(self.render(fields), 3)
        with self.assertRaises(ValueError): parse(self.render(fields), 3, old[:-1])

    def test_replicas_use_the_frozen_kernel_registry(self):
        from copy import deepcopy
        current=schema()
        old=[list(metric) for metric in current[:next(i for i,metric in enumerate(current)
                                                   if metric[0]=='page_meta_checked')]]
        rows=self.comparison_records()
        for row in rows:
            if not row['cost_diagnostics']:continue
            row['kernel_build_identity']={'kernel_sha256':row['kernel_sha256'],'metric_schema':old}
            for snapshot in row['snapshots']:
                for key in list(snapshot['values']):
                    if any('.'+metric+'.' in key for metric, _, _ in current[len(old):]):
                        del snapshot['values'][key]
        self.assertEqual(len(validate_replicas(rows)),3)
        bad=deepcopy(rows)
        target=next(row for row in bad if row['cost_diagnostics'])
        target['metric_schema']=[list(metric) for metric in current]
        with self.assertRaises(ValueError):validate_replicas(bad)
        target['metric_schema']=old
        target['kernel_build_identity']['kernel_sha256']='wrong'
        with self.assertRaises(ValueError):validate_replicas(bad)
        for row in rows:
            if row['cost_diagnostics']:
                row['metric_schema']=row['kernel_build_identity'].pop('metric_schema')
        self.assertEqual(len(validate_replicas(rows)),3)

    def test_allocator_requires_fixed_work_and_preserves_unavailable_metrics(self):
        from copy import deepcopy
        from cost_report import validate_allocator
        timings = {}; snapshots = []; lines = []
        for epoch, (pages, iterations) in enumerate(((1,1024),(64,64),(4096,4)), 1):
            name=f'allocator-{pages}-{iterations}'
            timings[name]=100
            lines.append(f'COST ALLOCATOR WORK {name} {pages} {iterations} {pages*iterations*90}')
            lines.append(f'COST CLOSING {name} 5 0')
            fields=self.valid(); fields['epoch']=str(epoch)
            for metric, value in [('page_accepted',pages*iterations),('irq_off_ticks',1),
                                  ('page_meta_checked',1),('page_meta_written',1),('allocator_meta_ticks',1)]:
                prefix='foreground.'+metric+'.'
                fields.update({prefix+'value':str(value),prefix+'max':str(value),prefix+'samples':'1'})
            fields['foreground.irq_off_ticks.bucket.1']='1'
            snapshots.append({'name':name,'values':parse(self.render(fields),epoch)})
        raw='\n'.join(lines)+'\n'
        self.assertTrue(validate_allocator(timings,snapshots,raw,True)['metadata_available'])
        for broken in (raw+lines[0]+'\n',raw.replace('92160','0'),raw.replace(lines[1]+'\n','')):
            with self.assertRaises(ValueError): validate_allocator(timings,snapshots,broken,True)
        old=deepcopy(snapshots)
        for snapshot in old:
            for key in list(snapshot['values']):
                if any('.'+metric+'.' in key for metric in
                       ('page_meta_checked','page_meta_written','allocator_meta_ticks')):
                    del snapshot['values'][key]
        self.assertFalse(validate_allocator(timings,old,raw,True)['metadata_available'])
        broken=deepcopy(snapshots)
        broken[0]['values']['foreground.page_meta_checked.value']=0
        with self.assertRaises(ValueError): validate_allocator(timings,broken,raw,True)
        with self.assertRaises(ValueError): validate_allocator(timings,snapshots[:-1],raw,True)
        off='\n'.join(line for line in lines if not line.startswith('COST CLOSING '))+'\n'
        self.assertFalse(validate_allocator(timings,[],off,False)['metadata_available'])
    def test_frozen_pre_resize_schema(self):
        current=schema()
        old=current[:next(i for i,metric in enumerate(current) if metric[0]=='resize_visits')]
        self.assertFalse(any(name.startswith('resize_') for name,_,_ in old))
        fields=self.valid()
        added={name for name,_,_ in schema()[len(old):]}
        self.assertTrue({'resize_visits','resize_tail_pages','resize_alias_rearms'} <= added)
        fields={key:value for key,value in fields.items()
                if not any(key.startswith(lane+'.'+name+'.')
                    for lane in ('foreground','background','observer') for name in added)}
        self.assertEqual(parse(self.render(fields),3,old)['epoch'],3)
        with self.assertRaises(ValueError):parse(self.render(fields),3)

    def test_frozen_pre_memory_schema(self):
        current=schema()
        old=current[:next(i for i,metric in enumerate(current) if metric[0]=='heap_zero_bytes')]
        self.assertEqual(len(old),203)
        fields=self.valid()
        added={name for name,_,_ in schema()[len(old):]}
        fields={key:value for key,value in fields.items()
                if not any(key.startswith(lane+'.'+name+'.')
                    for lane in ('foreground','background','observer') for name in added)}
        self.assertEqual(parse(self.render(fields),3,old)['epoch'],3)
        with self.assertRaises(ValueError):parse(self.render(fields),3)

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
    def test_impossible_max_and_reused_epoch(self):
        fields=self.valid();p='foreground.irq_off_ticks.'
        fields.update({p+'samples':'2',p+'value':'11',p+'max':'3',p+'bucket.2':'1',p+'bucket.4':'1'})
        with self.assertRaises(ValueError):parse(self.render(fields),3)
        fields=self.valid();p='foreground.irq_off_ticks.';fields.update({p+'samples':'2',p+'value':'15',p+'max':'8',p+'bucket.2':'1',p+'bucket.4':'1'})
        with self.assertRaises(ValueError):parse(self.render(fields),3)
        fields=self.valid();p='foreground.operations.';fields.update({p+'samples':'1',p+'value':'100',p+'max':'1'})
        with self.assertRaises(ValueError):parse(self.render(fields),3)
        snap=parse(self.render(self.valid()),3)
        rows=[dict(case='test',cost_diagnostics=1,transport='modern',cache='writeback',replica=i,replicas=3,acceptance=True,kernel_sha256='k',elf_sha256='e',source_sha256='s',fixture_sha256=str(i),snapshots=[dict(name='first',values=snap),dict(name='second',values=snap)],timings_ns={'first':1,'second':1}) for i in range(3)]
        with self.assertRaises(ValueError):validate_replicas(rows)
    def test_archive_detects_removed_counter(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location('evidence','tests/cost-evidence.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        snap=parse(self.render(self.valid()),3)
        snap.update({'foreground.operations.value':7,'foreground.operations.samples':1,'foreground.operations.max':7})
        rows=[dict(case='test',cost_diagnostics=1,transport='modern',cache='writeback',replica=i,replicas=3,acceptance=True,kernel_sha256='k',elf_sha256='e',source_sha256='s',fixture_sha256=str(i),snapshots=[dict(name='test',values=snap)],timings_ns={'test':1}) for i in range(3)]
        with self.assertRaises(ValueError):m.pack(rows,final=True)
        packed=m.pack(rows);self.assertEqual(m.unpack(packed),rows)
        with self.assertRaises(ValueError):m.unpack(packed,final=True)
        del packed['records'][0]['snapshots'][0]['counters']['foreground.operations']
        with self.assertRaises(ValueError):m.unpack(packed)
    def test_deadline_work_tracks_expirations(self):
        from cost_report import validate_deadline
        snap=parse(self.render(self.valid()),3)
        # 到期扫描的工作量必须与到期数一致，不再随无期限阻塞成员增长。
        for visits,expired in ((0,0),(4,0),(131,4),(4,136)):
            snap['foreground.deadline_visits.samples']=1 if (visits or expired) else 0
            snap['foreground.deadline_visits.value']=visits
            snap['foreground.deadline_visits.max']=visits
            snap['foreground.deadline_expired.value']=expired
            with self.assertRaises(ValueError):validate_deadline('deadline-128-0-0',snap)
        snap['foreground.deadline_visits.value']=136
        snap['foreground.deadline_expired.value']=136
        snap['foreground.deadline_visits.max']=136
        validate_deadline('deadline-128-0-0',snap)

    def test_bucket_intervals(self):
        bins=[0]*65; bins[0]=1; bins[5]=2; bins[64]=1
        self.assertEqual(percentile(bins, .5),(16,31))
        self.assertEqual(percentile(bins,1),(1<<63,(1<<64)-1))
        self.assertEqual(percentile([0]*65,.5),None)

if __name__ == '__main__': unittest.main()
