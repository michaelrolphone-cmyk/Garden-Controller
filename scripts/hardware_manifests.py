#!/usr/bin/env python3
"""Validate Garden's board catalogs, cross-device ownership and driver matches.
No hardware discovery; mappings must be explicitly supplied by board authors.
"""
import argparse,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def require(value,message):
    if not value:raise ValueError(message)
def shape(value,schema):
    """Validate the vocabulary used by our checked-in schema (no external refs)."""
    for constraint in schema.get('allOf',[]):shape(value,constraint)
    if 'oneOf' in schema:
        matches=0
        for option in schema['oneOf']:
            try:shape(value,option);matches+=1
            except ValueError:pass
        require(matches==1,'config schema mismatch')
    if 'const' in schema:require(type(value) is type(schema['const']) and value==schema['const'],'constant')
    if 'enum' in schema:require(value in schema['enum'],'enum')
    kind=schema.get('type')
    if kind=='object':
        require(type(value) is dict,'object')
        require(all(k in value for k in schema.get('required',[])),'missing field')
        properties=schema.get('properties',{});extra=schema.get('additionalProperties',True)
        for k,v in value.items():
            if k in properties:shape(v,properties[k])
            elif extra is False:raise ValueError('unknown field '+k)
            elif isinstance(extra,dict):shape(v,extra)
    elif kind=='array':
        require(type(value) is list and schema.get('minItems',0)<=len(value)<=schema.get('maxItems',2**31),'array bounds')
        if schema.get('uniqueItems'):require(len({json.dumps(v,sort_keys=True) for v in value})==len(value),'duplicate array item')
        for v in value:shape(v,schema['items'])
    elif kind=='integer':require(type(value) is int and schema.get('minimum',-2**63)<=value<=schema.get('maximum',2**63-1),'integer bounds')
    elif kind=='boolean':require(type(value) is bool,'boolean')
    elif kind=='string':require(type(value) is str and '\0' not in value and schema.get('minLength',0)<=len(value.encode('utf-8'))<=schema.get('maxLength',2**31),'string bounds')
