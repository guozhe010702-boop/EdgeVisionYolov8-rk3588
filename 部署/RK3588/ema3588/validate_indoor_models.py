"""Board-local held-out image checks for RKNN, zero-copy and label contracts."""
import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import subprocess
import time


def iou(left, right):
    overlap = max(0, min(left[2], right[2]) - max(left[0], right[0])) * max(0, min(left[3], right[3]) - max(left[1], right[1]))
    area_left = max(0, left[2] - left[0]) * max(0, left[3] - left[1])
    area_right = max(0, right[2] - right[0]) * max(0, right[3] - right[1])
    return overlap / max(1e-12, area_left + area_right - overlap)


def compare(predicted, reference):
    counts = defaultdict(lambda: {'tp': 0, 'fp': 0, 'fn': 0})
    used = set()
    for prediction in sorted(predicted, key=lambda p: -p.get('score', 1)):
        choices = [(iou(prediction['box'], target['box']), index) for index, target in enumerate(reference)
                   if index not in used and target['class_id'] == prediction['class_id']]
        best = max(choices, default=(0, -1))
        if best[0] >= .5:
            used.add(best[1]); counts[prediction['class_id']]['tp'] += 1
        else:
            counts[prediction['class_id']]['fp'] += 1
    for index, target in enumerate(reference):
        if index not in used:
            counts[target['class_id']]['fn'] += 1
    return counts


def totals(rows, names):
    merged = defaultdict(lambda: {'tp': 0, 'fp': 0, 'fn': 0})
    for row in rows:
        for category, stats in row.items():
            for key, value in stats.items():
                merged[category][key] += value
    def rates(stats):
        tp, fp, fn = stats['tp'], stats['fp'], stats['fn']
        return dict(stats, precision=tp / (tp + fp) if tp + fp else None,
                    recall=tp / (tp + fn) if tp + fn else None)
    combined = {key: sum(item[key] for item in merged.values()) for key in ('tp', 'fp', 'fn')}
    return {'overall': rates(combined), 'classes': {names[int(category)]: rates(value) for category, value in merged.items()}}


def ground_truth(root, record, kind, helmet):
    if kind == 'helmet':
        item = helmet[int(Path(record['file']).stem)]
        result = []
        for category, box in zip(item['objects']['category'], item['objects']['bbox']):
            result.append({'class_id': category, 'box': [box[0], box[1], box[0] + box[2], box[1] + box[3]]})
        return result
    label_path = root / record['file'].replace('/images/', '/labels/')
    label_path = label_path.with_suffix('.txt')
    width, height = record['width'], record['height']
    result = []
    if label_path.exists():
        for line in label_path.read_text().splitlines():
            category, cx, cy, w, h = map(float, line.split())
            result.append({'class_id': int(category), 'box': [(cx - w/2)*width, (cy - h/2)*height,
                          (cx + w/2)*width, (cy + h/2)*height]})
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--limit', type=int, default=0)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    assets = json.loads((args.data / 'assets.json').read_text())
    helmet = {item['row_index']: item for item in assets['helmet_validation']}
    summary = {'iou_threshold': .5, 'scope': 'Small public sanity subsets, not a complete unseen mAP benchmark', 'models': {}}
    all_records = []
    for kind in ('coco80', 'helmet'):
        folder = args.runtime / 'models' / kind
        manifest = json.loads((folder / 'manifest.json').read_text())
        records = json.loads((args.data / ('reference-' + kind + '.json')).read_text())
        if args.limit:
            records = records[:args.limit]
        names = manifest['classes']
        reference_stats, zero_copy_stats, accuracy_stats = [], [], []
        environment = dict(os.environ, LD_LIBRARY_PATH=str(args.runtime / 'lib'),
            EDGEVISION_LABELS=str(folder / 'classes.txt'), EDGEVISION_CONFIDENCE=str(manifest['confidence']),
            EDGEVISION_NPU_CORES='7', EDGEVISION_NO_IMAGE='1')
        started = time.monotonic()
        for index, record in enumerate(records):
            predictions = {}
            for executable in ('rknn_yolov8_demo', 'rknn_yolov8_demo_zero_copy'):
                result = subprocess.run([str(args.runtime / executable), str(folder / 'model.rknn'), str(args.data / record['file'])],
                    cwd=str(args.output), env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=30)
                if result.returncode or 'DETECTION_DONE ' not in result.stdout:
                    (args.output / 'failure.log').write_text(result.stdout)
                    raise RuntimeError('Native inference failed; inspect failure.log: ' + executable)
                predictions[executable] = [json.loads(line[10:]) for line in result.stdout.splitlines() if line.startswith('DETECTION ')]
            standard = predictions['rknn_yolov8_demo']
            direct = predictions['rknn_yolov8_demo_zero_copy']
            reference_stats.append(compare(direct, record['detections']))
            zero_copy_stats.append(compare(direct, standard))
            accuracy_stats.append(compare(direct, ground_truth(args.data, record, kind, helmet)))
            all_records.append(dict(model=kind, file=record['file'], reference=record['detections'], native=direct, standard=standard))
            if (index + 1) % 8 == 0:
                print(json.dumps({'model': kind, 'images_done': index + 1, 'seconds': time.monotonic() - started}), flush=True)
        summary['models'][kind] = {'images': len(records), 'confidence': manifest['confidence'],
            'onnx_agreement': totals(reference_stats, names), 'zero_copy_agreement': totals(zero_copy_stats, names),
            'ground_truth_sanity': totals(accuracy_stats, names), 'seconds': time.monotonic() - started}
        (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
        print(json.dumps({kind: summary['models'][kind]}), flush=True)
    (args.output / 'predictions.json').write_text(json.dumps(all_records))
    for info in summary['models'].values():
        for metric in ('precision', 'recall'):
            agreement = info['onnx_agreement']['overall'][metric]
            if agreement is None or agreement < .85:
                raise RuntimeError('RKNN / ONNX agreement is below the acceptance threshold')
            direct = info['zero_copy_agreement']['overall'][metric]
            if direct is None or direct < .98:
                raise RuntimeError('Zero-copy output disagrees with the standard API')
    print('NATIVE_VALIDATION_PASSED', flush=True)


if __name__ == '__main__':
    main()
