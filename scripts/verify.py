"""Validate a rebuilt v4 boot image, exact AK3 payload and all module CRCs."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zipfile


def check(condition, message):
    if not condition:
        raise SystemExit(message)


def cpio_files(data):
    files = {}
    pos = 0
    while pos < len(data):
        while pos < len(data) and data[pos] == 0:
            pos += 1
        if pos == len(data):
            break
        check(data[pos:pos + 6] in (b'070701', b'070702'), f'Invalid cpio header at {pos}')
        fields = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        name = data[pos + 110:pos + 110 + namesize - 1].decode('utf-8')
        start = (pos + 110 + namesize + 3) & ~3
        end = start + size
        check(end <= len(data), f'Truncated cpio member: {name}')
        if name != 'TRAILER!!!':
            files[name.removeprefix('./')] = data[start:end]
        pos = (end + 3) & ~3
    return files


def main():
    dist = Path(sys.argv[1]).resolve()
    project = Path(__file__).resolve().parents[1]
    build = Path(os.environ['K618'])
    image = (dist / 'boot.img').read_bytes()
    check(image[:8] == b'ANDROID!', 'Not an Android boot image')
    kernel_size, ramdisk_size = struct.unpack_from('<II', image, 8)
    version = struct.unpack_from('<I', image, 40)[0]
    check(version == 4, f'Expected boot header v4, got v{version}')
    kernel = image[4096:4096 + kernel_size]
    rdstart = 4096 + ((kernel_size + 4095) & ~4095)
    ramdisk = image[rdstart:rdstart + ramdisk_size]
    cmdline = image[44:1580].split(b'\0', 1)[0].decode()
    check(len(kernel) == kernel_size and len(ramdisk) == ramdisk_size, 'Truncated boot image')
    with zipfile.ZipFile(dist / 'nuwa-kernel-6.18-AnyKernel3.zip') as archive:
        check(archive.testzip() is None, 'AK3 zip CRC failure')
        check(archive.read('Image') == kernel, 'AK3 kernel differs from boot.img')
        check(archive.read('ramdisk-new.cpio.lz4') == ramdisk, 'AK3 ramdisk differs from boot.img')
        check(archive.read('anykernel.sh') == (project / 'tools/ak3/anykernel-618.sh').read_bytes(), 'AK3 installer differs')
    with zipfile.ZipFile(dist / 'nuwa-618-v3.zip') as archive:
        check(archive.testzip() is None and 'module.prop' in archive.namelist(), 'Invalid tuning-module zip')
        expected = {str(p.relative_to(project / 'module/nuwa-618')): p.read_bytes()
                    for p in (project / 'module/nuwa-618').rglob('*') if p.is_file()}
        installed = {name for name in archive.namelist() if not name.endswith('/')}
        check(installed == set(expected), 'Tuning-module file list differs from source')
        for name, value in expected.items():
            check(archive.read(name) == value, 'Tuning-module source mismatch: ' + name)
    raw = subprocess.run(['lz4', '-dcq'], input=ramdisk, stdout=subprocess.PIPE, check=True).stdout
    files = cpio_files(raw)
    # The boot ramdisk is the project's module layer only; the device's own ramdisks (vendor_boot,
    # init_boot or recovery) are unpacked first and supply init, fstab and the rest.
    for name in ('k618_umh', 'lib/modules/modules.load', 'lib/modules/modules.load.recovery',
                 'lib/modules/modules.load.charger', 'lib/modules/k618_batch.load'):
        check(name in files, f'Ramdisk is missing {name}')
    foreign = sorted(n for n in files if n not in ('k618_umh', 'lib') and not n.startswith('lib/'))
    check(not foreign, 'Ramdisk carries more than the module layer: ' + ', '.join(foreign[:10]))
    # The kernel carries the same layer (CONFIG_INITRAMFS_SOURCE) and unpacks it after the
    # bootloader's ramdisks; INITRAMFS_FORCE would drop the device's own ramdisks.
    embedded = build / 'boot-test/initramfs.cpio'
    check(embedded.is_file() and embedded.read_bytes() == raw,
          'Built-in initramfs differs from the boot image ramdisk layer')
    ir_config = (build / 'out-gki-ir/.config').read_text().splitlines()
    check(any(line.startswith('CONFIG_INITRAMFS_SOURCE=') and line.endswith('/initramfs.cpio"')
              for line in ir_config), 'Embedded-initramfs kernel has no module-layer source')
    check('CONFIG_INITRAMFS_FORCE=y' not in ir_config, 'INITRAMFS_FORCE is enabled')
    modules = {Path(n).name: value for n, value in files.items() if n.startswith('lib/modules/') and n.endswith('.ko')}
    for listing in ('modules.load', 'k618_batch.load'):
        for name in files['lib/modules/' + listing].decode().splitlines():
            check(name in modules, f'{listing} names missing module {name}')
    check('mibbr.ko' in modules and
          'mibbr.ko' in files['lib/modules/k618_batch.load'].decode().splitlines(),
          'Official TCP policy module missing from the load list')
    check(files['lib/modules/modules.load.recovery'] == files['lib/modules/modules.load'] ==
          files['lib/modules/modules.load.charger'], 'Recovery/charger first-stage lists differ')
    # Drivers for hardware of the 13 and 13 Ultra (k618_timg.sh DEVICES=1): batch only.
    batch_list = files['lib/modules/k618_batch.load'].decode().splitlines()
    device_modules = ['qcom-i2c-pmic.ko', 'qcom_pm8008-regulator.ko', 'stmvl53l5.ko']
    missing_devices = [m for m in device_modules if m not in modules or m not in batch_list]
    check(not missing_devices, 'Device driver modules missing from the batch: ' + ', '.join(missing_devices))
    # Use the original port's ELF decoder, but enforce failure instead of merely reporting.
    decoder_path = project / 'kernel-6.18/k618_kmi_check.py'
    namespace = {'__name__': 'kmi_decoder'}
    source = decoder_path.read_text(encoding='utf-8')
    exec(compile(source.rsplit('\nmain()', 1)[0], str(decoder_path), 'exec'), namespace)
    core_lines = []
    symbols = {}
    for line in (build / 'out-gki/Module.symvers').read_text().splitlines():
        fields = line.split('\t')
        if len(fields) >= 3 and fields[2] == 'vmlinux':
            core_lines.append(line)
            symbols[fields[1]] = int(fields[0], 16)
    # Other rows describe GKI modules, some of which this device replaces with vendor
    # implementations. Only vmlinux is always present; depmod reads the actual staged
    # providers for everything else, including the vendor QMI implementation.
    kernel_symvers = dist / 'kernel.symvers'
    kernel_symvers.write_text('\n'.join(core_lines) + '\n', encoding='utf-8')
    # Inspect the actual installed module set: superseded build-tree Module.symvers files
    # are not authoritative when a device-specific implementation replaces a techpack.
    depmod = (build / 'boot-test/depmod.err').read_text(errors='replace')
    errors = [line for line in depmod.splitlines()
              if 'unknown symbol' in line.lower() or 'disagrees about version' in line.lower()]
    check(not errors, 'depmod reported unresolved/versioned symbols:\n' + '\n'.join(errors[:30]))
    release = (build / 'out-gki/include/config/kernel.release').read_text().strip()
    version_check = subprocess.run(
        ['depmod', '-n', '-e', '-E', str(kernel_symvers),
         '-b', str(build / 'boot-test/stage/mods'), release],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    check(version_check.returncode == 0, 'depmod version check failed: ' + version_check.stderr)
    version_errors = [line for line in version_check.stderr.splitlines()
                      if 'unknown symbol' in line.lower() or 'version' in line.lower() and 'disagree' in line.lower()]
    check(not version_errors, 'depmod CRC check failed:\n' + '\n'.join(version_errors[:30]))
    crc_mismatches = []
    imports_checked = 0
    for path in (build / 'boot-test/stage/rd/lib/modules').glob('*.ko'):
        for name, crc in (namespace['versions'](path) or {}).items():
            if name in symbols:
                imports_checked += 1
                if symbols[name] != crc:
                    crc_mismatches.append(f'{path.name}: {name}')
    check(not crc_mismatches, 'KMI CRC mismatch: ' + ', '.join(crc_mismatches[:30]))
    check(imports_checked > 0, 'No versioned imports were inspected')
    config = (build / 'out-gki/.config').read_text()
    for option in ('CONFIG_KSU=y', 'CONFIG_KSU_SUSFS=y', 'CONFIG_ZRAM_DEF_COMP="lz4"',
                   'CONFIG_KFENCE_DEFERRABLE=y', 'CONFIG_TMPFS_XATTR=y',
                   'CONFIG_RCU_LAZY=y', 'CONFIG_RCU_NOCB_CPU=y',
                   'CONFIG_TCP_CONG_BBR=y', 'CONFIG_TCP_CONG_CUBIC=y',
                   'CONFIG_NET_SCH_FQ=y', 'CONFIG_NET_SCH_FQ_CODEL=y',
                   'CONFIG_DEFAULT_TCP_CONG="cubic"'):
        check(option in config.splitlines(), f'Missing kernel option {option}')
    for argument in ('trace_buf_size=8K', 'rcu_nocbs=all', 'rcutree.enable_rcu_lazy=1'):
        check(argument in cmdline.split(), f'Missing boot argument {argument}')
    source = build / 'ws/common'
    feature_checks = {
        'kfence_deferrable': 'CONFIG_KFENCE_DEFERRABLE=y' in config.splitlines(),
        'tmpfs_xattr': 'CONFIG_TMPFS_XATTR=y' in config.splitlines(),
        'trace_initial_8k': 'trace_buf_size=8K' in cmdline.split(),
        'lazy_rcu_offload': all(x in cmdline.split() for x in ('rcu_nocbs=all', 'rcutree.enable_rcu_lazy=1')),
        'zram_candidate_slab': 'k618_pp_slab' in (source/'drivers/block/zram/zram_drv.c').read_text(),
        'dm_bufio_budget': 'k618_bufio_budget' in (source/'drivers/md/dm-bufio.c').read_text(),
        'setting_trial_guard': (project/'module/nuwa-618/trial-guard.sh').is_file(),
        'initramfs_last': 'k618 initramfs_last' in (source/'init/initramfs.c').read_text(),
    }
    check(all(feature_checks.values()), 'Missing configured source feature: ' + str(feature_checks))
    report = {
        'status': 'passed', 'kernel_release': release, 'boot_header': version,
        'kernel_bytes': kernel_size, 'ramdisk_bytes': ramdisk_size,
        'module_count': len(modules), 'versioned_imports_checked': imports_checked,
        'crc_mismatches': crc_mismatches, 'depmod_symbol_errors': errors,
        'depmod_version_errors': version_errors,
        'kernel_sha256': hashlib.sha256(kernel).hexdigest(),
        'ramdisk_sha256': hashlib.sha256(ramdisk).hexdigest(), 'cmdline': cmdline,
        'configured_source_features': feature_checks,
        'ramdisk': {'content': 'module layer only (lib/, k618_umh)', 'entries': len(files),
                    'embedded_copy_identical': True, 'device_modules': device_modules},
        'tcp': {'built_in_congestion_control': ['cubic', 'bbr'], 'default': 'cubic',
                'built_in_qdiscs': ['fq', 'fq_codel'], 'vendor_policy_module': 'mibbr.ko'},
        'limits': 'Build, payload and static ABI validation; hardware behavior requires device testing.'
    }
    (dist / 'validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
