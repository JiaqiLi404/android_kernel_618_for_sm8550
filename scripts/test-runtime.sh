#!/usr/bin/env bash
# QEMU-only checks: configuration, tmpfs xattrs, zram recompression and slab teardown.
set -euo pipefail
[ "${K618_LIMITS_VERIFIED:-}" = 1 ] || { echo 'Use the limited entry point.' >&2; exit 2; }
. "$(dirname -- "${BASH_SOURCE[0]}")/project-env.sh"
. "$PROJECT_ROOT/kernel-6.18/env.sh"
TAG=${TAG:-release}
[[ "$TAG" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
for command in qemu-system-aarch64 cpio gzip; do
    command -v "$command" >/dev/null || { echo "Missing $command; see docs/VALIDATION.en.md"; exit 2; }
done
dist="$PROJECT_ROOT/dist/$TAG"
[ -f "$dist/validation.json" ] || { echo 'Build and verify first.'; exit 2; }
rm -f "$dist/runtime-validation.json"
fixture=$(mktemp -d "$BUILD_ROOT/runtime-check-$TAG.XXXXXX")
mkdir -p "$fixture/module" "$fixture/rd/bin" "$fixture/rd/mods" "$PROJECT_ROOT/logs/$TAG"
cp "$PROJECT_ROOT/kernel-6.18/k618_zramtest.c" "$fixture/module/"
printf 'obj-m += k618_zramtest.o\n' > "$fixture/module/Kbuild"
export PATH="$CLANG:${R:+$R/bin:}$W/prebuilts/clang-tools/linux-x86/bin:$TOOLS:$W/prebuilts/build-tools/linux-x86/bin:/usr/bin:/bin"
export KBUILD_GENDWARFKSYMS_STABLE=1 KBUILD_BUILD_HOST=build-host KBUILD_BUILD_USER=build-user
make -C "$W/common" O="$O" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
    PAHOLE="$TOOLS/pahole" M="$fixture/module" -j4 modules > "$fixture/module-build.log" 2>&1 || {
    tail -n 25 "$fixture/module-build.log"; exit 1;
}
cp "$AK3_SOURCE/tools/busybox" "$fixture/rd/bin/busybox"
for name in zram zsmalloc; do
    path=$(find "$O" -name "$name.ko" -print -quit)
    [ -n "$path" ] || { echo "Missing $name module"; exit 1; }
    cp "$path" "$fixture/rd/mods/"
done
cp "$fixture/module/k618_zramtest.ko" "$fixture/rd/mods/"
cat > "$fixture/rd/init" <<'GUEST'
#!/bin/busybox sh
bb() { /bin/busybox "$@"; }
shutdown() { echo o > /proc/sysrq-trigger; while :; do bb sleep 3600; done; }
fail() { echo "RUNTIME_CHECK: FAIL $*"; shutdown; }
bb mkdir -p /proc /sys /dev /tmp
bb mount -t proc proc /proc || fail proc
bb mount -t sysfs sysfs /sys || fail sysfs
bb mount -t tmpfs tmpfs /dev || fail dev-tmpfs
bb mknod /dev/null c 1 3 || fail dev-null
bb mknod /dev/console c 5 1 || fail dev-console
bb mount -t tmpfs tmpfs /tmp || fail tmpfs
bb touch /tmp/xattr
bb setfattr -n user.k618 -v check /tmp/xattr || fail xattr-set
bb setfattr -x user.k618 /tmp/xattr || fail xattr-remove
case "$(bb cat /sys/module/kfence/parameters/deferrable)" in Y|1) ;; *) fail kfence;; esac
case "$(bb cat /sys/module/rcutree/parameters/enable_rcu_lazy)" in Y|1) ;; *) fail lazy-rcu;; esac
bb mkdir -p /sys/kernel/tracing
bb mount -t tracefs tracefs /sys/kernel/tracing || fail tracefs
trace=$(bb cat /sys/kernel/tracing/buffer_size_kb)
case "$trace" in 8|*'expanded: 8)'*) ;; *) fail "trace-buffer $trace";; esac
echo "RUNTIME_CHECK: config and tmpfs xattrs PASS; trace=$trace"
cc=$(bb cat /proc/sys/net/ipv4/tcp_available_congestion_control)
for algorithm in cubic bbr; do
    echo " $cc " | bb grep -q " $algorithm " || fail "tcp-missing-$algorithm"
