#!/usr/bin/env python3
"""Validate built package identity, dependency, payload and catalog consistency."""
import hashlib,json,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
folder=ROOT/'dist/release-packages'
catalog=json.loads((folder/'package-catalog.json').read_text())['packages']
expected={json.loads(p.read_text())['id']:json.loads(p.read_text()) for p in (ROOT/'riscrte/Drivers').glob('*/manifest.json')}
assert len(catalog)==len(expected)==14
for row in catalog:
    source=expected.pop(row['id'])
    assert row['version']==source['version'] and row['artifact']==source['file_name']=='driver.elf'
    blob=(folder/row['archive']).read_bytes()
    assert len(blob)==row['size_bytes'] and hashlib.sha256(blob).hexdigest()==row['sha256']
    with zipfile.ZipFile(folder/row['archive']) as z:
        assert len(z.namelist())==len(set(z.namelist()))==3
        manifest=json.loads(z.read('.package.json'))
        assert manifest['id']==source['id'] and manifest['version']==source['version']
        assert manifest['provides']==source['provides'] and manifest['driver_abi']==2
        assert manifest['requires']==[{'capability':r['capability'],'min_api':r['api']} for r in source['requires']]
        assert z.read('driver.elf')[:4]==b'\x7fELF'
        metadata=z.read('provider-abi.v1').decode()
        assert f"provides={source['provides'][0]['capability']}\n" in metadata
        for entry in manifest['entries']:
            payload=z.read(entry['name'])
            assert len(payload)==entry['size_bytes'] and hashlib.sha256(payload).hexdigest()==entry['sha256']
assert not expected
print('14 driver package identities, dependencies, metadata and payload hashes passed')
