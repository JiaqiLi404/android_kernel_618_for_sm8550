#!/usr/bin/env bash
# Exercise the actual installer on temporary loop-backed images, never a phone.
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use the limited entry point.' >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo 'This loop-device/chroot test requires root.' >&2; exit 2; }
# Android's bundled 32-bit userspace uses small-TID mutex ownership fields.
# A private PID namespace provides Android-like TIDs even on a long-running host.
if [ "${K618_INSTALLER_PID_NS:-}" != 1 ]; then
  exec unshare --mount --pid --fork --mount-proc env K618_INSTALLER_PID_NS=1 bash "$0" "$@"
fi
[ -e /proc/sys/fs/binfmt_misc/register ] || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
TAG=${TAG:-release}
[[ "$TAG" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
dist="$PROJECT_ROOT/dist/$TAG"
testdir="$BUILD_ROOT/installer-check/$TAG"
[ -f "$dist/validation.json" ] || { echo 'Complete the build and validation first.' >&2; exit 2; }
rm -f "$dist/installer-validation.json"
mkdir -p "$testdir" "$PROJECT_ROOT/logs/$TAG"
exec > >(tee "$PROJECT_ROOT/logs/$TAG/installer-test.log") 2>&1
# Keep the host registrations intact. Temporary F-only handlers avoid relying on
# preserve-argv0 auxiliary-vector support in older host kernels, and are removed.
handler_record="$testdir/binfmt-handlers.txt"
cleanup_handlers() { python3 "$PROJECT_ROOT/scripts/binfmt-test.py" remove "$handler_record"; }
trap cleanup_handlers EXIT
echo '5bb397f66063efa349f6fd5cb3b68cd96f29edd0994e4ba5115cf0859a716bf0  assets/qemu-user-static.deb' | (cd "$PROJECT_ROOT" && sha256sum -c -)
qemu_root="$BUILD_ROOT/host-tools/qemu"
mkdir -p "$qemu_root"
dpkg-deb -x "$PROJECT_ROOT/assets/qemu-user-static.deb" "$qemu_root"
python3 "$PROJECT_ROOT/scripts/binfmt-test.py" create "$handler_record" "$qemu_root/usr/bin"
python3 -I "$MKB/unpack_bootimg.py" --boot_img "$K618/boot-test/init_boot.img" --out "$testdir/init-boot" >/dev/null
python3 "$MKB/mkbootimg.py" --header_version 4 --kernel "$K618/out-gki/arch/arm64/boot/Image" \
  --ramdisk "$testdir/init-boot/ramdisk" --cmdline installer.fixture=1 -o "$testdir/base-boot.img"
# The fixture is a partition image, with free space for a replacement kernel/ramdisk.
truncate -s 128M "$testdir/base-boot.img"
unzip -p "$dist/nuwa-kernel-6.18-AnyKernel3.zip" ramdisk-new.cpio.lz4 | lz4 -dcq > "$testdir/expected-ramdisk.cpio"
cmdline=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cmdline"])' "$dist/validation.json")
for scenario in 'horizon _a' 'recovery _b'; do
  read -r mode slot <<< "$scenario"
  AK3_EXPECT_CMDLINE="$cmdline" AK3_EXPECT_RAMDISK="$testdir/expected-ramdisk.cpio" \
    AK3_FLASH_TIMEOUT=180 bash "$PROJECT_ROOT/tools/ak3/flash_test.sh" \
    "$dist/nuwa-kernel-6.18-AnyKernel3.zip" "$testdir/base-boot.img" '' "$mode" "$slot"
done
python3 - "$dist" <<'PY'
import hashlib, json, sys
from pathlib import Path
directory = Path(sys.argv[1])
report = {
    'status': 'passed',
    'scenarios': ['Horizon / slot A', 'recovery / slot B'],
    'checks': ['kernel payload', 'complete ramdisk', 'exact command line', 'header fields', 'inactive slot unchanged'],
    'fixture_partition_bytes': 128 * 1024 * 1024,
    'installer_zip_sha256': hashlib.sha256((directory / 'nuwa-kernel-6.18-AnyKernel3.zip').read_bytes()).hexdigest(),
    'environment': 'isolated PID/mount namespaces, temporary loop images, pinned QEMU 8.2.2',
    'limits': 'Installer emulation; hardware behavior requires device testing.'
}
(directory / 'installer-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
PY
extra=()
[ ! -f "$dist/runtime-validation.json" ] || extra+=(runtime-validation.json)
(cd "$dist" && sha256sum boot.img *.zip kernel.config kernel.symvers validation.json installer-validation.json "${extra[@]}" first-stage-missing.txt depmod.txt > SHA256SUMS)
echo 'INSTALLER EMULATION PASSED (Horizon slot A, recovery slot B)'
