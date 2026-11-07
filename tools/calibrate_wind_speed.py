#!/usr/bin/env python3
"""Reproduce the historical candidate-model calibration without editing measurements.

This preserves the 2026-10-29 model comparison and requires its local data
archive. The current firmware uses the generated piecewise-linear profile in
weight_wind_speed_profile.h; selected_model in this historical report is not
the active firmware selection. Use calibrate_adc_run.py for new calibration.

Each entire run is held out in turn. A held-out run contributes only its initial
four-second no-wind ADC baseline, as the firmware's startup tare would do.
The three runs are also used for model comparison; these are not a new external
validation dataset. Requires numpy. Plotting is an optional separate output.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA = ROOT / 'calibration/wind_tunnel_20261029'
ZERO_SECONDS = 4.0


def load_runs(folder):
    manifest = json.loads((folder / 'sources.json').read_text())
    runs = []
    for item in manifest['runs']:
        path = folder / item['snapshot']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == item['sha256'], path
        with path.open() as handle:
            records = list(csv.DictReader(handle))
        t = np.array([float(r['timestamp_epoch']) for r in records])
        v = np.array([float(r['ground_truth_mps']) for r in records])
        raw = np.array([float(r['student_mps']) for r in records])
        assert np.isfinite(t).all() and np.isfinite(v).all() and np.isfinite(raw).all()
        assert (np.diff(t) >= 0).all() and len(t) >= 20
        zero_mask = t < t[0] + ZERO_SECONDS
        assert zero_mask.sum() >= 12, 'Need at least three seconds of baseline readings'
        zero = float(np.mean(raw[zero_mask]))
        runs.append({'name': item['run'], 't': t, 'v': v, 'raw': raw, 'delta': raw-zero,
                     'zero': zero, 'zero_count': int(zero_mask.sum()),
                     'zero_range': float(np.ptp(raw[zero_mask])),
                     'zero_reference_max_mps': float(max(v[zero_mask]))})
    return runs, manifest


def train(model, runs):
    y = np.concatenate([r['v'] for r in runs])
    d = np.concatenate([r['delta'] for r in runs])
    # Equal total weight per independent run rather than favoring the longest.
    w = np.concatenate([np.full(len(r['v']), 1/len(r['v'])) for r in runs])
    if model == 'sqrt_forward':
        gain = float(np.sum(w*y*y*d) / np.sum(w*y**4))
        return {'gain': gain}
    if model == 'sqrt_speed_loss':
        x = np.sqrt(np.maximum(d, 0))
        slope = float(np.sum(w*x*y) / np.sum(w*x*x))
        return {'gain': 1/(slope*slope)}
    if model == 'quadratic_forward':
        x = np.column_stack([y*y, y])
    elif model == 'sqrt_plus_linear':
        x = np.column_stack([np.sqrt(np.maximum(d,0)), np.maximum(d,0)])
    else:
        raise ValueError(model)
    target = d if model == 'quadratic_forward' else y
    coefficients = np.linalg.lstsq(x*np.sqrt(w[:,None]), target*np.sqrt(w), rcond=None)[0]
    return {'coefficients': coefficients.tolist()}


def predict(model, parameters, delta):
    d = np.maximum(np.asarray(delta, dtype=float), 0)
    if model.startswith('sqrt_') and model != 'sqrt_plus_linear':
        return np.sqrt(d/parameters['gain'])
    a, b = parameters['coefficients']
    if model == 'quadratic_forward':
        assert a > 0 and b >= 0, 'Quadratic must be monotonic for non-negative wind speed'
        return 2*d/(np.sqrt(b*b+4*a*d)+b)
    return np.maximum(a*np.sqrt(d)+b*d, 0)


def metrics(truth, pred):
    err = np.asarray(pred)-np.asarray(truth)
    if len(err) == 0:
        return None
    return {'n': len(err), 'mae_mps': float(np.mean(abs(err))),
            'rmse_mps': float(np.sqrt(np.mean(err*err))),
            'bias_mps': float(np.mean(err)), 'p95_abs_error_mps': float(np.quantile(abs(err),.95)),
            'max_abs_error_mps': float(max(abs(err)))}


def review(runs):
    candidates = {}
    for model in ('sqrt_forward','sqrt_speed_loss','quadratic_forward','sqrt_plus_linear'):
        folds, all_y, all_pred, post_tare_y, post_tare_pred = [], [], [], [], []
        for i, held in enumerate(runs):
            parameters = train(model, [r for j,r in enumerate(runs) if j != i])
            pred = predict(model, parameters, held['delta'])
            mask = (held['v'] >= 2) & (held['v'] <= 10)
            ready = held['t'] >= held['t'][0] + ZERO_SECONDS
            folds.append({'held_out_run': held['name'], 'training_parameters': parameters,
                          'all_speeds': metrics(held['v'],pred),
                          'after_initial_tare': metrics(held['v'][ready],pred[ready]),
                          'reference_2_to_10_mps': metrics(held['v'][mask],pred[mask])})
            all_y.extend(held['v']); all_pred.extend(pred)
            post_tare_y.extend(held['v'][ready]); post_tare_pred.extend(pred[ready])
        y,p = np.array(all_y),np.array(all_pred)
        mid = (y >= 2) & (y <= 10)
        low = y < 2
        candidates[model] = {'folds':folds, 'pooled_out_of_fold': metrics(y,p),
                             'pooled_after_initial_tare': metrics(post_tare_y,post_tare_pred),
                             'reference_2_to_10_mps':metrics(y[mid],p[mid]),
                             'reference_below_2_mps':metrics(y[low],p[low]),
                             'equal_run_mean_rmse_mps':float(np.mean([f['all_speeds']['rmse_mps'] for f in folds])),
                             'final_all_run_parameters':train(model,runs)}
    return candidates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-dir',type=Path,default=DEFAULT_DATA)
    parser.add_argument('--report',type=Path)
    args = parser.parse_args()
    runs,manifest=load_runs(args.data_dir)
    candidates=review(runs)
    result={'method':'Leave one entire run out; equal training weight per run; no random row split',
            'zero_method':'Mean raw ADC during first 4 seconds only; these initial sections must be no-wind',
            'zero_replay_limit':'Offline baseline averages ~16 aligned readings at 4 Hz; firmware averages 40 hardware polls at ~10 Hz. Same duration strategy, not an exact tare-state hardware replay.',
            'metrics_note':'pooled_out_of_fold includes 48 initial baseline samples; pooled_after_initial_tare excludes them because the firmware is silent while taring.',
            'same_plate_and_installation':True,
            'samples_are':'~4 Hz VDAS-paired readings, not 200 Hz independent sensor samples',
            'model_comparison_reuses_validation_runs':True,
            'runs':[{k:r[k] for k in ('name','zero','zero_count','zero_range','zero_reference_max_mps')} for r in runs],
            'observed_reference_range_mps':[float(min(min(r['v']) for r in runs)),float(max(max(r['v']) for r in runs))],
            'largest_observed_delta_adc':float(max(max(r['delta']) for r in runs)),
            'smallest_positive_reference_mps':float(min(min(r['v'][r['v']>0]) for r in runs)),
            'selected_model':'sqrt_forward',
            'selection_reason':'Smallest pooled leave-one-run-out RMSE among four candidates, with one gain and explicit current zero',
            'firmware_guard_delta_counts':[-5000,120000],
            'firmware_tare':{'samples':40,'poll_ms':100,'maximum_spread_counts':500,
                             'trigger':'Automatic on boot/reset with fan off; B1 held for one second restarts tare','persistent':False},
            'physical_validation':'New speed firmware not flashed or tested in the wind tunnel',
            'candidates':candidates,'sources':manifest}
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(result,indent=2)+'\n')
    for name, r in candidates.items():
        print(name, json.dumps({'pooled':r['pooled_out_of_fold'], 'after_tare':r['pooled_after_initial_tare'], 'range_2_10':r['reference_2_to_10_mps'],
                                'fold_rmse':[f['all_speeds']['rmse_mps'] for f in r['folds']],
                                'final':r['final_all_run_parameters']}))
    print('Baselines:',result['runs'])
    print('Maximum observed delta:',result['largest_observed_delta_adc'])


if __name__ == '__main__':
    main()
