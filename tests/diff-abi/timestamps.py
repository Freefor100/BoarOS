"""Normalize timestamp observations against each guest's measured clock interval.

Only absolute wall-clock identity is normalized. Raw 224-byte records remain in
serial logs; changes, clock membership, relatime inputs and timestamp coupling
are checked. The schema is 28 signed little-endian RV64 words:
lo, hi, before, after, parent-before, parent-after (each timestamp is sec,nsec).
"""
import json
import struct


def normalize_timestamps(data):
    try:
        raw = bytes.fromhex(data)
    except ValueError as error:
        raise ValueError('invalid timestamp payload encoding') from error
    if len(raw) != 224:
        raise ValueError('timestamp payload must contain 28 RV64 words')
    values = struct.unpack('<28q', raw)
    pairs = list(zip(values[::2], values[1::2]))
    if any(not 0 <= nsec < 1_000_000_000 for _, nsec in pairs):
        raise ValueError('invalid timestamp nanoseconds')
    low, high = pairs[:2]
    if low > high:
        raise ValueError('reversed operation clock interval')
    before, after, parent_before, parent_after = (pairs[index:index + 3] for index in (2, 5, 8, 11))

    def observation(previous, current, optional=False):
        existed = any(value != (0, 0) for value in previous)
        exists = any(value != (0, 0) for value in current)
        if not exists:
            if not optional or existed:
                raise ValueError('missing timestamp observation')
            return None
        changed = [old != new for old, new in zip(previous, current)]
        for new, did_change in zip(current, changed):
            if did_change and not low <= new <= high:
                raise ValueError('changed timestamp outside operation clock interval')
        result = {'existed': existed, 'changed': changed,
                  'mtime_eq_ctime': current[1] == current[2] if changed[1] else None}
        if not existed:
            result['creation_equal'] = current[0] == current[1] == current[2]
        return result

    result = {'file': observation(before, after),
              'parent': observation(parent_before, parent_after, optional=True)}
    # The pinned Linux relatime decision compares a/m/c and whole-second age.
    # Test fixtures wait before operations, avoiding a boundary racing 24h.
    if result['file']['existed']:
        result['relatime_before'] = [before[0] <= before[1], before[0] <= before[2],
                                     low[0] - before[0][0] >= 86400]
    return 'timestamps:' + json.dumps(result, sort_keys=True, separators=(',', ':'))
