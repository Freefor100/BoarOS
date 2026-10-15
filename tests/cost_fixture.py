"""Freeze prepared fixture bytes, before any guest can modify them."""
import hashlib
import json
from pathlib import Path
import shutil
import tempfile


def validate_resume_fixture(record, requested_sha256):
    expected_mode = 'fresh' if requested_sha256 is None else 'frozen'
    if record.get('fixture_mode', 'fresh') != expected_mode:
        raise ValueError('resume fixture mode differs')
    if expected_mode == 'frozen':
        if (record.get('fixture_input_sha256') != requested_sha256 or
                record.get('fixture_sha256') != requested_sha256):
            raise ValueError('resume frozen fixture differs')
    elif record.get('fixture_input_sha256') is not None:
        raise ValueError('unexpected frozen fixture identity')


def writable_root_path(record):
    argv = record.get('argv', [])
    roots = []
    for index, argument in enumerate(argv):
        if argument != '-drive' or index + 1 >= len(argv):
            continue
        fields = argv[index + 1].split(',')
        if 'id=root' in fields:
            files = [field[5:] for field in fields if field.startswith('file=')]
            if len(files) != 1 or not files[0]:
                raise ValueError('invalid frozen fixture drive')
            roots.append(str(Path(files[0]).resolve()))
    if len(roots) != 1:
        raise ValueError('one writable root fixture required')
    return roots[0]


class FrozenFixture:
    def __init__(self, directory, case, elf_sha256):
        if case not in ('allocator', 'latency', 'locking', 'scheduler'):
            raise ValueError('unsupported frozen fixture case')
        self.directory = Path(directory) / case
        self.case = case
        self.elf_sha256 = elf_sha256
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
        # Serial measurement lock excludes other publishers. Publish the image
        # and identity together; failed preparation never becomes a cache hit.
        with tempfile.TemporaryDirectory(prefix='.fixture-', dir=self.directory.parent) as temporary:
            staging = Path(temporary) / 'published'; staging.mkdir()
            image = staging / 'root.img'
            shutil.copyfile(source, image)
            record = dict(version=1, case=self.case, elf_sha256=self.elf_sha256,
                          bytes=image.stat().st_size, fixture_sha256=self.digest(image))
            manifest = staging / 'input.json'
            manifest.write_text(json.dumps(record, sort_keys=True) + '\n')
            image.chmod(0o444); manifest.chmod(0o444)
            staging.rename(self.directory)
        self.sha256 = record['fixture_sha256']
