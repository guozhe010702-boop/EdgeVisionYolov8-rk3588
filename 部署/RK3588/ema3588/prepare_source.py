"""Pack only the sources and ARM64 SDK files required by the native build."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import subprocess

HERE = Path(__file__).resolve().parent
SDK = HERE.parent / 'c_tuili'
ROOT = HERE.parents[2]


def main():
    destination = ROOT / '.cache' / 'ema3588-source.tar.gz'
    destination.parent.mkdir(exist_ok=True)
    sources = {
        SDK / 'examples/yolov8/cpp': 'upstream/cpp',
        SDK / 'utils': 'upstream/utils',
        SDK / '3rdparty/rknpu2/include': 'upstream/rknn/include',
        SDK / '3rdparty/rknpu2/Linux/aarch64/librknnrt.so': 'upstream/rknn/librknnrt.so',
        SDK / '3rdparty/librga/include': 'upstream/rga/include',
        SDK / '3rdparty/librga/Linux/aarch64/librga.so': 'upstream/rga/librga.so',
        SDK / '3rdparty/stb_image': 'upstream/stb_image',
        SDK / 'install/rk3588_linux_aarch64/rknn_yolov8_demo/model': 'upstream/model',
    }
    bundles=ROOT/'.cache/indoor-upgrade/bundle'
    lock_path=ROOT/'models.lock.json'
    if lock_path.exists() and all((bundles/kind/'model.rknn').is_file() for kind in ('coco80','helmet')):
        locked=json.loads(lock_path.read_text(encoding='utf-8'))
        for kind in ('coco80','helmet'):
            model=bundles/kind/'model.rknn'
            if hashlib.sha256(model.read_bytes()).hexdigest()!=locked['models'][kind]['rknn_sha256']:
                raise ValueError('Model bundle differs from models.lock.json: '+kind)
            for name in ('model.rknn','classes.txt','manifest.json'):
                sources[bundles/kind/name]='model-bundle/'+kind+'/'+name
    with tarfile.open(destination, 'w:gz') as archive:
        source_hashes = {}
        for path, name in sources.items():
            archive.add(path, arcname=name)
            files = path.rglob('*') if path.is_dir() else [path]
            for item in files:
                if item.is_file():
                    relative = name + '/' + item.relative_to(path).as_posix() if path.is_dir() else name
                    source_hashes[relative] = hashlib.sha256(item.read_bytes()).hexdigest()
        for path in HERE.iterdir():
            if path.is_file() and path.suffix in ('.txt', '.sh', '.py', '.json'):
                archive.add(path, arcname='ema3588/' + path.name)
                source_hashes['ema3588/' + path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        archive.add(SDK / 'LICENSE', arcname='upstream/licenses/RKNN-Model-Zoo-LICENSE')
        manifest = {'upstream_repository': 'https://github.com/Alba8228/EdgeVision-RK3588',
                    'upstream_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    'source_sha256': source_hashes}
        data = json.dumps(manifest, indent=2).encode()
        item = tarfile.TarInfo('source-manifest.json')
        item.size = len(data)
        archive.addfile(item, io.BytesIO(data))
        for name in ('README_EMA3588.md', 'VALIDATION_EMA3588.md', 'models.lock.json'):
            if (ROOT / name).exists():
                archive.add(ROOT / name, arcname=name)
    print(json.dumps({'archive': str(destination), 'bytes': destination.stat().st_size,
                      'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()}, ensure_ascii=True))


if __name__ == '__main__':
    main()
