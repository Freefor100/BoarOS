"""Deterministic percentile bootstrap for twelve independent boot triplets.

Each row joins before/middle/after boots from one predeclared pairing block.
Resampling keeps that complete block together; phases and individual timings
are never sampled separately. The interval is a finite-sample engineering
screen, not a theorem about equivalence or protection from systematic bias.
The caller must separately verify identities, completion, resource return,
the environment, and successful calibration before accepting a comparison.
"""

from fractions import Fraction
import math
import random


BLOCK_COUNT = 12
BOOTSTRAP_DRAWS = 10000
DEFAULT_SEED = 0xB30020261011
FIELDS = ("before_ns", "middle_ns", "after_ns")


def _median_twice(rows, field):
    values = sorted(row[field] for row in rows)
    return values[5] + values[6]


def _percent(numerator, denominator):
    # Convert after the exact ratio, so an exact +3%, ±1%, or ±2% boundary
    # cannot be rejected through cancellation in a floating-point subtraction.
    try:
        result = float(Fraction(100 * numerator, denominator))
    except OverflowError as error:
        raise ValueError("measurements cannot produce a finite percentage") from error
    if not math.isfinite(result):
        raise ValueError("measurements cannot produce a finite percentage")
    return result


def _estimate(rows):
    before = _median_twice(rows, "before_ns")
    middle = _median_twice(rows, "middle_ns")
    after = _median_twice(rows, "after_ns")
    return _percent(2 * middle - before - after, before + after)


def analyse_blocks(blocks, *, mode, seed=DEFAULT_SEED, calibration_pass=False):
    """Return all estimates and a pass/regression/unresolved classification.

    Input is exactly twelve dictionaries with the three named positive integer
    nanosecond measurements. Missing/extra fields, non-integer values and
    malformed parameters raise ValueError. A comparison without an explicit
    successful calibration remains unresolved, including an apparent gain or
    regression. The RNG seed must be a non-negative integer.
    """
    if mode not in ("calibration", "comparison"):
        raise ValueError("mode must be calibration or comparison")
    if type(seed) is not int or seed < 0:
        raise ValueError("seed must be a non-negative integer")
    if type(calibration_pass) is not bool:
        raise ValueError("calibration_pass must be a boolean")
    if not isinstance(blocks, (list, tuple)) or len(blocks) != BLOCK_COUNT:
        raise ValueError("exactly twelve complete pairing blocks are required")
    for row in blocks:
        if not isinstance(row, dict) or set(row) != set(FIELDS):
            raise ValueError("each block must have exactly before_ns/middle_ns/after_ns")
        for value in row.values():
            if type(value) is not int or value <= 0:
                raise ValueError("measurements must be positive integer nanoseconds")
            try:
                finite = math.isfinite(float(value))
            except OverflowError:
                finite = False
            if not finite:
                raise ValueError("measurements must be finite")

    rng = random.Random(seed)
    estimates = []
    for _ in range(BOOTSTRAP_DRAWS):
        sample = [blocks[rng.randrange(BLOCK_COUNT)] for _ in range(BLOCK_COUNT)]
        estimates.append(_estimate(sample))
    estimates.sort()

    # Nearest rank on 10,000 ordered draws: ranks 250/500/9500/9750.
    lower_two, lower, upper, upper_two = (
        estimates[249], estimates[499], estimates[9499], estimates[9749])
    before = _median_twice(blocks, "before_ns")
    middle = _median_twice(blocks, "middle_ns")
    after = _median_twice(blocks, "after_ns")
    drift = _percent(after - before, before)
    drift_pass = abs(drift) <= 2
    reasons = []
    if not drift_pass:
        reasons.append("drift-exceeds-limit")

    if mode == "calibration":
        interval_pass = lower_two >= -1 and upper_two <= 1
        if not interval_pass:
            reasons.append("calibration-interval-outside-tolerance")
        calibration_pass = drift_pass and interval_pass
        classification = "pass" if calibration_pass else "unresolved"
    elif not calibration_pass or not drift_pass:
        if not calibration_pass:
            reasons.append("calibration-missing")
        classification = "unresolved"
    elif upper <= 3:
        classification = "pass"
    elif lower > 3:
        classification = "regression"
        reasons.append("stable-regression")
    else:
        classification = "unresolved"
        reasons.append("interval-crosses-threshold")

    return {
        "mode": mode,
        "medians_ns": {"before": before / 2, "middle": middle / 2, "after": after / 2},
        "estimate_percent": _estimate(blocks),
        "drift_percent": drift,
        "lower95_percent": lower,
        "upper95_percent": upper,
        "lower_two95_percent": lower_two,
        "upper_two95_percent": upper_two,
        "bootstrap_draws": BOOTSTRAP_DRAWS,
        "block_count": BLOCK_COUNT,
        "bootstrap_seed": seed,
        "calibration_pass": calibration_pass,
        "drift_pass": drift_pass,
        "classification": classification,
        "reasons": reasons,
    }