done
[ "$(bb cat /proc/sys/net/ipv4/tcp_congestion_control)" = cubic ] || fail tcp-default
echo "RUNTIME_CHECK: TCP PASS; available=$cc; default=cubic"
bb insmod /mods/zsmalloc.ko || fail zsmalloc-load
bb insmod /mods/zram.ko recomp_idle_secs=0 || fail zram-load
bb insmod /mods/k618_zramtest.ko || fail test-load
n=0
while [ "$n" -lt 60 ]; do
    bb dmesg | bb grep -q 'k618_zramtest: FAIL' && fail zram-data
    bb dmesg | bb grep -q 'k618_zramtest: PASS' && break
    n=$((n+1)); bb sleep 1
done
[ "$n" -lt 60 ] || fail zram-timeout
size=$(bb cat /sys/kernel/slab/zram_pp_slot/object_size) || fail slab-missing
echo "RUNTIME_CHECK: candidate_object_size=$size"
bb rmmod zram || fail zram-unload
[ ! -e /sys/kernel/slab/zram_pp_slot ] || fail slab-leak
echo 'RUNTIME_CHECK: PASS'
shutdown
GUEST
chmod 755 "$fixture/rd/init" "$fixture/rd/bin/busybox"
(cd "$fixture/rd" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1 > "$fixture/initrd.gz")
log="$PROJECT_ROOT/logs/$TAG/runtime-test.log"
set +e
timeout 120 qemu-system-aarch64 -M virt -cpu max,pauth-impdef=on -smp 4 -m 2048 \
    -nic none -nographic -no-reboot -kernel "$O/arch/arm64/boot/Image" \
    -initrd "$fixture/initrd.gz" \
    -append 'console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 trace_buf_size=8K rcu_nocbs=all rcutree.enable_rcu_lazy=1' \
    > "$log" 2>&1
result=$?
set -e
python3 - "$log" "$dist" "$result" <<'PY'
import json,re,sys
from pathlib import Path
log=Path(sys.argv[1]).read_text(errors='replace')
passed=(int(sys.argv[3])==0 and 'RUNTIME_CHECK: PASS' in log and
        'RUNTIME_CHECK: TCP PASS' in log and
        'k618_zramtest: PASS' in log and 'RUNTIME_CHECK: FAIL' not in log and
        not re.search(r'BUG:|Oops:|WARNING:|Kernel panic',log))
for line in log.splitlines():
    if 'RUNTIME_CHECK:' in line or 'k618_zramtest:' in line: print(line)
if not passed:
    print('\n'.join(log.splitlines()[-30:])); raise SystemExit('RUNTIME CHECK FAILED')
size=int(re.search(r'candidate_object_size=(\d+)',log)[1])
report={'status':'passed','environment':'QEMU virt, four CPUs, 2 GiB guest RAM; no physical device',
        'kernel_image':'base GKI with the same configured features and exports as the packaged kernel',
        'checks':['tmpfs xattr storage/removal','deferrable KFENCE enabled','lazy RCU enabled',
                  '8 KiB initial trace setting','4 MiB zram data after two recompression rounds',
                  'dedicated candidate cache exists','zram unload destroys candidate cache',
                  'BBR and CUBIC available; default CUBIC'],
        'candidate_object_bytes':size,'data_pages_verified':1024}
(Path(sys.argv[2])/'runtime-validation.json').write_text(json.dumps(report,indent=2)+'\n')
print('RUNTIME VALIDATION PASSED')
PY
