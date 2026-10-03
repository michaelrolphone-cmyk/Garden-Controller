#!/usr/bin/env python3
"""Compile and run host-only capability fixtures. Never opens a device."""
import os, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CASES=[('relay',1,1),('buzzer',2,1),('button',3,2),('led',4,2),('garden_encoder',5,2),('pixel',6,2),('panel',7,2),('epaper',8,3),('storage',9,3),('touch',10,2),('wifi',11,1),('garden_relay6',12,1),('garden_crowpanel',12,2),('garden_paper',12,3)]
with tempfile.TemporaryDirectory(prefix='garden-contracts-') as tmp:
    for folder,case,board in CASES:
        output=Path(tmp)/folder
        command=[os.environ.get('CC','cc'),'-std=c11','-g','-fsanitize=undefined','-fno-omit-frame-pointer',f'-DTEST_ID={case}',f'-DTEST_BOARD={board}',f'-DDRIVER_SOURCE="{folder}/driver.c"',f'-I{ROOT}/riscrte/sdk',f'-I{ROOT}/riscrte/Drivers',str(ROOT/'riscrte/test/contracts/driver_check.c'),'-o',str(output)]
        for variant in [0,1]:
            subprocess.run(command+[f'-DALT_MAP={variant}'],check=True)
            subprocess.run([str(output)],check=True,timeout=30)
