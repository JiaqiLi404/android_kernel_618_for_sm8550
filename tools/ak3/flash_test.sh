#!/bin/bash
# Emulated AnyKernel3 flash.
#
# Runs an AK3 zip's update-binary the way it is run on the phone -- `sh update-binary 3 <outfd>
# <zip>` -- inside a chroot that looks like the phone where it matters. Modes:
#   horizon   Horizon Kernel Flasher (xzr.hkf, Worker.java): booted (a zygote in `ps`), the zip
#             and update-binary copied into the app's files dir, POSTINSTALL exported to that
#             dir (so AKHOME lives there), and its sed that adds mkbootfs applied
#   booted    a generic booted-Android flasher (FKM, KernelFlasher): zip on /sdcard
#   recovery  TWRP: no zygote, so AK3 takes its recovery path
# In all modes:
#   - the shell and core tools are the zip's own ARM busybox (run through qemu-user binfmt),
#   - getprop reports the active slot,
#   - /dev/block/by-name/boot_a and boot_b are block devices (loop devices holding a real
#     boot.img), so AK3's `cat boot-new.img /dev/zero > partition` stops at the end of the
#     partition as it does on the phone.
# Then it checks the result: the zip finished ("Done!"), the active slot's kernel is the
# expected Image, the header and AVB properties were kept, and the other slot is untouched.
#
# usage: sudo tools/ak3/flash_test.sh <ak3.zip> <base-boot.img> [expected-Image] [horizon|booted|recovery] [_a|_b]
#   expected-Image defaults to the Image inside the zip.
#   AK3_FLASH_TIMEOUT (seconds, default 600) bounds the update-binary run; on a hang the processes
#   still running in the chroot are listed (with their kernel stacks) along with the tail of the
#   shell trace, then killed.
#   AK3_EXPECT_CMDLINE="<item> ..." expects the zip to set the command line: each item must be in
#   the flashed header instead of the header's command line being unchanged (the 6.18 zip).
#   AK3_EXPECT_RAMDISK=<raw cpio> expects the zip to replace the whole ramdisk with that one.
# needs: root, losetup, chroot, unzip, and binfmt for 32-bit ARM (qemu-user-static).
set -euo pipefail

[ $# -ge 2 ] || { sed -n '2,20p' "$0"; exit 2; }
for f in "$1" "$2" ${3:+"$3"}; do [ -f "$f" ] || { echo "::error::ak3 flash test: no such file: $f" >&2; exit 2; }; done
zip=$(readlink -f "$1")
boot=$(readlink -f "$2")
image=${3:-}
mode=${4:-horizon}
slot=${5:-_a}
other=_b; [ "$slot" = _b ] && other=_a
case $mode in horizon|booted|recovery) ;; *) echo "mode must be horizon, booted or recovery" >&2; exit 2;; esac

w=$(readlink -f "$(mktemp -d)")
root=$w/root
loops=()
cleanup() {
  umount "$root/proc" 2>/dev/null || true
  for l in "${loops[@]}"; do losetup -d "$l" 2>/dev/null || true; done
  umount "$w" 2>/dev/null || umount -l "$w" 2>/dev/null || true
  rm -rf "$w"
}
trap cleanup EXIT
# Everything -- the chroot, AK3's work dir, the boot images behind the loop devices -- lives on a
# private tmpfs: a run writes ~400 MB, and on a CI runner whose disk is still writing back a kernel
# build those writes crawl under dirty-page throttling. (A tmpfs of our own also allows the device
# nodes the chroot needs, unlike /dev/shm.)
mount -t tmpfs -o size=2g,mode=0755 tmpfs "$w"

fail() { echo "::error::ak3 flash test ($mode, slot $slot): $*" >&2; exit 1; }
say() { echo "ak3 flash test ($mode, slot $slot): $*"; }
# processes whose root is the chroot (whatever session or parent they ended up with)
inchroot() { local p; for p in /proc/[0-9]*; do [ "$(readlink "$p/root" 2>/dev/null)" = "$root" ] && echo "${p#/proc/}"; done; true; }

[ -n "$image" ] && image=$(readlink -f "$image")
if [ -z "$image" ]; then image=$w/expected-Image; unzip -p "$zip" Image > "$image"; fi
[ -s "$image" ] || fail "no expected Image"

