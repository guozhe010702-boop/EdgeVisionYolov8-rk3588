"""Download public indoor and hardhat model assets into the ignored local cache."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / '.cache/indoor-upgrade'
HELMET_REV = '287bafa2feb311ee45d21f9e9b33315ff6ff955d'


def download(url, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        error = None
        for attempt in range(3):
            try:
                request = urllib.request.Request(url, headers={'User-Agent': 'EdgeVision-model-setup/1.0'})
                with urllib.request.urlopen(request, timeout=90) as response, path.with_suffix(path.suffix + '.part').open('wb') as output:
                    while True:
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        output.write(block)
                path.with_suffix(path.suffix + '.part').replace(path)
                break
            except OSError as problem:
                error = problem
                time.sleep(attempt + 1)
        else:
            raise error
    actual_digest=hashlib.sha256(path.read_bytes()).hexdigest()
    lock=ROOT/'models.lock.json'
    if lock.exists():
        entries=json.loads(lock.read_text(encoding='utf-8')).get('sources',[])
        expected=next((item['sha256'] for item in entries if Path(item['file'].replace('\\','/')).name==path.name),None)
        if expected and actual_digest!=expected:
            raise ValueError('Downloaded asset differs from the locked hash: '+path.name)
    print('Ready: {} ({} bytes)'.format(path.name, path.stat().st_size), flush=True)
    return {'file': str(path.relative_to(WORK)), 'bytes': path.stat().st_size,
            'sha256': actual_digest, 'url': url.split('?')[0]}


def helmet_split(split, count):
    query = urllib.parse.urlencode({'dataset': 'keremberke/hard-hat-detection',
        'config': 'full', 'split': split, 'offset': 0, 'length': count})
    with urllib.request.urlopen('https://datasets-server.huggingface.co/rows?' + query, timeout=60) as response:
        data = json.load(response)
    entries = []
    def fetch(item):
        row = item['row']
        filename = 'helmet/{}/{:04d}.jpg'.format(split, item['row_idx'])
        receipt = download(row['image']['src'], WORK / filename)
        receipt.pop('url', None)  # short-lived public CDN URL; use dataset row identity
        return dict(receipt, dataset='keremberke/hard-hat-detection', split=split,
            row_index=item['row_idx'], width=row.get('width'), height=row.get('height'), objects=row.get('objects'))
    with ThreadPoolExecutor(max_workers=4) as pool:
        entries = list(pool.map(fetch, data['rows']))
    return entries


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    tasks = [
        ('https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8s.pt', WORK / 'source/yolov8s.pt'),
        ('https://raw.githubusercontent.com/airockchip/rknn_model_zoo/main/examples/yolov8/model/coco_80_labels_list.txt', WORK / 'source/coco80.txt'),
        ('https://huggingface.co/keremberke/yolov8n-hard-hat-detection/resolve/' + HELMET_REV + '/best.pt', WORK / 'source/helmet-yolov8n.pt'),
        ('https://github.com/ultralytics/assets/releases/download/v0.0.0/coco128.zip', WORK / 'source/coco128.zip'),
    ]
    with ThreadPoolExecutor(max_workers=4) as pool:
        sources = list(pool.map(lambda pair: download(*pair), tasks))
    print(json.dumps({'sources': sources}), flush=True)
    dataset = WORK / 'datasets'
    dataset.mkdir(exist_ok=True)
    with zipfile.ZipFile(WORK / 'source/coco128.zip') as archive:
        for item in archive.infolist():
            target = (dataset / item.filename).resolve()
            if dataset.resolve() not in target.parents:
                raise ValueError('Invalid dataset archive member')
        archive.extractall(dataset)
    calibration = helmet_split('train', 32)
    validation = helmet_split('validation', 16)
    manifest = dict(sources=sources, helmet_revision=HELMET_REV,
        helmet_calibration=calibration, helmet_validation=validation,
        coco_note='COCO128 is a sanity/quantization set, not an unseen accuracy benchmark.')
    (WORK / 'assets.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'ready': True, 'helmet_calibration': len(calibration),
        'helmet_validation': len(validation), 'manifest': str(WORK / 'assets.json')}), flush=True)


if __name__ == '__main__':
    main()
