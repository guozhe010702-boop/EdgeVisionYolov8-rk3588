"""Real RKNN model/label mismatch checks; no camera access."""
import json
import os
from pathlib import Path
import subprocess
import sys

runtime=Path(sys.argv[1])
data=Path(sys.argv[2])
image=data/'helmet/validation/0000.jpg'
checks=[]
cases=[('wrong_label_count', 'coco80', runtime/'model/coco_80_labels_list.txt', '.35'),
       ('wrong_label_capacity', 'helmet', runtime/'models/coco80/classes.txt', '.5'),
       ('invalid_threshold', 'coco80', runtime/'models/coco80/classes.txt', 'nan')]
for name,kind,labels,threshold in cases:
    environment=dict(os.environ,LD_LIBRARY_PATH=str(runtime/'lib'),EDGEVISION_LABELS=str(labels),
                     EDGEVISION_CONFIDENCE=threshold,EDGEVISION_NO_IMAGE='1')
    result=subprocess.run([str(runtime/'rknn_yolov8_demo_zero_copy'),str(runtime/'models'/kind/'model.rknn'),str(image)],
        env=environment,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=20)
    assert result.returncode!=0 and 'DETECTION_DONE' not in result.stdout,(name,result.stdout)
    checks.append({'case':name,'rejected':True,'exit_code':result.returncode})
print(json.dumps({'model_contract_checks':checks}))
