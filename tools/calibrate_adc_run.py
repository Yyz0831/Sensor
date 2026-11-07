#!/usr/bin/env python3
"""One raw-ADC Logger run -> online EMA -> 12 fitted knots plus origin.

Requires NumPy and SciPy. Only --apply writes the firmware profile, after saving
its previous contents. This program never builds or uploads firmware.
"""
import argparse
import bisect
import csv
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import time

import numpy as np
from scipy.optimize import nnls

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'Core/Inc/weight_wind_speed_profile.h'
KNOTS = 12  # Measured-range knots, with a separate fixed (0, 0) origin.
TAU = 0.2
STALE = 0.25


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_csv(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        return list(csv.DictReader(stream))


def write_csv(path, rows):
    with path.open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def resolve_aligned(path, run_name=None):
    path = path.expanduser().resolve()
    if path.is_dir():
        candidates = sorted(p for p in path.glob('*_aligned.csv') if p.is_file())
        if run_name:
            if Path(run_name).name != run_name:
                raise ValueError('--run-name 只填写组名，不包含文件夹路径。')
            filename = run_name if run_name.endswith('_aligned.csv') else f'{run_name}_aligned.csv'
            candidates = [p for p in candidates if p.name == filename]
        if not candidates:
            raise ValueError(f'文件夹中没有找到匹配的 *_aligned.csv：{path}')
        if len(candidates) > 1:
            names = '\n'.join(f'  {p.name.removesuffix("_aligned.csv")}' for p in candidates)
            raise ValueError(f'文件夹中有多组数据，请加 --run-name 指定其中一组：\n{names}')
        path = candidates[0]
    elif run_name:
        raise ValueError('--run-name 只在输入文件夹时使用。')
    if not path.is_file() or not path.name.endswith('_aligned.csv'):
        raise ValueError(f'请提供包含 *_aligned.csv 的文件夹，或一个存在的 *_aligned.csv：{path}')
    student = path.with_name(path.name.removesuffix('_aligned.csv') + '_student.csv')
    if not student.is_file():
        raise ValueError(f'缺少同组原始 ADC 文件：{student}')
    return path


def online_ema(t, raw, start):
    """Process every serial row in arrival order, including held ADC values.

    Host receive timestamps approximate the board's 5 ms clock. Equal timestamps
    have dt=0; a >=250 ms receive gap discards history. No future samples enter
    an estimate. This is not a reconstruction of unavailable MCU timestamps.
    """
    out = np.full(len(raw), np.nan)
    previous = None
    previous_t = None
    for i in range(start, len(raw)):
        dt = 0.0 if previous_t is None else t[i] - previous_t
        if previous is None or dt >= STALE:
            previous = float(raw[i])
        else:
            previous += (1.0 - math.exp(-dt / TAU)) * (raw[i] - previous)
        out[i] = previous
        previous_t = t[i]
    return out


def basis(delta, knots):
    x = np.maximum(np.asarray(delta, dtype=float), 0.0)
    segment = np.clip(np.searchsorted(knots, x, side='right') - 1, 0, len(knots) - 2)
    fraction = (x - knots[segment]) / (knots[segment + 1] - knots[segment])
    matrix = np.zeros((len(x), len(knots)))
    matrix[np.arange(len(x)), segment] = 1.0 - fraction
    matrix[np.arange(len(x)), segment + 1] = fraction
    return matrix


def fit(delta, reference):
    positive = delta[delta > 0]
    if len(delta) < 24 or len(np.unique(positive)) < KNOTS:
        raise ValueError('2–10 m/s 内的数据不足以拟合 12 个节点。')
    measured = np.linspace(np.sqrt(positive.min()), np.sqrt(positive.max()), KNOTS) ** 2
    measured[0], measured[-1] = positive.min(), positive.max()
    knots = np.r_[0.0, measured]
    h = basis(delta, knots)
    # Nonnegative increments enforce a nondecreasing calibration curve.
    design = h[:, 1:] @ np.tril(np.ones((KNOTS, KNOTS)))
    if np.linalg.matrix_rank(design) < KNOTS:
        raise ValueError('这一组没有覆盖足够的 ADC 区间，12 节点拟合不能唯一确定。')
    increments, _ = nnls(design, reference, maxiter=100 * KNOTS)
    gradient = design.T @ (design @ increments - reference)
    if (gradient.min() < -1e-6 or
            np.max(abs(gradient[increments > 1e-9]), initial=0.0) > 1e-6):
        raise ValueError('拟合未通过数值收敛检查。')
    values = np.r_[0.0, np.cumsum(increments)]
    predictions = np.array([float(f'{v:.3f}') for v in h @ values])
    return knots, values, predictions


def make_header(knots, values, run, n, mse):
    pairs = '\n'.join(f'  {{{x:.17g}, {y:.17g}}},' for x, y in zip(knots, values))
    return f'''#ifndef WEIGHT_WIND_SPEED_PROFILE_H
#define WEIGHT_WIND_SPEED_PROFILE_H

/* Generated from raw ADC: {run}
 * Input: online EMA(raw ADC, tau=200 ms) minus this reset's no-wind zero.
 * Twelve fitted measured-range knots plus fixed origin; 13 points total.
 * Reference 2-10 m/s: {n} pairs; full-fit MSE={mse:.12g} (m/s)^2.
 * This is training residual, not held-out accuracy. See calibration report.
 * Firmware updates EMA at each 5 ms output slot; hardware tare remains 4 s.
 * Nonpositive delta -> zero; above the last knot -> final-segment extrapolation.
 * Raw ADC acquisition and the sensor module configuration are unchanged.
 */
#define WEIGHT_WIND_SPEED_EMA_TAU_MS 200.0
#define WEIGHT_WIND_SPEED_KNOT_COUNT 13U

typedef struct
{{
  double delta_adc;
  double speed_mps;
}} WeightWindSpeedKnot;

static const WeightWindSpeedKnot WEIGHT_WIND_SPEED_KNOTS[WEIGHT_WIND_SPEED_KNOT_COUNT] = {{
{pairs}
}};

#endif
'''


def install_profile(target, content, previous, output):
    """Backup and replace only the selected header, never build or upload."""
    if target.read_bytes() != previous:
        raise ValueError('标定期间参数文件已被其他操作修改，结果已保存，但未覆盖参数。')
    backup = output / 'previous_profile.h'
    with backup.open('xb') as stream:
        stream.write(previous)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content.encode('utf-8'))
        temporary.chmod(target.stat().st_mode & 0o777)
        os.replace(temporary, target)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def execute(args):
    started = time.perf_counter()
    aligned_path = resolve_aligned(args.run_dir or args.aligned_csv, args.run_name)
    print(f'已选择：{aligned_path}')
    student_path = aligned_path.with_name(aligned_path.name.replace('_aligned.csv', '_student.csv'))
    source_hashes = {str(p): digest(p) for p in (aligned_path, student_path)}
    previous = PROFILE.read_bytes() if args.apply else None
    run = aligned_path.name.removesuffix('_aligned.csv')
    output = (args.output_dir or ROOT / 'calibration' /
              f'adc_{run}_{datetime.now():%Y%m%d_%H%M%S_%f}').resolve()
    if output.exists():
        raise ValueError('输出文件夹已存在，请指定新文件夹，以免覆盖历史结果。')
    student, aligned = read_csv(student_path), read_csv(aligned_path)
    if not student or not aligned:
        raise ValueError('CSV 为空。')
    t = np.array([float(row['timestamp_epoch']) for row in student])
    raw = np.array([float(row['value_raw_mps']) for row in student])
    if not (np.isfinite(t).all() and np.isfinite(raw).all() and
            np.all(np.diff(t) >= 0) and np.all(raw == np.rint(raw)) and
            np.all((raw >= -2147483648) & (raw <= 2147483647))):
        raise ValueError('需要按时间顺序保存的原始整数 ADC；不要输入风速或裁剪后的数值列。')
    elapsed = t - t[0]
    zero_end = args.zero_start + args.zero_seconds
    zero_mask = (elapsed >= args.zero_start) & (elapsed < zero_end)
    if elapsed[-1] < zero_end or zero_mask.sum() < 20:
        raise ValueError('指定无风区间内没有足够数据。')
    zero_raw = raw[zero_mask]
    zero = float(zero_raw.mean())
    spread = float(np.ptp(zero_raw))
    if spread > 500:
        raise ValueError(f'指定零点区间波动 {spread:g} counts，超过板子校零阈值 500；'
                         '请选择确实无风且稳定的 --zero-start / --zero-seconds 区间。')
    seed = int(np.flatnonzero(zero_mask)[-1])
    filtered = online_ema(t, raw, seed)

    # Match the official Logger's nearest-row selection, including timestamp
    # ties sorted by raw ADC. Filtering itself stays in original serial order.
    order = sorted(range(len(t)), key=lambda i: (t[i], raw[i]))
    ordered_t = [t[i] for i in order]
    pairs = []
    for row_number, row in enumerate(aligned, start=2):
        reference_t = float(row['timestamp_epoch'])
        reference = float(row['ground_truth_mps'])
        if not math.isfinite(reference_t) or not math.isfinite(reference):
            raise ValueError(f'aligned CSV 第 {row_number} 行有无效参考值。')
        if not row['student_mps'].strip():
            continue  # Logger explicitly marked this reference as unpaired.
        recorded_raw = float(row['student_mps'])
        position = bisect.bisect_left(ordered_t, reference_t)
        choices = [j for j in (position - 1, position) if 0 <= j < len(order)]
        best = min(choices, key=lambda j: abs(ordered_t[j] - reference_t))
        i = order[best]
        if abs(t[i] - reference_t) > args.align_tolerance_ms / 1000 or raw[i] != recorded_raw:
            raise ValueError(f'aligned CSV 第 {row_number} 行无法与同组原始 ADC 对应。')
        pairs.append(dict(aligned_csv_row=row_number, student_csv_row=i + 2,
                          timestamp_epoch=reference_t, elapsed_s=reference_t - t[0],
                          reference_mps=reference, raw_adc=float(raw[i]),
                          filtered_adc=float(filtered[i]), delta_adc=float(filtered[i] - zero),
                          pairing_offset_ms=float((t[i] - reference_t) * 1000),
                          after_zero=bool(reference_t - t[0] >= zero_end and i >= seed)))
    # Selection depends on reference and zero acquisition, never on predictions.
    selected = [p for p in pairs if 2 <= p['reference_mps'] <= 10 and p['after_zero']]
    delta = np.array([p['delta_adc'] for p in selected])
    reference = np.array([p['reference_mps'] for p in selected])
    if not np.isfinite(delta).all():
        raise ValueError('存在没有有效 EMA 的评分点。')
    knots, values, predicted = fit(delta, reference)
    errors = predicted - reference
    mse = float(np.mean(errors ** 2))
    header = make_header(knots, values, run, len(selected), mse)
    for pair, speed, error in zip(selected, predicted, errors):
        pair.update(predicted_mps=float(speed), error_mps=float(error), squared_error=float(error ** 2))
    if any(digest(Path(p)) != h for p, h in source_hashes.items()):
        raise ValueError('运行期间源 CSV 发生变化，请在保存完成后重试。')
    report = dict(
        run=run, source_sha256=source_hashes, input_kind='raw integer ADC counts',
        measured_knots=KNOTS, total_knots=KNOTS + 1, ema_tau_seconds=TAU,
        ema_method='Every serial row, including held values; causal actual host dt; no second EMA.',
        zero=dict(start_seconds=args.zero_start, end_seconds=zero_end, adc=zero,
                  serial_rows=int(zero_mask.sum()), raw_spread_counts=spread),
        reference_band_mps=[2, 10], score_points=len(selected), mse=mse, rmse=math.sqrt(mse),
        metric_kind='full-run training residual at 3 decimal places, not held-out validation',
        input_student_rows=len(student), input_aligned_rows=len(aligned),
        reference_band_points_before_zero_exclusion=sum(2 <= p['reference_mps'] <= 10 for p in pairs),
        nonpositive_delta_scored=int(sum(delta <= 0)), flat_segments=int(sum(np.diff(values) <= 1e-10)),
        prediction_clipped_to_evaluation_band=False, residual_outliers_removed=False,
        max_pairing_offset_ms=max(abs(p['pairing_offset_ms']) for p in selected),
        host_timing=dict(duplicate_timestamps=int(sum(np.diff(t) == 0)),
                         largest_gap_seconds=float(max(np.diff(t))),
                         row_rate_hz=float((len(t) - 1) / (t[-1] - t[0]))),
        parameters=[dict(delta_adc=float(x), speed_mps=float(y)) for x, y in zip(knots, values)],
        limitations=['Host receive timestamps approximate the MCU 5 ms output clock.',
                     'Held serial rows are not independent hardware ADC conversions.',
                     'Offline zero is a raw-serial-window mean; hardware still takes 40 raw reads over 4 s.',
                     'One-run fit error does not establish accuracy on a new run or installation.'],
        firmware_profile=str(PROFILE), applied=False, compiled=False, uploaded=False)
    output.mkdir(parents=True)
    write_csv(output / 'predictions.csv', selected)
    write_csv(output / 'calibration_knots.csv', report['parameters'])
    (output / 'weight_wind_speed_profile.h').write_text(header)
    if args.apply:
        install_profile(PROFILE, header, previous, output)
        report.update(applied=True, previous_profile_sha256=hashlib.sha256(previous).hexdigest(),
                      installed_profile_sha256=digest(PROFILE))
    report['elapsed_seconds'] = time.perf_counter() - started
    (output / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    print(f'2–10 m/s 全量拟合：N={len(selected)}, MSE={mse:.9f} (m/s)^2, RMSE={math.sqrt(mse):.9f} m/s')
    print(f'零点={zero:.6f} counts；EMA=0.2 s；12 个拟合节点 + 固定原点。')
    print('delta_adc, speed_mps')
    for x, y in zip(knots, values):
        print(f'{x:.17g}, {y:.17g}')
    print(f'结果：{output}')
    print(f'已写入：{PROFILE}' if args.apply else '未改板子参数；添加 --apply 可备份后写入。')
    print('未编译、未上传。MSE 是本组拟合误差，不是独立验证成绩。')
    return report


def main():
    parser = argparse.ArgumentParser(description='原始 ADC 单组标定：0.2 秒在线 EMA，固定 12 节点，参考 2–10 m/s。')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--run-dir', type=Path, help='数据文件夹；自动寻找其中的 *_aligned.csv')
    source.add_argument('--aligned-csv', type=Path, help='兼容原来的文件路径，也接受数据文件夹')
    parser.add_argument('--run-name', help='文件夹中有多组时指定组名，例如 run_20261104_173428')
    parser.add_argument('--output-dir', type=Path, help='新输出目录；省略时在 calibration 中按时间创建')
    parser.add_argument('--zero-start', type=float, default=0, help='无风区间开始秒数，相对 student 文件第一行')
    parser.add_argument('--zero-seconds', type=float, default=4, help='无风零点窗口，默认 4 秒；不改变固件校零时长')
    parser.add_argument('--align-tolerance-ms', type=float, default=150)
    parser.add_argument('--apply', action='store_true', help='备份并写入当前工程的风速标定参数，不编译或上传')
    args = parser.parse_args()
    if (not all(math.isfinite(x) for x in (args.zero_start, args.zero_seconds, args.align_tolerance_ms)) or
            args.zero_start < 0 or args.zero_seconds <= 0 or args.align_tolerance_ms <= 0):
        parser.error('时间参数必须是有效正数（zero-start 可为零）。')
    try:
        execute(args)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        parser.exit(1, f'无法完成标定：{error}\n')


if __name__ == '__main__':
    main()
