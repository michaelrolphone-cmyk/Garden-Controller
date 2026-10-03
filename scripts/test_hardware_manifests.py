#!/usr/bin/env python3
"""Host-only physical catalog and cross-instance arbitration regression checks."""
from copy import deepcopy
import json
from hardware_manifests import ROOT,validate,main
manifests=[json.loads(p.read_text()) for p in (ROOT/'riscrte/Drivers').glob('*/manifest.json')]
board=json.loads((ROOT/'riscrte/Drivers/garden_crowpanel/hardware.json').read_text())
def rejected(change):
    case=deepcopy(board);change(case)
    try:validate(case,manifests)
    except (ValueError,KeyError):return
    raise AssertionError('invalid mapping admitted')
main()
rejected(lambda b:b['devices'][0].update(compatible='unknown,chip'))
rejected(lambda b:b['devices'][0]['config'].update(unknown_pin=1))
rejected(lambda b:b['devices'][0]['config'].update(active_high='true'))
rejected(lambda b:b['devices'][0]['config']['pins'].__setitem__(0,-1))
rejected(lambda b:b['devices'][0]['config'].pop('pins'))
rejected(lambda b:b['devices'][0]['config']['pins'].__setitem__(0,b['buses'][0]['pins']['sclk']))
rejected(lambda b:b['devices'][1]['bindings'].update({'input.button':999}))
rejected(lambda b:b['buses'].append(deepcopy(b['buses'][0])))
rejected(lambda b:b['devices'].append(deepcopy(b['devices'][0])))
# One chip can occur twice only on separate resources and with unique instance ID.
case=deepcopy(board);button=deepcopy(case['devices'][0]);button['instance_id']=100;button['config']['pins']=[8]
case['devices'].append(button);validate(case,manifests)
button['config']['pins']=case['devices'][0]['config']['pins']
try:validate(case,manifests)
except ValueError:pass
else:raise AssertionError('duplicate chip GPIO ownership admitted')
print('10 invalid catalog cases and independent same-chip resource mapping passed')