mkdir -p "$root"/{system/bin,sbin,tmp,sdcard,proc,dev/block/by-name,data/adb}
unzip -p "$zip" tools/busybox > "$root/system/bin/busybox"
chmod 755 "$root/system/bin/busybox"
timeout 60 chroot "$root" /system/bin/busybox true 2>/dev/null ||
  fail "the zip's ARM busybox does not run here (register qemu-arm binfmt: apt install qemu-user-static)"
timeout 60 chroot "$root" /system/bin/busybox --install -s /system/bin
ln -sf /system/bin/busybox "$root/sbin/sh"
mknod -m 666 "$root/dev/null" c 1 3
mknod -m 666 "$root/dev/zero" c 1 5
mknod -m 444 "$root/dev/random" c 1 8
mknod -m 444 "$root/dev/urandom" c 1 9

# Android's getprop, and the process list AK3 uses to tell a booted phone from recovery
rm -f "$root/system/bin/getprop" "$root/system/bin/ps"
cat > "$root/system/bin/getprop" <<EOF
#!/system/bin/sh
case "\$1" in
  ro.boot.slot_suffix) echo $slot ;;
  ro.build.type) echo user ;;
  "") echo "[ro.boot.slot_suffix]: [$slot]" ;;
esac
EOF
if [ "$mode" != recovery ]; then
  printf '#!/system/bin/sh\necho "USER PID PPID NAME"\necho "root 1 0 init"\necho "root 812 1 zygote64"\n' > "$root/system/bin/ps"
else
  printf '#!/system/bin/sh\necho "PID USER COMMAND"\necho "1 root /init"\necho "402 root /system/bin/recovery"\n' > "$root/system/bin/ps"
fi
chmod 755 "$root/system/bin/getprop" "$root/system/bin/ps"

