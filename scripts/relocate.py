"""Check that the Linux source snapshot has no private absolute symlink targets."""
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
if not (root / '.xiaomi13-k618-workspace').is_file():
    raise SystemExit('Workspace marker missing')
checked = 0
for directory, dirs, files in os.walk(root, followlinks=False):
    for name in dirs + files:
        path = Path(directory) / name
        if not path.is_symlink():
            continue
        value = os.readlink(path)
        if value.startswith('/') and not value.startswith(('/usr/', '/lib/', '/bin/')):
            if not Path(value).resolve().is_relative_to(root):
                raise SystemExit(f'Nonportable absolute symlink: {path.relative_to(root)}')
        checked += 1
print(f'Checked {checked} source symlinks')
