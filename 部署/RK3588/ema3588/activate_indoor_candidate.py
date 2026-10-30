"""Activate a validated candidate runtime, retain the complete previous runtime."""
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

import board_control as control

ROOT=Path(__file__).resolve().parent.parent


def main():
    if b'RK3588 EVM3588' not in Path('/proc/device-tree/model').read_bytes():
        raise RuntimeError('This activation is restricted to the verified EVM3588 board.')
    candidate=(ROOT/'ema-runtime-candidate').resolve()
    runtime=(ROOT/'ema-runtime').resolve()
    if candidate.parent!=ROOT.resolve() or runtime.parent!=ROOT.resolve():
        raise RuntimeError('Runtime paths must stay directly inside the project.')
    summary=json.loads((ROOT/'local/indoor-validation/summary.json').read_text())
    for kind in ('coco80','helmet'):
        result=summary['models'][kind]
        if result['images'] < (64 if kind=='coco80' else 16):
            raise RuntimeError('The required public validation set was not completed.')
        for metric in ('precision','recall'):
            if result['onnx_agreement']['overall'][metric]<.85 or result['zero_copy_agreement']['overall'][metric]<.98:
                raise RuntimeError('Candidate inference agreement did not pass.')
        manifest=json.loads((candidate/'models'/kind/'manifest.json').read_text())
        model=candidate/'models'/kind/'model.rknn'
        if hashlib.sha256(model.read_bytes()).hexdigest()!=manifest['rknn_sha256']:
            raise RuntimeError('Candidate model hash changed after conversion.')
    subprocess.run(['sha256sum','--quiet','-c','SHA256SUMS'],cwd=str(candidate),check=True)
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup=ROOT/'backups'/('before-indoor-'+stamp)
    backup.mkdir(parents=True,exist_ok=False)
    previous=control.load_state()
    previous_profile=previous.get('profile',control.active_profile())
    was_running=control.alive(previous)
    old_active=control.ACTIVE_PROFILE.read_bytes() if control.ACTIVE_PROFILE.exists() else None
    if control.STATE.exists():
        shutil.copy2(control.STATE,backup/'previous-session.json')
    preview=ROOT/'ema3588/open_hdmi_preview.py'
    moved_old=False
    try:
        subprocess.run([sys.executable,str(preview),'--stop'],check=True)
        if was_running:
            control.stop()
        runtime.rename(backup/'ema-runtime')
        moved_old=True
        candidate.rename(runtime)
        control.start(take_camera=True,profile='indoor-helmet',restore_on_failure=False)
        subprocess.run([sys.executable,str(preview)],check=True)
        report={'activated':True,'profile':'indoor-helmet','previous_profile':previous_profile,
                'backup':str(backup),'runtime':str(runtime),'session_pid':control.load_state().get('pid')}
        (ROOT/'local/indoor-activation.json').write_text(json.dumps(report,indent=2))
        print(json.dumps(report),flush=True)
    except BaseException:
        if moved_old:
            control.stop()
            if runtime.exists():
                runtime.rename(backup/'failed-candidate-runtime')
            (backup/'ema-runtime').rename(runtime)
        if old_active is None:
            if control.ACTIVE_PROFILE.exists(): control.ACTIVE_PROFILE.unlink()
        else:
            control.ACTIVE_PROFILE.write_bytes(old_active)
        if was_running:
            control.start(take_camera=True,profile=previous_profile)
            subprocess.run([sys.executable,str(preview)],check=False)
        print(json.dumps({'activated':False,'previous_runtime_restored':moved_old,'backup':str(backup)}),flush=True)
        raise


if __name__=='__main__':
    main()
