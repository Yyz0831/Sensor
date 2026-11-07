#!/usr/bin/env python3
"""Fit a candidate delta-ADC LUT from one Logger run of calibrated speeds.

Requires numpy and scipy. The supplied profile must be the strictly increasing
delta-ADC LUT used to record the run. No firmware or input file is modified.
"""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import math
import re
import sys
import time


def read_csv(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))


def write_csv(path, rows):
    with path.open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args):
    started = time.perf_counter()
    # Count dependency loading in the reported total runtime.
    import numpy as np
    from scipy.optimize import nnls

    def rounded(values):
        return np.array([float(f'{v:.3f}') for v in values])

    def metric(reference, predicted):
        if not len(reference):
            return None
        error = predicted - reference
        mse = float(np.mean(error * error))
        return dict(n=len(error), mse=mse, rmse=math.sqrt(mse),
                    mae=float(np.mean(abs(error))), bias=float(np.mean(error)))

    def basis(values, knots):
        values = np.maximum(np.asarray(values, dtype=float), 0)
        seg = np.clip(np.searchsorted(knots, values, side='right') - 1, 0, len(knots) - 2)
        frac = (values - knots[seg]) / (knots[seg + 1] - knots[seg])
        matrix = np.zeros((len(values), len(knots)))
        matrix[np.arange(len(values)), seg] = 1 - frac
        matrix[np.arange(len(values)), seg + 1] = frac
        return matrix

    def predict(model, values):
        return basis(values, np.asarray(model['delta_adc'])) @ np.asarray(model['speed_mps'])

    run = args.run_dir.resolve()
    aligned_files = sorted(run.glob('*_aligned.csv'))
    if len(aligned_files) != 1:
        raise ValueError('输入文件夹必须恰好包含一组 *_aligned.csv。')
    aligned_path = aligned_files[0]
    student_path = aligned_path.with_name(aligned_path.name.replace('_aligned.csv', '_student.csv'))
    profile = args.profile.resolve()
    hashes = {str(p): digest(p) for p in (aligned_path, student_path, profile)}
    student = read_csv(student_path)
    rows = read_csv(aligned_path)
    if not student or not rows:
        raise ValueError('输入 CSV 为空。')
    t0 = float(student[0]['timestamp_epoch'])
    table = np.array([tuple(map(float, pair)) for pair in re.findall(
        r'\{([0-9.eE+-]+),\s*([0-9.eE+-]+)\}', profile.read_text())])
    if (table.ndim != 2 or table.shape[1] != 2 or len(table) < 2 or
            not np.all(np.isfinite(table)) or not np.all(table[0] == 0) or
            not np.all(np.diff(table, axis=0) > 0)):
        raise ValueError('原标定表必须从 (0,0) 开始，且 ADC 和风速节点严格递增，才能反解。')

    def inverse(values):
        return basis(values, table[:, 1]) @ table[:, 0]

    elapsed = np.array([float(row['timestamp_epoch']) - t0 for row in rows])
    reference = np.array([float(row['ground_truth_mps']) for row in rows])
    recorded = np.array([float(row['student_mps']) for row in rows])
    if not (np.all(np.isfinite(elapsed)) and np.all(np.isfinite(reference)) and
            np.all(np.isfinite(recorded)) and np.all(recorded >= 0) and
            np.all(np.diff(elapsed) >= 0)):
        raise ValueError('配对 CSV 含无效数值、负的传感器风速或倒序时间。')
    # Select only by reference wind speed. Sensor zero / >10 remains scored.
    eligible = ((reference >= 2) & (reference <= 10) &
                (elapsed >= args.exclude_initial_seconds))
    positive = recorded > 0
    fit_eligible = eligible & positive
    if sum(fit_eligible) < 24:
        raise ValueError('2–10 m/s 内可反解的正值太少，无法进行本流程的分块验证。')
    delta = np.full(len(rows), np.nan)
    delta[positive] = inverse(recorded[positive])
    low = np.full(len(rows), np.nan)
    low[positive] = inverse(np.maximum(recorded[positive] - .0005, 0))
    high = inverse(recorded + .0005)
    original = dict(delta_adc=table[:, 0], speed_mps=table[:, 1])
    if not np.array_equal(rounded(predict(original, delta[positive])), recorded[positive]):
        raise ValueError('反解校验失败：数据不符合三位小数风速格式或所提供的标定表。')

    holdout_start = args.holdout_start
    if holdout_start is None:
        holdout_start = args.exclude_initial_seconds + .7 * (elapsed[-1] - args.exclude_initial_seconds)
    dev_end = holdout_start - args.gap_seconds
    if not args.exclude_initial_seconds < dev_end < holdout_start < elapsed[-1]:
        raise ValueError('记录时长不足或时间划分无效。')
    development = fit_eligible & (elapsed < dev_end)
    holdout = eligible & (elapsed >= holdout_start)
    blocks = np.linspace(args.exclude_initial_seconds, dev_end, 4)
    zero_upper = float(inverse([.0005])[0])

    def fit(mask, count):
        x, y = delta[mask], reference[mask]
        if len(x) < count + 1 or len(np.unique(x)) < 2:
            raise ValueError(f'{count} 节点的某个训练块数据不足。')
        locations = np.linspace(math.sqrt(x.min()), math.sqrt(x.max()), count) ** 2
        locations[0], locations[-1] = x.min(), x.max()
        knots = np.r_[0., locations]
        matrix = basis(x, knots)[:, 1:] @ np.tril(np.ones((count, count)))
        increments, _ = nnls(matrix, y, maxiter=100 * count)
        values = np.r_[0., np.cumsum(increments)]
        gradient = matrix.T @ (matrix @ increments - y)
        if gradient.min() < -1e-6 or np.max(abs(gradient[increments > 1e-9]), initial=0.) > 1e-6:
            raise ValueError('最小二乘解未通过数值检查。')
        model = dict(measured_knots=count, total_knots=count + 1,
                     delta_adc=knots.tolist(), speed_mps=values.tolist(),
                     training_indices=np.flatnonzero(mask).tolist(),
                     flat_segments=int(sum(increments <= 1e-10)))
        model['old_zero_bin_new_upper_speed_mps'] = float(predict(model, [zero_upper])[0])
        return model

    def score(model, mask):
        known = mask & positive
        p = rounded(predict(model, delta[known]))
        # Explicit scenario only: unrecoverable zeros retain zero output.
        scenario = np.zeros(len(rows))
        scenario[known] = p
        return dict(positive_current=metric(reference[known], recorded[known]),
                    positive_candidate=metric(reference[known], p),
                    all_current=metric(reference[mask], recorded[mask]),
                    zero_kept_zero_scenario=metric(reference[mask], scenario[mask]),
                    censored_zeros=int(sum(mask & ~positive)))

    cv = []
    for count in args.knots:
        out_of_fold = np.full(len(rows), np.nan)
        folds = []
        try:
            for left, right in zip(blocks, blocks[1:]):
                test = development & (elapsed >= left) & (elapsed < right)
                train = development & ((elapsed < left - args.gap_seconds) |
                                       (elapsed >= right + args.gap_seconds))
                if not test.any():
                    raise ValueError('某个验证时间块没有合格点。')
                model = fit(train, count)
                out_of_fold[test] = predict(model, delta[test])
                folds.append(dict(start_s=float(left), end_s=float(right),
                                  training_indices=model['training_indices'],
                                  test_indices=np.flatnonzero(test).tolist(),
                                  mse=metric(reference[test], out_of_fold[test])['mse']))
            if not np.all(np.isfinite(out_of_fold[development])):
                raise ValueError('验证覆盖不完整。')
            cv.append(dict(knots=count, metric=metric(reference[development], out_of_fold[development]),
                           folds=folds))
        except ValueError as error:
            cv.append(dict(knots=count, unavailable=str(error)))
    available = [candidate for candidate in cv if 'metric' in candidate]
    if not available or not (holdout & positive).any():
        raise ValueError('没有足够数据完成分块选择和后段检验；请采集更完整的一组。')
    selected = min(available, key=lambda c: (c['metric']['mse'], c['knots']))['knots']
    # Freeze selection before fitting/scoring the forward holdout.
    frozen = fit(development, selected)
    validation = score(frozen, holdout)
    full_model = fit(fit_eligible, selected)
    full_score = score(full_model, eligible)

    output = args.output_dir.resolve()
    if output.exists():
        raise ValueError('输出目录已存在，请指定新目录以保留已有结果。')
    if any(digest(Path(p)) != h for p, h in hashes.items()):
        raise ValueError('运行过程中输入文件发生变化，请在保存完成后重试。')
    output.mkdir(parents=True)
    predictions = []
    for i in np.flatnonzero(eligible):
        candidate = float(rounded(predict(full_model, [delta[i]]))[0]) if positive[i] else ''
        predictions.append(dict(timestamp_iso=rows[i]['timestamp_iso'], elapsed_s=float(elapsed[i]),
                                reference_mps=float(reference[i]), recorded_speed_mps=float(recorded[i]),
                                reconstructed_filtered_delta_adc=float(delta[i]) if positive[i] else '',
                                delta_lower=float(low[i]) if positive[i] else '', delta_upper=float(high[i]),
                                invertible=int(positive[i]), full_fit_speed_mps=candidate,
                                zero_kept_zero_scenario_mps=candidate if positive[i] else 0))
    write_csv(output / 'predictions.csv', predictions)
    write_csv(output / 'calibration_knots.csv', [
        dict(knot_index=i, delta_adc=x, speed_mps=y)
        for i, (x, y) in enumerate(zip(full_model['delta_adc'], full_model['speed_mps']))])
    models = dict(frozen_for_validation=frozen, full_fit=full_model)
    (output / 'models.json').write_text(json.dumps(models, indent=2, allow_nan=False) + '\n')
    summary = dict(
        source_sha256=hashes, reference_band_mps=[2, 10],
        excluded_initial_seconds=args.exclude_initial_seconds,
        holdout_start_seconds=holdout_start, development_end_seconds=dev_end,
        gap_seconds=args.gap_seconds, additional_filtering=False,
        counts=dict(student_lines=len(student), aligned=len(rows), score=int(sum(eligible)),
                    invertible_fit=int(sum(fit_eligible)), zero=int(sum(eligible & ~positive))),
        selected_knots=selected, inner_cv=cv, forward_holdout=validation,
        full_fit_residual=full_score, input_kind='recorded calibrated speed, three decimal places',
        reconstructed_kind='filtered delta ADC; absolute zero and raw ADC are not recovered',
        zero_policy='Not invertible; blank actual predictions. Zero-kept-zero results are a scenario only.',
        original_zero_bin_candidate_max_mps=full_model['old_zero_bin_new_upper_speed_mps'],
        limitations=['Full fit is training residual, not independent validation.',
                     'Single-run time holdout does not establish performance after reinstalling the sensor.',
                     'A biased or noisy wind-tunnel reference can be learned by the calibration.'],
        source_unchanged=True, firmware_modified=False,
        elapsed_seconds_including_imports=time.perf_counter() - started)
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    print(json.dumps(dict(output=str(output), seconds=summary['elapsed_seconds_including_imports'],
                          counts=summary['counts'], selected_knots=selected,
                          forward_holdout=validation, full_fit_residual=full_score),
                     ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description='单组风速 CSV 反解及候选标定，仅按参考风速 2–10 m/s 评分。')
    parser.add_argument('--run-dir', type=Path, required=True, help='包含同一轮 student/aligned CSV 的文件夹')
    parser.add_argument('--profile', type=Path, required=True, help='采集时使用的 delta-ADC LUT 头文件')
    parser.add_argument('--output-dir', type=Path, required=True, help='新的输出文件夹；已有目录不会被覆盖')
    parser.add_argument('--exclude-initial-seconds', type=float, default=0,
                        help='仅在已知开头无风等情况下指定；默认不额外排除时间')
    parser.add_argument('--holdout-start', type=float, help='后段验证开始秒数；默认最后约30%%时间')
    parser.add_argument('--gap-seconds', type=float, default=1, help='训练与验证的时间隔离，默认1秒')
    parser.add_argument('--knots', type=int, nargs='+', default=[4, 6, 8, 12], help='拟合节点候选数，另加原点')
    args = parser.parse_args()
    if (not math.isfinite(args.exclude_initial_seconds) or args.exclude_initial_seconds < 0 or
            not math.isfinite(args.gap_seconds) or args.gap_seconds <= 0 or
            (args.holdout_start is not None and not math.isfinite(args.holdout_start)) or
            any(k < 2 or k > 32 for k in args.knots)):
        parser.error('时间参数必须有效，节点数须在 2–32 之间。')
    args.knots = sorted(set(args.knots))
    try:
        execute(args)
    except (OSError, ValueError, KeyError, ImportError, RuntimeError) as error:
        parser.exit(1, f'无法完成标定：{error}\n')


if __name__ == '__main__':
    main()
