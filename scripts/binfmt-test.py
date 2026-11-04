"""Create/remove temporary F-only QEMU handlers for the installer fixture."""
from pathlib import Path
import os
import secrets
import sys

directory = Path('/proc/sys/fs/binfmt_misc')
record = Path(sys.argv[2])
if sys.argv[1] == 'remove':
    if record.exists():
        for name in record.read_text().splitlines():
            if not name.startswith('nuwa-k618-test-') or '/' in name:
                raise SystemExit('Unexpected test-handler name')
            handler = directory / name
            if handler.exists():
                handler.write_text('-1\n')
        record.unlink()
elif sys.argv[1] == 'create':
    record.write_text('')
    interpreter_root = Path(sys.argv[3])
    mask = 'ffffffffffffff00fffffffffffffffffeffffff'
    for architecture in ('arm', 'aarch64'):
        interpreter = interpreter_root / f'qemu-{architecture}-static'
        if not interpreter.is_file():
            raise SystemExit('Pinned QEMU interpreter is unavailable')
        magic = ('7f454c4601010100000000000000000002002800' if architecture == 'arm'
                 else '7f454c460201010000000000000000000200b700')
        name = f'nuwa-k618-test-{architecture}-{os.getpid()}-{secrets.token_hex(3)}'
        def escaped(value):
            return ''.join(f'\\x{byte:02x}' for byte in bytes.fromhex(value))
        definition = f":{name}:M:0:{escaped(magic)}:{escaped(mask)}:{interpreter}:F"
        (directory / 'register').write_text(definition)
        with record.open('a') as output:
            output.write(name + '\n')
        print(f'Temporary installer-test handler: {architecture} (F)', flush=True)
else:
    raise SystemExit('Use create or remove')
