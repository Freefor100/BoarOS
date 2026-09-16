"""Validate original suite assertions while retaining unnormalized raw output."""
import re


def validate_output(case, observation):
    contract = case.get('output_contract')
    if not contract:
        return {'status': 'pass', 'errors': []}
    stdout = bytes.fromhex(observation.get('stdout_hex', '')).decode(errors='replace')
    stderr = bytes.fromhex(observation.get('stderr_hex', '')).decode(errors='replace')
    errors, records = [], []
    if contract['kind'] == 'busybox-script':
        for marker in ('START', 'END'):
            if stdout.splitlines().count(f'#### OS COMP TEST GROUP {marker} busybox ####') != 1:
                errors.append('missing or duplicate ' + marker)
        # clear emits terminal escapes and printf need not end with a newline;
        # the unchanged shell's following result marker can share that line.
        for match in re.finditer(r'testcase busybox ([^\r\n]*?) (success|fail)(?:\r?\n|$)', stdout):
            records.append({'command': match[1], 'status': match[2]})
        if len(records) != contract['expected_records']:
            errors.append(f"observed {len(records)}/{contract['expected_records']} command results")
        if 'commands' in contract and [record['command'] for record in records] != contract['commands']:
            errors.append('missing, duplicate or reordered BusyBox command results')
        if any(record['status'] != 'success' for record in records):
            errors.append('upstream BusyBox assertions failed')
    elif contract['kind'] == 'libc-runtest':
        entry = re.escape(contract['entry'])
        marker = re.compile(rf'========== (START|END) {entry} (\S+) ==========')
        active, body = None, []
        for line in stdout.splitlines():
            match = marker.fullmatch(line)
            if match:
                if match[1] == 'START':
                    if active is not None:
                        errors.append('nested libc START marker')
                    active, body = match[2], []
                else:
                    if active != match[2]:
                        errors.append('unmatched libc END marker')
                    else:
                        records.append({'case': active, 'status': 'pass' if body.count('Pass!') == 1
                                        and not any(item.startswith('FAIL ') for item in body) else 'fail'})
                    active, body = None, []
            elif line.startswith(('========== START', '========== END')):
                errors.append('malformed libc boundary marker')
            elif active is not None:
                body.append(line)
            elif line == 'Pass!':
                errors.append('libc success marker outside a case')
        if active is not None:
            errors.append('unterminated libc case')
        if [record['case'] for record in records] != contract['cases']:
            errors.append('missing, duplicate or reordered libc wrapper results')
        if any(record['status'] != 'pass' for record in records) or re.search(r'^FAIL ', stdout + '\n' + stderr, re.M):
            errors.append('upstream libc assertions failed')
    else:
        raise ValueError('unknown upstream output contract')
    return {'status': 'upstream-failure' if errors else 'pass', 'errors': errors, 'records': records}
