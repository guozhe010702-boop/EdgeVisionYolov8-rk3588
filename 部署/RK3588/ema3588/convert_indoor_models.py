"""CPU-only PT -> verified nine-output ONNX -> INT8 RKNN model bundles."""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import random
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def preprocess(path):
    import cv2
    import numpy as np
    image = cv2.imread(str(path))
    if image is None:
        raise ValueError('Invalid image: ' + str(path))
    height, width = image.shape[:2]
    scale = min(640 / width, 640 / height)
    resized_w, resized_h = int(width * scale), int(height * scale)
    left, top = (640 - resized_w) // 2, (640 - resized_h) // 2
    canvas = np.full((640, 640, 3), 114, dtype=np.uint8)
    canvas[top:top + resized_h, left:left + resized_w] = cv2.resize(image, (resized_w, resized_h))[:, :, ::-1]
    return canvas.transpose(2, 0, 1)[None].astype(np.float32) / 255, (width, height, scale, left, top)


def decode(outputs, geometry, threshold):
    import numpy as np
    all_boxes, all_scores, all_classes = [], [], []
    for i in (0, 3, 6):
        boxes, scores = outputs[i][0], outputs[i + 1][0]
        height, width = scores.shape[1:]
        classes = scores.argmax(axis=0).reshape(-1)
        confidence = scores.max(axis=0).reshape(-1)
        keep = confidence >= threshold
        values = boxes.reshape(4, 16, -1)
        values = np.exp(values - values.max(axis=1, keepdims=True))
        distance = (values / values.sum(axis=1, keepdims=True) * np.arange(16)[None, :, None]).sum(axis=1)
        yy, xx = np.meshgrid(np.arange(height), np.arange(width), indexing='ij')
        x, y = xx.reshape(-1) + .5, yy.reshape(-1) + .5
        stride = 640 / height
        xyxy = np.stack((x - distance[0], y - distance[1], x + distance[2], y + distance[3]), axis=1) * stride
        all_boxes.extend(xyxy[keep]); all_scores.extend(confidence[keep]); all_classes.extend(classes[keep])
    if not all_boxes:
        return []
    boxes, scores, classes = np.asarray(all_boxes), np.asarray(all_scores), np.asarray(all_classes)
    keep = []
    for cls in set(classes.tolist()):
        order = np.where(classes == cls)[0]
        order = order[np.argsort(-scores[order])]
        while len(order):
            index = order[0]
            keep.append(index)
            rest = order[1:]
            if not len(rest):
                break
            wh = np.maximum(0, np.minimum(boxes[index, 2:], boxes[rest, 2:]) - np.maximum(boxes[index, :2], boxes[rest, :2]))
            intersection = wh[:, 0] * wh[:, 1]
            areas = np.maximum(0, boxes[rest, 2] - boxes[rest, 0]) * np.maximum(0, boxes[rest, 3] - boxes[rest, 1])
            own_area = np.prod(np.maximum(0, boxes[index, 2:] - boxes[index, :2]))
            order = rest[intersection / np.maximum(own_area + areas - intersection, 1e-9) <= .45]
    original_w, original_h, scale, left, top = geometry
    results = []
    for index in sorted(keep, key=lambda j: -scores[j])[:128]:
        box = (boxes[index] - [left, top, left, top]) / scale
        box[[0, 2]] = np.clip(box[[0, 2]], 0, original_w)
        box[[1, 3]] = np.clip(box[[1, 3]], 0, original_h)
        results.append(dict(class_id=int(classes[index]), score=float(scores[index]), box=box.tolist()))
    return results


