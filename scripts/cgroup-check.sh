#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
from pathlib import Path
import json
group = next(line.split(':', 2)[2].strip() for line in Path('/proc/self/cgroup').read_text().splitlines() if line.startswith('0::'))
mount = None
for line in Path('/proc/self/mountinfo').read_text().splitlines():
    if ' - cgroup2 ' in line:
        fields = line.split()
        mount = Path(fields[4])
        mount_root = fields[3]
        break
if mount is None:
    raise SystemExit('cgroup v2 is required; enable systemd on this Ubuntu/WSL host')
relative = group.removeprefix(mount_root).lstrip('/') if mount_root != '/' else group.lstrip('/')
directory = mount / relative
values = {name: (directory / name).read_text().strip() for name in ('memory.max', 'memory.high', 'memory.swap.max', 'cpu.max')}
limits = {'memory.max': 12 * 1024**3, 'memory.high': 10 * 1024**3, 'memory.swap.max': 1024**3}
for name, maximum in limits.items():
    if values[name] == 'max' or int(values[name]) > maximum:
        raise SystemExit(f'Resource limit not enforced: {name}={values[name]}')
quota, period = values['cpu.max'].split()
if quota == 'max' or int(quota) > 4 * int(period):
    raise SystemExit('CPU quota is not enforced')
print('Verified cgroup limits: ' + json.dumps(values), flush=True)
PY
export K618_LIMITS_VERIFIED=1
exec "$@"