SCHEMA=json.loads((ROOT/'riscrte/hardware/board-manifest-v1.schema.json').read_text())
def validate(data,manifests):
    shape(data,SCHEMA)
    require(data.get('schema')=='riscrte.board-hardware' and data.get('schema_version')==1,'board schema')
    require(isinstance(data.get('board_id'),str) and data.get('board_id') and isinstance(data.get('revision'),str),'board identity')
    require(0<=len(data['buses'])<=8 and 1<=len(data['devices'])<=64,'catalog bounds')
    occupied={};buses={};devices={}
    def claim(pin,owner):
        require(type(pin) is int and -1<=pin<=48,'pin range')
        if pin==-1:return
        require(pin not in occupied,f'pin conflict {pin}: {owner} / {occupied.get(pin)}')
        occupied[pin]=owner
    for bus in data['buses']:
        i=bus['instance_id'];require(type(i) is int and i>0 and i not in buses,'duplicate bus')
        buses[i]=bus;require(bus['kind'] in ('spi','i2c') and bus['mode']==0 and 0<=bus['controller']<=3,'bus config')
        namespace=bus.get('controller_namespace')
        require(namespace in ('esp32.peripheral','riscrte.logical'),'explicit controller namespace required for mapping')
        physical=bus.get('physical_controller',bus['controller'])
        require(namespace!='esp32.peripheral' or physical==bus['controller'],'physical controller identity mismatch')
        required=('sclk','mosi','miso') if bus['kind']=='spi' else ('sda','scl')
        require(set(bus['pins'])==set(required),'bus pins')
        require(0<bus['frequency_hz']<=(10000000 if bus['kind']=='spi' else 400000),'bus frequency')
        for name,pin in bus['pins'].items():
            require(pin>=0 or name=='miso','missing required bus pin');claim(pin,f'bus {i}')
        for other in list(buses.values())[:-1]:require((other['kind'],other.get('physical_controller',other['controller']))!=(bus['kind'],physical),'one descriptor per controller')
    for dev in data['devices']:
        i=dev['instance_id'];require(type(i) is int and i>0 and i not in devices,'duplicate device instance')
        devices[i]=dev
        require(dev['compatible']==dev['chip']['vendor']+','+dev['chip']['model'],'vendor/model compatibility mismatch')
        require(dev['config_version']==1,'config version')
        matches=[m for m in manifests if any(h['compatible']==dev['compatible'] and dev['chip']['revision'] in h['revisions'] and h['config_type']==dev['config_type'] and h['config_version']==dev['config_version'] for h in m.get('hardware_compatibility',[]))]
        require(len(matches)==1,'missing or ambiguous compatible driver')
        c=dev['config'];typ=dev['config_type'];pins=[]
        if 'bus_instance_id' in c:
            require(c['bus_instance_id'] in buses,'missing bus')
            b=buses[c['bus_instance_id']]
            require(b['kind']==('i2c' if typ=='touch.i2c' else 'spi'),'wrong bus type')
        if typ=='gpio.bank':
            require(1<=len(c['pins'])<=8,'bank count');pins=c['pins']
        elif typ=='input.quadrature':pins=[c['a'],c['b']]
        elif typ=='pixel.ws2812':pins=[c['pin']];require(1<=c['count']<=16 and c['order'] in (0,1),'pixel config')
        elif typ=='display.spi':
            require(len(c['power_pins'])==len(c['power_active_high'])<=4,'power config')
            pins=[c[k] for k in ('cs','dc','reset','backlight','busy')]+c['power_pins']
        elif typ=='touch.i2c':
            pins=[c['reset'],c['irq']];require(8<=c['address']<=0x77,'I2C address')
            for other in list(devices.values())[:-1]:
                if other['config_type']=='touch.i2c':require((other['config']['bus_instance_id'],other['config']['address'])!=(c['bus_instance_id'],c['address']),'duplicate I2C address')
        elif typ=='storage.sd-spi':pins=[c[k] for k in ('cs','detect','write_protect')]
        elif typ=='radio.integrated':require(0<=c['unit']<=3 and 1<=c['features']<=3,'radio config')
        else:raise ValueError('unsupported config type')
        if typ=='gpio.bank':require(all(p>=0 for p in pins),'missing required device pin')
        elif typ=='display.spi':require(all(c[k]>=0 for k in ('cs','dc')) and all(p>=0 for p in c['power_pins']),'missing display pin')
        elif typ=='touch.i2c':require(c['irq']>=0,'missing touch IRQ')
        elif typ=='storage.sd-spi':require(c['cs']>=0,'missing storage CS')
        else:require(all(p>=0 for p in pins),'missing required device pin')
        for pin in pins:claim(pin,f'device {i}')
    for dev in devices.values():
        for capability,instance in dev.get('bindings',{}).items():
            require(instance in devices and instance!=dev['instance_id'],'missing related instance')
            related=devices[instance]
            require(any(any(h['compatible']==related['compatible'] for h in m.get('hardware_compatibility',[])) and any(p['capability']==capability for p in m['provides']) for m in manifests),'wrong binding capability')
        if dev['config_type']=='input.quadrature':require(dev.get('bindings',{}).get('input.button')==dev['config']['button_instance_id'],'button binding mismatch')
        if dev['compatible']=='generic,gpio-relay-bank':require('sound.buzzer' in dev.get('bindings',{}),'relay sound binding missing')
def generated(text):
    data=json.loads(text)
    return '/* Generated by scripts/hardware_manifests.py --generate. */\n#define BOARD_ID '+json.dumps(data['board_id'])+'\n#define BOARD_REVISION '+json.dumps(data['revision'])+'\nstatic const char board_json[] =\n'+''.join(json.dumps(line)+'\n' for line in text.splitlines(keepends=True))+';\n'
def main(generate=False):
    manifests=[json.loads(p.read_text()) for p in (ROOT/'riscrte/Drivers').glob('*/manifest.json')]
    for path in sorted((ROOT/'riscrte/Drivers').glob('*/hardware.json')):
        validate(json.loads(path.read_text()),manifests)
        header=path.with_name('hardware_json.h');expected=generated(path.read_text())
        if generate:header.write_text(expected)
        require(header.read_text()==expected,f'stale generated header {header}')
    print('3 physical board manifests, bindings, pin ownership and generated catalogs passed')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--generate',action='store_true');main(p.parse_args().generate)
