#!/usr/bin/env python3
"""Cache identity counterexamples exercise the real userland preparation entry."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/loongarch'))
import prepare_userland as subject

class CacheContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original_root=subject.ROOT;cls.original_base=subject.BASE
        cls.old=json.loads((subject.BASE/'userland-identity.json').read_text())
        cls.template=tempfile.TemporaryDirectory(prefix='la-cache-template-',dir=ROOT/'build/host')
        cls.tree=Path(cls.template.name)
        for name in ('gcc-sf/root','musl-root','uapi'):
            shutil.copytree(subject.BASE/name,cls.tree/name,symlinks=True)
        for original in ('busybox-source/busybox/busybox','busybox-source/busybox/.config'):
            target=cls.tree/original;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(subject.BASE/original,target)
        profile=json.loads((ROOT/'tests/program-inventory/inputs.json').read_text())['busybox']
        original_config=subject.BASE/'busybox-source'/profile['config']
        target=cls.tree/original_config.relative_to(subject.BASE);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original_config,target)
    @classmethod
    def tearDownClass(cls): cls.template.cleanup()
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='la-cache-test-',dir=ROOT/'build/host')
        self.area=Path(self.temporary.name);self.base=self.area/'products'
        shutil.copytree(self.tree,self.base,symlinks=True)
        subject.BASE=self.base;subject.ROOT=self.area/'metadata';subject.ROOT.mkdir()
        refs=subject.ROOT/'references';refs.mkdir()
        shutil.copy2(ROOT/'references/sources.tsv',refs/'sources.tsv')
        for name in ('gcc/gcc-15.1.0.tar.xz','musl/musl-1.2.5.tar.gz','linux-uapi/linux-6.6.tar.xz'):
            target=refs/name;target.parent.mkdir(exist_ok=True);target.symlink_to(ROOT/'references'/name)
        (refs/'oscomp-testsuits').symlink_to(ROOT/'references/oscomp-testsuits',target_is_directory=True)
        inputs=subject.ROOT/'tests/program-inventory';inputs.mkdir(parents=True)
        shutil.copy2(ROOT/'tests/program-inventory/inputs.json',inputs/'inputs.json')
        stamp=json.loads(json.dumps(self.old))
        if not hasattr(subject,'cache_inputs'):
            stamp['products']={str(self.base/Path(p).relative_to(self.original_base)):h for p,h in stamp['products'].items()}
        self.saved_flags=list(subject.FLAGS)
        self.args=argparse.Namespace(cross='loongarch64-unknown-linux-gnu-',jobs=1)
        if hasattr(subject,'cache_inputs'):
            stamp['inputs']=subject.cache_inputs(self.args)[0]
            stamp['products']=subject.installed_manifest(self.base)
        (self.base/'userland-identity.json').write_text(json.dumps(stamp))
        self.call()  # A valid cache must hit before each single counterexample.
    def tearDown(self):
        subject.ROOT=self.original_root;subject.BASE=self.original_base;subject.FLAGS[:]=self.saved_flags;self.temporary.cleanup()
    def call(self):
        with contextlib.redirect_stdout(io.StringIO()): subject.main(self.args)
    def rejected(self):
        with self.assertRaisesRegex(SystemExit,'identity|changed|missing|checksum'): self.call()
    def test_busybox_revision_change(self):
        path=subject.ROOT/'tests/program-inventory/inputs.json';data=json.loads(path.read_text());data['busybox']['revision']='0'*40;path.write_text(json.dumps(data));self.rejected()
    def test_busybox_configuration_selection_change(self):
        path=subject.ROOT/'tests/program-inventory/inputs.json';data=json.loads(path.read_text());data['busybox']['config']='config/another-config';path.write_text(json.dumps(data));self.rejected()
    def test_uapi_archive_identity_change(self):
        path=subject.ROOT/'references/sources.tsv';text=path.read_text();rows=text.splitlines();rows=[line.rsplit('\t',1)[0]+'\t'+'0'*64 if '\tlinux-uapi/linux-6.6.tar.xz\t' in line else line for line in rows];path.write_text('\n'.join(rows)+'\n');self.rejected()
    def test_missing_cc1(self):
        helper=next((self.base/'gcc-sf/root/libexec').rglob('cc1'));helper.unlink();self.rejected()
    def test_missing_collect2(self):
        helper=next((self.base/'gcc-sf/root/libexec').rglob('collect2'));helper.unlink();self.rejected()
    def test_changed_specs(self):
        path=self.base/'musl-root/lib/musl-gcc.specs';path.write_text(path.read_text()+'\n# altered\n');self.rejected()
    def test_changed_header(self):
        path=self.base/'musl-root/include/stdio.h';path.write_text(path.read_text()+'\n/* altered */\n');self.rejected()
    def test_changed_user_flags(self):
        subject.FLAGS[0]='-mabi=lp64d';self.rejected()
    def test_changed_tool_symlink(self):
        path=self.base/'gcc-sf/root/bin/loongarch64-unknown-linux-gnusf-as';path.unlink();path.symlink_to('/bin/true');self.rejected()
if __name__=='__main__': unittest.main()