def convert(work, kind):
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from ultralytics import YOLO
    from ultralytics.nn.modules.head import Detect
    from rknn.api import RKNN
    started = time.monotonic()
    source = work / 'source' / ('yolov8s.pt' if kind == 'coco80' else 'helmet-yolov8n.pt')
    destination = work / 'bundle' / kind
    destination.mkdir(parents=True, exist_ok=True)
    wrapped = YOLO(str(source), task='detect')
    classes = [wrapped.names[i] for i in range(len(wrapped.names))]
    if len(classes) != (80 if kind == 'coco80' else 2):
        raise ValueError('Unexpected checkpoint class list: ' + str(classes))
    if kind == 'coco80' and classes != (work / 'source/coco80.txt').read_text().splitlines():
        raise ValueError('COCO label order differs from the checkpoint')
    model = wrapped.model.cpu().float().eval()
    model.fuse()
    for module in model.modules():
        if isinstance(module, Detect):
            module.export, module.format, module.dynamic, module.end2end = True, 'rknn', False, False
    tensor = torch.zeros(1, 3, 640, 640)
    with torch.inference_mode():
        outputs = model(tensor)
    expected = []
    for grid in (80, 40, 20):
        expected += [[1, 64, grid, grid], [1, len(classes), grid, grid], [1, 1, grid, grid]]
    if not isinstance(outputs, (list, tuple)) or [list(x.shape) for x in outputs] != expected:
        raise ValueError('The exporter did not produce the required nine DFL outputs')
    onnx_file = destination / 'model.onnx'
    torch.onnx.export(model, tensor, str(onnx_file), opset_version=12, input_names=['images'],
                      output_names=['branch_' + str(i) for i in range(9)], do_constant_folding=True)
    onnx.checker.check_model(onnx.load(str(onnx_file)))
    if kind == 'coco80':
        images = sorted((work / 'datasets/coco128/images/train2017').glob('*.jpg'))
        random.Random(3588).shuffle(images)
        calibration, validation = images[:64], images[64:]
        threshold = .35
    else:
        calibration = sorted((work / 'helmet/train').glob('*.jpg'))
        validation = sorted((work / 'helmet/validation').glob('*.jpg'))
        threshold = .5
    if len(calibration) < 20 or not validation:
        raise ValueError('Missing real calibration or validation images')
    session = ort.InferenceSession(str(onnx_file), providers=['CPUExecutionProvider'])
    sample, _ = preprocess(validation[0])
    with torch.inference_mode():
        reference = [x.numpy() for x in model(torch.from_numpy(sample))]
    exported = session.run(None, {'images': sample})
    for left, right in zip(reference, exported):
        np.testing.assert_allclose(left, right, atol=.003, rtol=.003)
    maximum_error = max(float(np.abs(left - right).max()) for left, right in zip(reference, exported))
    text = destination / 'calibration.txt'
    text.write_text('\n'.join(str(p) for p in calibration) + '\n')
    rknn = RKNN(verbose=False)
    try:
        for result in [rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
            target_platform='rk3588', optimization_level=3), rknn.load_onnx(model=str(onnx_file))]:
            if result not in (None, 0):
                raise RuntimeError('RKNN configuration/import failed: ' + str(result))
        result = rknn.build(do_quantization=True, dataset=str(text))
        if result != 0:
            raise RuntimeError('RKNN build failed: ' + str(result))
        if rknn.export_rknn(str(destination / 'model.rknn')) != 0:
            raise RuntimeError('RKNN export failed')
    finally:
        rknn.release()
    labels = destination / 'classes.txt'
    labels.write_text('\n'.join(classes) + '\n')
    manifest = dict(id=kind, classes=classes, input_size=[640, 640], precision='int8',
        source_pt_sha256=digest(source), onnx_sha256=digest(onnx_file), rknn_sha256=digest(destination / 'model.rknn'),
        labels_sha256=digest(labels), calibration_images=len(calibration), validation_images=len(validation),
        onnx_max_absolute_error=maximum_error, confidence=threshold, pad_value=114,
        toolkit=version('rknn-toolkit2'), exporter='airockchip/ultralytics_yolo11@0692e9297670acf4cc6d0cec773d7a9493cb8a5f',
        seconds=time.monotonic() - started, tested_on_board=False)
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    records = []
    for path in validation:
        image, geometry = preprocess(path)
        results = decode(session.run(None, {'images': image}), geometry, threshold)
        records.append(dict(file=path.relative_to(work).as_posix(), width=geometry[0], height=geometry[1], detections=results))
    (destination / 'reference.json').write_text(json.dumps(records, indent=2))
    print(json.dumps(manifest), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', type=Path, default=Path('/work'))
    parser.add_argument('--kind', choices=('coco80', 'helmet'), required=True)
    args = parser.parse_args()
    import torch
    torch.set_num_threads(4)
    convert(args.work, args.kind)
