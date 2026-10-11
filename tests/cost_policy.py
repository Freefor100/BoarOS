"""Fixed CPU 2/3 frequency-policy lease for matched COST experiments.

The owning controller must keep stdin open until measurement ends. EOF, supported
signals, failed configuration and broken output all leave through restoration.
Only governor, min/max and a governor-modified EPP can be changed; this helper
never invokes commands, changes global turbo/SMT, or runs a supplied workload.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import stat
import sys
import time


SYSFS = Path('/sys/devices/system/cpu')
CPUS = (2, 3)
TARGET_KHZ = 2500000
READBACK_TIMEOUT = 1.0
READBACK_INTERVAL = 0.01
REQUIRED = ('scaling_governor', 'scaling_min_freq', 'scaling_max_freq')
EPP = 'energy_performance_preference'


class FileBackend:
    def read(self, path):
        return path.read_text().strip()

    def writable(self, path):
        return bool(path.stat().st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)) and os.access(path, os.W_OK)

    def write(self, path, value):
        # 每项单独写入并读回；文件替换或临时文件不适用于 sysfs 属性。
        with path.open('w') as stream:
            stream.write(value + '\n')


def cpu_set(text):
    result = set()
    for group in text.replace(',', ' ').split():
        span = group.split('-')
        if len(span) == 1:
            result.add(int(span[0]))
        elif len(span) == 2 and 0 <= int(span[0]) <= int(span[1]):
            result.update(range(int(span[0]), int(span[1]) + 1))
        else:
            raise ValueError('invalid CPU set')
    if not result or any(cpu < 0 for cpu in result):
        raise ValueError('empty/negative CPU set')
    return result


class PolicyError(RuntimeError):
    def __init__(self, message, restoration=None):
        super().__init__(message)
        self.restoration = restoration


class PolicyLease:
    """Own saved policy values from acquisition through verified restoration.

    Alternate sysfs/backend arguments are an in-process host-test API. The CLI
    deliberately exposes neither, so a privileged helper has no path/CPU input.
    Acquisition snapshots all policies before changing the first one. Failed
    writes are treated as potentially mutating and included in restoration.
    """

    def __init__(self, sysfs=Path('/sys/devices/system/cpu'), *, backend=None):
        self.sysfs = Path(sysfs)
        self.backend = backend or FileBackend()
        self.policies = {}
        self.original = {}
        self.requested = {}
        self._expected = {}
        self._modified = set()
        self._active = False

    def _discover(self):
        policies = {}
        root = (self.sysfs / 'cpufreq').resolve(strict=True)
        for cpu in CPUS:
            path = (self.sysfs / f'cpu{cpu}' / 'cpufreq').resolve(strict=True)
            if path.parent != root or not path.name.startswith('policy'):
                raise PolicyError('CPU policy is outside fixed cpufreq directory')
            members = cpu_set(self.backend.read(path / 'related_cpus'))
            if cpu not in members or not members.issubset(CPUS):
                raise PolicyError('policy affects CPUs outside CPU 2/3')
            policies[path.name] = {'path': path, 'cpus': sorted(members)}
        claimed = set()
        for policy in policies.values():
            if claimed.intersection(policy['cpus']):
                raise PolicyError('distinct CPU policies overlap')
            claimed.update(policy['cpus'])
        originals = {}
        for name, policy in policies.items():
            path = policy['path']
            fields = list(REQUIRED)
            if (path / EPP).exists():
                fields.append(EPP)
            values = {}
            for field in fields:
                values[field] = self.backend.read(path / field)
                if not self.backend.writable(path / field):
                    raise PolicyError(f'policy field is not writable: {name}/{field}')
            low, high = (int(values[field]) for field in REQUIRED[1:])
            if not values['scaling_governor'] or not 0 < low <= high:
                raise PolicyError(f'invalid policy values: {name}')
            originals[name] = values
        self.policies = policies
        self.original = originals

    def _write(self, name, field, value):
        path = self.policies[name]['path'] / field
        if self.backend.read(path) == value:
            return
        self._modified.add(name)
        self.backend.write(path, value)
        # cpufreq 的 QoS 写入只提交请求，policy 更新由 workqueue 异步完成。
        # 等确认后再写下一边界；不重写请求，超期或驱动钳制仍明确失败。
        deadline = time.monotonic() + READBACK_TIMEOUT
        while True:
            actual = self.backend.read(path)
            if actual == value:
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PolicyError(f'policy readback mismatch after {READBACK_TIMEOUT}s: '
                                  f'{name}/{field}: {actual} != {value}')
            time.sleep(min(READBACK_INTERVAL, remaining))

    def _range(self, name, minimum, maximum):
        path = self.policies[name]['path']
        current_min = int(self.backend.read(path / 'scaling_min_freq'))
        current_max = int(self.backend.read(path / 'scaling_max_freq'))
        # 目标高于当前上限时先扩上限，否则先降/设下限，避免瞬间 min > max。
        order = ('scaling_max_freq', 'scaling_min_freq') if int(minimum) > current_max else REQUIRED[1:]
        if not 0 < current_min <= current_max or not 0 < int(minimum) <= int(maximum):
            raise PolicyError(f'invalid frequency bounds: {name}')
        values = {'scaling_min_freq': minimum, 'scaling_max_freq': maximum}
        for field in order:
            self._write(name, field, values[field])

    def _optional(self, path):
        try:
            return {'available': True, 'value': self.backend.read(path)}
        except (OSError, ValueError) as error:
            return {'available': False, 'error': str(error)}

    def snapshot(self):
        policies = {}
        for name, policy in self.policies.items():
            path = policy['path']
            fields = {}
            for field in self.original[name]:
                try:
                    fields[field] = self.backend.read(path / field)
                except OSError as error:
                    fields[field] = {'unavailable': str(error)}
            observations = {field: self._optional(path / field)
                            for field in ('scaling_cur_freq', 'cpuinfo_cur_freq', 'scaling_driver')}
            policies[name] = {'cpus': policy['cpus'], 'fields': fields, 'observed': observations}
        activity = {}
        for cpu in CPUS:
            path = self.sysfs / f'cpu{cpu}'
            activity[str(cpu)] = {
                'siblings': self._optional(path / 'topology/thread_siblings_list'),
                'core_throttle_count': self._optional(path / 'thermal_throttle/core_throttle_count'),
                'package_throttle_count': self._optional(path / 'thermal_throttle/package_throttle_count')}
        try:
            lines = Path('/proc/stat').read_text().splitlines()
            counters = {line.split()[0]: [int(value) for value in line.split()[1:]]
                        for line in lines if line.split()[0] in ('cpu2', 'cpu3')}
            activity['proc_stat'] = {'available': True, 'counters': counters}
        except (OSError, ValueError) as error:
            activity['proc_stat'] = {'available': False, 'error': str(error)}
        affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None
        return {'policies': policies, 'activity': activity, 'affinity': affinity,
                'monotonic_ns': time.monotonic_ns()}

    def acquire(self):
        if self._active or self._modified:
            raise PolicyError('policy lease is already active or unrestored')
        try:
            self._discover()
            self.requested = {name: {'scaling_governor': 'performance',
                                    'scaling_min_freq': str(TARGET_KHZ),
                                    'scaling_max_freq': str(TARGET_KHZ)} for name in self.policies}
            for name in self.policies:
                self._write(name, 'scaling_governor', 'performance')
                self._range(name, str(TARGET_KHZ), str(TARGET_KHZ))
            self._active = True
            snapshot = self.snapshot()
            # governor 可隐式改变 EPP；锁定实际值供后续 check，归还时使用最初保存值。
            self._expected = {name: record['fields'].copy() for name, record in snapshot['policies'].items()}
            self.check()
            return {'event': 'ready', 'original': self.original,
                    'requested': self.requested, 'snapshot': snapshot}
        except Exception as error:
            restored = self.restore()
            raise PolicyError(str(error), restored) from error

    def check(self):
        if not self._active:
            raise PolicyError('policy lease is not active')
        snapshot = self.snapshot()
        for name, expected in self._expected.items():
            policy = self.policies[name]
            members = cpu_set(self.backend.read(policy['path'] / 'related_cpus'))
            if members != set(policy['cpus']) or any(
                    (self.sysfs / f'cpu{cpu}' / 'cpufreq').resolve(strict=True) != policy['path']
                    for cpu in policy['cpus']):
                raise PolicyError(f'CPU policy identity changed during measurement: {name}')
            fields = snapshot['policies'][name]['fields']
            if fields != expected or any(fields.get(field) != value for field, value in self.requested[name].items()):
                raise PolicyError(f'policy changed during measurement: {name}')
        return snapshot

    def restore(self):
        errors = []
        for name in sorted(self._modified):
            original = self.original[name]
            try:
                members = cpu_set(self.backend.read(self.policies[name]['path'] / 'related_cpus'))
                if not members.issubset(CPUS):
                    raise PolicyError(f'policy now affects outside CPU 2/3; cannot restore safely: {name}')
            except Exception as error:
                errors.append(str(error))
                continue
            try:
                self._write(name, 'scaling_governor', original['scaling_governor'])
            except Exception as error:
                errors.append(str(error))
            try:
                self._range(name, original['scaling_min_freq'], original['scaling_max_freq'])
            except Exception as error:
                errors.append(str(error))
            if EPP in original:
                try:
                    # 最后恢复 EPP，避免恢复 governor 再次覆盖原偏好。
                    self._write(name, EPP, original[EPP])
                except Exception as error:
                    errors.append(str(error))
        snapshot = self.snapshot()
        for name in self._modified:
            if snapshot['policies'][name]['fields'] != self.original[name]:
                errors.append(f'policy restoration mismatch: {name}')
        self._active = False
        if not errors:
            self._modified.clear()
        return {'event': 'restored', 'original': self.original,
                'snapshot': snapshot, 'errors': errors}


def _emit(output, record):
    try:
        output.write(json.dumps(record, sort_keys=True) + '\n')
        output.flush()
        return True
    except (OSError, ValueError):
        return False


def serve_lease(lease, *, source=None, output=None):
    """Serve the fixed check/restore protocol; never execute controller input."""
    source = source if source is not None else sys.stdin
    output = output if output is not None else sys.stdout
    result = 0
    previous = {}
    original_affinity = os.sched_getaffinity(0) if hasattr(os, 'sched_getaffinity') else None

    def interrupted(signum, frame):
        raise PolicyError(f'policy lease interrupted by signal {signum}')

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, interrupted)
    try:
        if original_affinity is not None:
            available = original_affinity - set(CPUS)
            if available:
                os.sched_setaffinity(0, available)
        if not _emit(output, lease.acquire()):
            raise PolicyError('controller output is unavailable')
        while True:
            line = source.readline(4097)
            if not line:
                break
            if len(line) > 4096 or not line.endswith('\n'):
                raise PolicyError('malformed policy command')
            try:
                record = json.loads(line)
            except (ValueError, TypeError) as error:
                raise PolicyError('malformed policy command') from error
            if not isinstance(record, dict) or set(record) != {'command'}:
                raise PolicyError('malformed policy command')
            if record['command'] == 'restore':
                break
            if record['command'] != 'check':
                raise PolicyError('unknown policy command')
            if not _emit(output, {'event': 'checked', 'snapshot': lease.check()}):
                raise PolicyError('controller output is unavailable')
    except Exception as error:
        result = 2
        _emit(output, {'event': 'blocked', 'error': str(error),
                       'restoration': getattr(error, 'restoration', None)})
    finally:
        # 恢复期间忽略第二个终止信号；输出管道失效也不能跳过归还。
        for signum in previous:
            signal.signal(signum, signal.SIG_IGN)
        try:
            restored = lease.restore()
            if original_affinity is not None:
                try:
                    os.sched_setaffinity(0, original_affinity)
                except OSError as error:
                    restored['errors'].append(f'helper affinity restoration failed: {error}')
            # 恢复失败也必须先发回原值、实际值及具体错误，不能短路丢掉记录。
            emitted = _emit(output, restored)
            if restored['errors'] or not emitted:
                result = 2
        finally:
            for signum, handler in previous.items():
                signal.signal(signum, handler)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lease', action='store_true', required=True)
    parser.parse_args()
    # 在任何写入之前拒绝无权限调用，不尝试 sudo 或另一个测试 profile。
    if os.geteuid() != 0:
        _emit(sys.stdout, {'event': 'blocked', 'error': 'CPU policy lease requires root',
                           'restoration': None})
        return 2
    return serve_lease(PolicyLease())


if __name__ == '__main__':
    sys.exit(main())
