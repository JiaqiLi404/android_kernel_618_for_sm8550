#!/usr/bin/env python3
"""Reconstruct the kernel build workspace from pinned public upstream inputs."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request

from inputlib.payload import open_ota

PROJECT = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ['BUILD_ROOT']).resolve()
SPEC = json.loads((PROJECT / 'provenance/input-sources.json').read_text())
JOBS = min(4, max(1, int(os.environ.get('JOBS', '4'))))
ENV = dict(os.environ, GIT_TERMINAL_PROMPT='0')
MARKER = ROOT / '.xiaomi13-k618-upstream'


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, env=ENV, **kw)


def git(path, *args, capture=False):
    result = run(['git', '-c', 'pack.threads=4', '-c', 'http.version=HTTP/1.1',
                  '-C', path, *args], capture_output=capture, text=True)
    return result.stdout.strip() if capture else None


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(url, target, digest):
    target = Path(target)
    if target.is_file() and sha(target) == digest:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + '.part')
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'xiaomi13-k618-inputs/1.0'})
            with urllib.request.urlopen(request, timeout=30) as source, temporary.open('wb') as dest:
                shutil.copyfileobj(source, dest, length=1 << 20)
            if sha(temporary) != digest:
                raise RuntimeError(f'SHA-256 mismatch: {target.name}')
            temporary.replace(target)
            return
        except (OSError, RuntimeError):
            if attempt == 2:
                raise
            time.sleep(2 * (attempt + 1))


def checkout(item):
    path = ROOT / item['path']
    path.parent.mkdir(parents=True, exist_ok=True)
    if not (path / '.git').is_dir():
        if path.exists() and any(path.iterdir()):
            raise RuntimeError(f'Nonempty non-repository input directory: {item["path"]}')
        run(['git', 'init', '-q', path])
        git(path, 'remote', 'add', 'origin', item['url'])
        git(path, 'config', 'core.autocrlf', 'false')
    if item.get('target_tree'):
        current = subprocess.run(['git', '-C', str(path), 'rev-parse', 'HEAD^{tree}'],
                                 capture_output=True, text=True)
        if current.returncode == 0 and current.stdout.strip() == item['target_tree']:
            return
    present = subprocess.run(['git', '-C', str(path), 'cat-file', '-e', item['revision']+'^{commit}'],
                             capture_output=True).returncode == 0
    if not present:
        flags = [] if item.get('full_history') else ['--depth=1']
        if item.get('sparse') or item.get('full_history'):
            git(path, 'config', 'remote.origin.promisor', 'true')
            git(path, 'config', 'remote.origin.partialclonefilter', 'blob:none')
            flags += ['--filter=blob:none']
        git(path, 'fetch', '-q', '--no-tags', *flags, 'origin', item['revision'])
    if item.get('sparse'):
        git(path, 'sparse-checkout', 'init', '--no-cone')
        run(['git', '-C', path, 'sparse-checkout', 'set', '--no-cone', '--stdin'],
            input='\n'.join(item['sparse'])+'\n', text=True)
    git(path, '-c', 'advice.detachedHead=false', 'checkout', '-q', '--detach', item['revision'])
    if item.get('patch'):
        record = json.loads((PROJECT / 'provenance/soc-baseline.json').read_text())
        patch = PROJECT / item['patch']
        if sha(patch) != record['patch_sha256']:
            raise RuntimeError('SoC adaptation patch checksum mismatch')
        git(path, 'apply', '--index', '--whitespace=nowarn', patch)
        if git(path, 'write-tree', capture=True) != item['target_tree']:
            raise RuntimeError('SoC reconstruction tree differs from the recorded integration')
        git(path, '-c', 'user.name=Xiaomi 13 Pro Kernel Contributors',
            '-c', 'user.email=kernel@localhost', 'commit', '-q', '-m', 'Apply SM8550 device integration')
        git(path, 'config', 'core.excludesFile', PROJECT / 'kernel-6.18/k618_soc_gitignore.txt')


def vendor_cache():
    path = ROOT / 'vendor-study/mods.git'
    if not (path / 'HEAD').is_file():
        path.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'init', '--bare', '-q', path])
    for item in SPEC['vendor_cache']:
        existing = subprocess.run(['git', '-C', str(path), 'rev-parse', item['ref']],
                                  capture_output=True, text=True)
        if existing.returncode == 0 and existing.stdout.strip() == item['revision']:
            continue
        print('[vendor] '+item['ref'], flush=True)
        git(path, 'fetch', '-q', '--depth=1', '--no-tags', item['url'],
            '+'+item['revision']+':'+item['ref'])


def unpack_modules(image, target):
    target = Path(target)
    if (target / '.input-extracted').exists():
        return
    target.mkdir(parents=True, exist_ok=True)
    run(['fsck.erofs', '--no-preserve', '--extract='+str(target), image], stdout=subprocess.DEVNULL)
    (target / '.input-extracted').touch()


def unpack_vendor_modules(image, target):
    target = Path(target)
    if (target / '.input-extracted').exists():
        return
    work = target / 'unpacked'
    work.mkdir(parents=True, exist_ok=True)
    run(['python3', ROOT / 'home/mkbootimg/unpack_bootimg.py', '--boot_img', image,
         '--out', work], stdout=subprocess.DEVNULL)
    destination = target / 'ramdisk0'
    destination.mkdir(exist_ok=True)
    with subprocess.Popen(['lz4', '-dcq', str(work / 'vendor_ramdisk00')], stdout=subprocess.PIPE) as decomp:
        run(['cpio', '-idm', '--quiet', '--no-absolute-filenames', '--no-preserve-owner', 'lib/modules/*'],
            stdin=decomp.stdout, cwd=destination)
        decomp.stdout.close()
        if decomp.wait():
            raise RuntimeError('vendor ramdisk decompression failed')
    (target / '.input-extracted').touch()


def firmware():
    for item in SPEC['roms']:
        directory = ROOT / '.inputs/roms' / item['name']
        directory.mkdir(parents=True, exist_ok=True)
        needed = [(name, info) for name, info in item['partitions'].items()
                  if not (directory / (name+'.img')).is_file()
                  or sha(directory / (name+'.img')) != info['sha256']]
        if needed:
            local_zip = ROOT / 'downloads' / item['url'].rsplit('/', 1)[-1]
            source = str(local_zip) if local_zip.is_file() else item['url']
            print('[firmware] '+item['name']+' '+item['version'], flush=True)
            ota = open_ota(source)
            if ota.metadata.get('pre-device') != item['name']:
                raise RuntimeError('OTA device metadata mismatch')
            if ota.metadata.get('METADATA_HASH') != item['payload_metadata_sha256']:
                raise RuntimeError('OTA payload metadata differs from the pinned package')
            for name, info in needed:
                part = ota.payload.partitions[name]
                if part.size != info['size'] or part.sha256.hex() != info['sha256']:
                    raise RuntimeError(f'OTA manifest mismatch: {item["name"]}/{name}')
                print('[extract] '+item['name']+'/'+name, flush=True)
                ota.payload.extract(name, str(directory / (name+'.img')), threads=JOBS, verify=True)
                if sha(directory / (name+'.img')) != info['sha256']:
                    raise RuntimeError(f'Extracted partition hash mismatch: {name}')
    nuwa = ROOT / '.inputs/roms/nuwa'
    for relative in ['k618/boot-test', 'nikohyper/img']:
        directory = ROOT / relative
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(nuwa / 'init_boot.img', directory / 'init_boot.img')
    shutil.copy2(nuwa / 'vendor_boot.img', ROOT / 'nikohyper/img/vendor_boot.img')
    unpack_modules(nuwa / 'vendor_dlkm.img', ROOT / 'nikohyper/x/vendor_dlkm_a')
    unpack_vendor_modules(nuwa / 'vendor_boot.img', ROOT / 'stockmods/vendor_boot')
    stock_dlkm = ROOT / 'stockmods/vendor_dlkm'
    if not stock_dlkm.exists() and not stock_dlkm.is_symlink():
        stock_dlkm.symlink_to('../nikohyper/x/vendor_dlkm_a', target_is_directory=True)
    madrid = ROOT / '.inputs/roms/madrid'
    dest = ROOT / 'madrid-OS4.0.24.0.XEOCNXM'
    unpack_vendor_modules(madrid / 'vendor_boot.img', dest / 'vendor_boot')
    for name in ['vendor_dlkm', 'system_dlkm']:
        unpack_modules(madrid / (name+'.img'), dest / name)


def verify():
    report = {'repositories': [], 'partitions': [], 'xiaomi_modules': 0}
    for item in SPEC['repositories']:
        path = ROOT / item['path']
        if item.get('target_tree'):
            if git(path, 'rev-parse', 'HEAD^{tree}', capture=True) != item['target_tree']:
                raise RuntimeError('SoC integration tree mismatch')
        elif git(path, 'rev-parse', 'HEAD', capture=True) != item['revision']:
            raise RuntimeError('Source revision mismatch: '+item['name'])
        report['repositories'].append({'name': item['name'], 'revision': item['revision']})
    for item in SPEC['vendor_cache']:
        if git(ROOT/'vendor-study/mods.git', 'rev-parse', item['ref'], capture=True) != item['revision']:
            raise RuntimeError('Vendor cache revision mismatch')
    for item in SPEC['roms']:
        for name, info in item['partitions'].items():
            path = ROOT / '.inputs/roms' / item['name'] / (name+'.img')
            if path.stat().st_size != info['size'] or sha(path) != info['sha256']:
                raise RuntimeError('Firmware partition mismatch: '+item['name']+'/'+name)
            report['partitions'].append({'name': item['name']+'/'+name, 'sha256': info['sha256']})
    with (PROJECT/'provenance/binary-modules.tsv').open() as stream:
        for item in csv.DictReader(stream, delimiter='\t'):
            path = ROOT / item['source']
            if not path.resolve().is_relative_to(ROOT) or sha(path) != item['sha256']:
                raise RuntimeError('Xiaomi module hash mismatch: '+item['module'])
            report['xiaomi_modules'] += 1
    tools = ['clang/host/linux-x86/clang-r584948c/bin/clang',
             'rust-toolchain/linux-x86/1.91.1.p3/bin/rustc',
             'clang-tools/linux-x86/bin/bindgen', 'kernel-build-tools/linux_musl-x86/bin/pahole']
    for name in tools:
        if not os.access(ROOT/'k618/ws/prebuilts'/name, os.X_OK):
            raise RuntimeError('Missing executable toolchain input: '+name)
    bison_data = ROOT/'k618/ws/prebuilts/build-tools/common/bison/m4sugar/m4sugar.m4'
    if not bison_data.is_file() or bison_data.stat().st_size == 0:
        raise RuntimeError('Missing shared Bison data; rerun bootstrap --only sources')
    if sha(PROJECT/'assets/qemu-user-static.deb') != SPEC['qemu']['sha256']:
        raise RuntimeError('QEMU package hash mismatch')
    report['status'] = 'passed'
    (ROOT/'input-validation.json').write_text(json.dumps(report, indent=2)+'\n')
    print('INPUT VALIDATION PASSED: 14 repositories, 2 vendor refs, 6 partitions, '
          +str(report['xiaomi_modules'])+' pinned Xiaomi modules', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--only', choices=['sources', 'firmware', 'all'], default='all')
    ap.add_argument('--check', action='store_true', help='Verify an already reconstructed workspace')
    args = ap.parse_args()
    if os.environ.get('K618_LIMITS_VERIFIED') != '1':
        ap.error('Use bash scripts/run-limited.sh bootstrap')
    if not MARKER.exists():
        if ROOT.exists() and any(ROOT.iterdir()):
            ap.error('Choose an empty native Linux BUILD_ROOT for upstream reconstruction')
        ROOT.mkdir(parents=True, exist_ok=True)
        MARKER.write_text('Pinned upstream input reconstruction\n')
    if args.check:
        verify()
        return
    if args.only in ['sources', 'all']:
        for item in SPEC['repositories']:
            print('[source] '+item['name'], flush=True)
            checkout(item)
        vendor_cache()
        download(SPEC['qemu']['url'], PROJECT/'assets/qemu-user-static.deb', SPEC['qemu']['sha256'])
    if args.only in ['firmware', 'all']:
        for command in ['fsck.erofs', 'lz4', 'cpio']:
            if not shutil.which(command):
                ap.error('Install host dependency '+command+' (see docs/PROVENANCE.en.md#acquire)')
        firmware()
    if args.only == 'all':
        verify()
        run(['bash', PROJECT/'kernel-6.18/k618_tp_get.sh'])
        (ROOT/'.xiaomi13-k618-workspace').write_text('Upstream sources and official firmware\n')
        (ROOT/'.xiaomi13-k618-ready').write_text('Public input reconstruction verified\n')
        print('Ready for bash scripts/run-limited.sh build', flush=True)


if __name__ == '__main__':
    main()