# A/B boot partitions as real (fixed-size) block devices
for s in _a _b; do
  cp "$boot" "$w/boot$s.img"
  l=$(losetup -f --show "$w/boot$s.img")
  loops+=("$l")
  read -r maj min < <(stat -c '%t %T' "$l")
  mknod "$root/dev/block/by-name/boot$s" b $((16#$maj)) $((16#$min))
done
mkdir -p "$root/dev/block/bootdevice"
ln -s ../by-name "$root/dev/block/bootdevice/by-name"
mount -t proc proc "$root/proc"

envs=(PATH=/system/bin:/sbin HOME=/ TMPDIR=/tmp)
if [ "$mode" = horizon ]; then
  # Worker.java: copy the zip into getFilesDir(), unzip */update-binary there, patch it to copy
  # the app's mkbootfs into $AKHOME/tools, export POSTINSTALL=<files dir>, run sh update-binary 3 1
  files=/data/data/xzr.hkf/files
  mkdir -p "$root$files"
  cp "$zip" "$root$files/kernel.zip"
  (cd "$root$files" && unzip -q kernel.zip '*/update-binary')
  printf '#!/system/bin/sh\n' > "$root$files/mkbootfs"
  sed -i "/\$BB chmod -R 755 tools bin;/i cp -f $files/mkbootfs \$AKHOME/tools;" \
    "$root$files/META-INF/com/google/android/update-binary"
  binary=$files/META-INF/com/google/android/update-binary
  zippath=$files/kernel.zip
  envs+=("POSTINSTALL=$files")
else
  cp "$zip" "$root/sdcard/kernel.zip"
  unzip -p "$zip" META-INF/com/google/android/update-binary > "$root/tmp/update-binary"
  binary=/tmp/update-binary
  zippath=/sdcard/kernel.zip
fi
limit=${AK3_FLASH_TIMEOUT:-600}
say "running update-binary (limit ${limit}s)"
start=$SECONDS
set +e
# sh -x: the trace goes to err.log, so a failure or hang shows the last commands run
chroot "$root" /system/bin/env -i "${envs[@]}" \
  /system/bin/sh -x "$binary" 3 1 "$zippath" > "$w/out.log" 2> "$w/err.log" &
pid=$!
while kill -0 $pid 2>/dev/null && [ $((SECONDS - start)) -lt "$limit" ]; do sleep 1; done
hung=0
if kill -0 $pid 2>/dev/null; then
  hung=1
  echo "--- still running after ${limit}s: processes in the chroot ---"
  for p in $(inchroot); do
    echo "== pid $p ($(cat "/proc/$p/wchan" 2>/dev/null)): $(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)"
    head -n 8 "/proc/$p/stack" 2>/dev/null | sed 's/^/     /'
  done
  for p in $(inchroot); do kill -KILL "$p" 2>/dev/null; done
  kill -KILL $pid 2>/dev/null
fi
wait $pid
rc=$?
set -e
sed -n 's/^ui_print \{0,1\}//p' "$w/out.log" | sed '/^\s*$/d' | sed 's/^/  | /'
say "update-binary exited $rc after $((SECONDS - start))s"
if [ $hung -eq 1 ] || [ $rc -ne 0 ] || ! grep -q 'Done!' "$w/out.log"; then
  echo "--- stderr (shell trace), last 60 lines ---"; tail -n 60 "$w/err.log"
  [ $hung -eq 1 ] && fail "update-binary hung (killed after ${limit}s)"
  fail "update-binary failed (exit $rc)"
fi
grep -q 'Aborting' "$w/out.log" && fail "aborted"
for l in "${loops[@]}"; do blockdev --flushbufs "$l"; done

# the active slot now carries the expected kernel, with the base boot's header and AVB props
unzip -p "$zip" tools/magiskboot > "$w/magiskboot"
chmod 755 "$w/magiskboot"
mkdir -p "$w/new" "$w/base"
(cd "$w/new" && timeout 300 "$w/magiskboot" unpack -h "$w/boot$slot.img" > hdr.txt 2>&1) || fail "flashed slot does not unpack"
(cd "$w/base" && timeout 300 "$w/magiskboot" unpack -h "$boot" > hdr.txt 2>&1) || fail "base boot does not unpack"
cmp -s "$w/new/kernel" "$image" || fail "boot$slot kernel is not the expected Image"
# AK3_EXPECT_RAMDISK=<raw cpio>: this zip replaces the whole ramdisk (the 6.18 zip ships the test
# image's), so what the flashed slot carries must be that cpio -- magiskboot unpacked it above
if [ -n "${AK3_EXPECT_RAMDISK:-}" ]; then
  [ -s "$w/new/ramdisk.cpio" ] || fail "boot$slot has no ramdisk"
  cmp -s "$w/new/ramdisk.cpio" "$AK3_EXPECT_RAMDISK" ||
    fail "boot$slot ramdisk is not the expected one ($(stat -c %s "$w/new/ramdisk.cpio") vs $(stat -c %s "$AK3_EXPECT_RAMDISK") bytes)"
  echo "ramdisk: $(stat -c %s "$w/new/ramdisk.cpio") bytes, as expected"
fi
for k in HEADER_VER PAGESIZE CMDLINE OS_VERSION OS_PATCH_LEVEL; do
  a=$(grep "^$k" "$w/base/hdr.txt" || true); b=$(grep "^$k" "$w/new/hdr.txt" || true)
  # AK3_EXPECT_CMDLINE="<items>": this zip is meant to set the command line (the 6.18 test kernel
  # needs items the phone's boot header has none of), so every item is looked for instead
  if [ "$k" = CMDLINE ] && [ -n "${AK3_EXPECT_CMDLINE:-}" ]; then
    actual_cmdline=${b#*\[}
    actual_cmdline=${actual_cmdline%\]}
    [ "$actual_cmdline" = "$AK3_EXPECT_CMDLINE" ] ||
      fail "command line differs: [$actual_cmdline]"
    echo "command line now: [$actual_cmdline]"
    continue
  fi
  [ "$a" = "$b" ] || fail "header $k changed: [$a] -> [$b]"
done
# AVB hash-footer properties (key and value are adjacent NUL-terminated strings)
avbprop() { strings -n 2 "$1" | grep -A1 -x "com.android.build.boot.$2" | tr '\n' ' '; }
if [ -n "$(avbprop "$boot" os_version)" ]; then
  for p in os_version security_patch; do
    a=$(avbprop "$boot" $p); b=$(avbprop "$w/boot$slot.img" $p)
    [ "$a" = "$b" ] || fail "AVB prop $p changed: [$a] -> [$b]"
  done
fi
cmp -s "$w/boot$other.img" "$boot" || fail "the other slot (boot$other) was modified"
echo "ak3 flash test ($mode, slot $slot): OK -- boot$slot got the new kernel, boot$other untouched"
