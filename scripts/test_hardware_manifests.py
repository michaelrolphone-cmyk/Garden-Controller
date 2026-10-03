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
# Shared schema permits physically absent reset without inventing reset pulses.
for typ in ('display.spi','touch.i2c'):
    index=next(i for i,d in enumerate(board['devices']) if d['config_type']==typ)
    case=deepcopy(board);config=case['devices'][index]['config']
    config.update(reset=-1,reset_assert_ms=0,reset_recovery_ms=0)
    validate(case,manifests)
    for field,value in [('reset_assert_ms',1),('reset_recovery_ms',1),('reset',-2)]:
        invalid=deepcopy(case);invalid['devices'][index]['config'][field]=value
        try:validate(invalid,manifests)
        except ValueError:pass
        else:raise AssertionError('invalid absent reset admitted')
    for field,value in [('reset_assert_ms',0),('reset_recovery_ms',0),('reset_assert_ms',501)]:
        rejected(lambda b,f=field,v=value,i=index:b['devices'][i]['config'].__setitem__(f,v))
    rejected(lambda b,i=index:b['devices'][i]['config'].pop('reset_assert_ms'))
# Logical indices preserve their value and explicitly map to a physical owner.
case=deepcopy(board);case['buses'][0].update(controller_namespace='riscrte.logical',controller=0,physical_controller=2)
validate(case,manifests)
rejected(lambda b:b['buses'][0].pop('controller_namespace'))
rejected(lambda b:b['buses'][0].update(controller_namespace='unknown'))
rejected(lambda b:b['buses'][0].update(controller_namespace='riscrte.logical'))
rejected(lambda b:b['buses'][0].update(physical_controller=3))
# Two different logical controller IDs cannot alias one physical SPI peripheral.
case=deepcopy(case);alias=deepcopy(case['buses'][0]);alias.update(instance_id=100,controller=1);alias['pins']={'sclk':8,'mosi':12,'miso':15};case['buses'].append(alias)
try:validate(case,manifests)
except ValueError:pass
else:raise AssertionError('physical controller alias admitted')
print('absent/present reset boundaries and explicit controller namespace mapping passed')
