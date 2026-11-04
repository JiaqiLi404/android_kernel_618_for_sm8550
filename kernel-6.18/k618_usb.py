#!/usr/bin/env python3
"""Adapt Qualcomm DWC3 endpoint accesses to the indexed ACK register macros."""
from pathlib import Path
import re
import sys

path = Path(sys.argv[1]) / 'drivers/usb/dwc3/dwc3-msm-core.c'
text = path.read_text(encoding='utf-8')
old = re.compile(r'(dwc3_msm_ep_(?:readl|writel))\(dep->regs, (DWC3_DEPCMD(?:PAR[012])?)(?=[,)])')
new = re.compile(r'dwc3_msm_ep_(?:readl|writel)\(dwc->regs, DWC3_DEPCMD(?:PAR[012])?\(dep->number\)')
physical_old = 'DWC3_DEP_BASE(dep->number) + DWC3_DEPCMD'
physical_new = 'DWC3_DEPCMD(dep->number)'
matches = len(old.findall(text))
if matches:
    if matches != 6 or text.count(physical_old) != 1:
        raise SystemExit('k618_usb: unexpected DWC3 endpoint-access source layout')
    text = old.sub(lambda m: f'{m[1]}(dwc->regs, {m[2]}(dep->number)', text)
    text = text.replace(physical_old, physical_new)
    path.write_text(text, encoding='utf-8', newline='\n')
    print('k618_usb: endpoint registers use the controller base and indexed offsets')
elif len(new.findall(text)) == 6 and physical_old not in text:
    print('k618_usb: endpoint-register adaptation already applied')
else:
    raise SystemExit('k618_usb: unrecognized or partially adapted DWC3 source')
