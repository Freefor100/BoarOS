#!/usr/bin/env python3
"""Accept a valid shared runtime cache, reject independent identity changes."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/loongarch'))
import prepare_dynamic as subject

class Cache(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='la-dynamic-cache-',dir=ROOT/'build/host')
        self.base=Path(self.temporary.name);shutil.copytree(subject.BASE/'root',self.base/'root',symlinks=True)
        self.args=argparse.Namespace(cross='loongarch64-unknown-linux-gnu-',jobs=1,output=self.base)
        self.flags=list(subject.FLAGS)
        stamp={'inputs':subject.cache_inputs(self.args)[0],'products':subject.static.installed_tree_manifest(self.base,['root'])}
        self.stamp=self.base/'identity.json';self.stamp.write_text(json.dumps(stamp));self.call()
    def tearDown(self):
        subject.FLAGS[:]=self.flags;self.temporary.cleanup()
    def call(self):
        with contextlib.redirect_stdout(io.StringIO()):subject.main(self.args)
    def rejected(self):
        with self.assertRaisesRegex(SystemExit,'identity|missing'):self.call()
    def modify(self,path):
        file=self.base/'root'/path;file.write_bytes(file.read_bytes()+b'changed');self.rejected()
    def test_loader(self):self.modify('lib/libc.so')
    def test_headers(self):self.modify('include/bits/fenv.h')
    def test_specs(self):self.modify('lib/musl-gcc.specs')
    def test_missing_crt(self):(self.base/'root/lib/Scrt1.o').unlink();self.rejected()
    def test_mode(self):
        file=self.base/'root/bin/musl-gcc';file.chmod(file.stat().st_mode^0o111);self.rejected()
    def test_link(self):
        file=self.base/'root/lib/ld-musl-loongarch64.so.1';file.unlink();file.symlink_to('missing');self.rejected()
    def test_abi(self):subject.FLAGS[0]='-mabi=lp64s';self.rejected()
    def wrong_input(self,field,key):
        stamp=json.loads(self.stamp.read_text());stamp['inputs'][field][key]='wrong';self.stamp.write_text(json.dumps(stamp));self.rejected()
    def test_frontend_identity(self):self.wrong_input('frontend','sha256')
    def test_runtime_identity(self):
        stamp=json.loads(self.stamp.read_text());stamp['inputs']['runtime']['crtbeginS.o']['sha256']='wrong';self.stamp.write_text(json.dumps(stamp));self.rejected()
if __name__=='__main__':unittest.main()
