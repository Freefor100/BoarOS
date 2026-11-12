"""Freeze prepared fixture bytes, before any guest can modify them."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import hashlib
import json
import shutil
import tempfile


class FrozenFixture:
    def __init__(self, directory, case, elf_sha256, configuration=None):
        if case != 'ipc-fds':
            raise ValueError('unsupported frozen fixture case')
        self.directory = Path(directory) / case
        self.case = case
        self.elf_sha256 = elf_sha256
        self.configuration = configuration
        self.sha256 = None

    @staticmethod
    def digest(path):
        with path.open('rb') as source:
            return hashlib.file_digest(source, 'sha256').hexdigest()

    def load(self):
        if not self.directory.exists():
            return None
        image = self.directory / 'root.img'
        try:
            record = json.loads((self.directory / 'input.json').read_text())
            valid = (record['version'] == 1 and record['case'] == self.case and
                     record['elf_sha256'] == self.elf_sha256 and
                     record.get('configuration') == self.configuration and
                     image.stat().st_size == record['bytes'] and
                     self.digest(image) == record['fixture_sha256'])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise ValueError('incomplete or invalid frozen fixture') from error
        if not valid:
            raise ValueError('frozen fixture identity differs')
        self.sha256 = record['fixture_sha256']
        return image

    def publish(self, source):
        if self.directory.exists():
            raise ValueError('frozen fixture already published')
        self.directory.parent.mkdir(parents=True, exist_ok=True)
        # Publish bytes and identity together; failed preparation never becomes a cache hit.
        with tempfile.TemporaryDirectory(prefix='.fixture-', dir=self.directory.parent) as temporary:
            staging = Path(temporary) / 'published'; staging.mkdir()
            image = staging / 'root.img'
            shutil.copyfile(source, image)
            record = dict(version=1, case=self.case, elf_sha256=self.elf_sha256,
                          bytes=image.stat().st_size, fixture_sha256=self.digest(image))
            if self.configuration is not None:
                record['configuration'] = self.configuration
            manifest = staging / 'input.json'
            manifest.write_text(json.dumps(record, sort_keys=True) + '\n')
            image.chmod(0o444); manifest.chmod(0o444)
            staging.rename(self.directory)
        self.sha256 = record['fixture_sha256']
