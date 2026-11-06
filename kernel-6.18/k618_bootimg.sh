#!/bin/bash
# k618_bootimg.sh [init_boot.img]: a boot image to *try* the 6.12 kernel on the 13 Pro with
# `fastboot boot` (it flashes nothing; the next normal boot is the flashed 5.15 system again).
#
#   kernel   our GKI 6.12 Image ($K618/out-gki)
#   ramdisk  the generic ramdisk of the given init_boot.img (default $K618/boot-test/init_boot.img:
#            the ROM's own, unpatched -- not the phone's, which carries KernelSU's 5.15 LKM), with a
#            second cpio archive after it: /lib/modules with our 6.12 modules (soc-repo's build in
#            $K618/soc and the GKI modules named by GKIMODS, by default zram and zsmalloc -- on a
#            real device those live in system_dlkm), debug info stripped, and modules.load / modules.dep /
#            modules.alias / modules.softdep for them. The bootloader puts the vendor ramdisk first
#            and this one after it, so these files win over vendor_boot's 5.15 ones. modules.load is
#            the phone's own first-stage order ($STOCK), less what we do not have.
#
# Output in $K618/boot-test: boot-618.img, the module list and what the first stage misses.
# Needs AOSP's mkbootimg (tools/mkbootimg) in $MKB.
. "$(dirname "$(readlink -f "$0")")/env.sh"
OUT=$K618/boot-test
STOCK=${STOCK:-$NB/stockmods/vendor_boot/ramdisk0/lib/modules}
MKB=${MKB:-$NB/home/mkbootimg}
IB=${1:-$OUT/init_boot.img}
REL=$(cat $O/include/config/kernel.release)
T=$OUT/stage
[ -f "$IB" ] || { echo "no init_boot image: $IB"; exit 1; }
# KERNEL=<Image>: another kernel than out-gki's (k618_irbuild.sh's, which carries our ramdisk layer
# itself); REUSE_STAGE=1: only make the boot image again, from the stage already there
mkimg() {
	# (AOSP's mkbootimg imports its own gki package from next to it: not isolated); CMDLINE: the boot
	# image's own command line (the bootloader adds it to vendor_boot's)
	python3 $MKB/mkbootimg.py --header_version 4 --kernel ${KERNEL:-$O/arch/arm64/boot/Image} \
		--ramdisk $T/overlay.lz4 --cmdline "${CMDLINE:-}" -o $OUT/boot-618.img || exit 1
}
if [ -n "$REUSE_STAGE" ]; then
	[ -f $T/overlay.lz4 ] || { echo "no stage to reuse"; exit 1; }
	rm -f $OUT/boot-618.img
	mkimg
	ls -la $OUT/boot-618.img; sha256sum $OUT/boot-618.img | cut -c1-16
	exit 0
fi
# a failed run leaves no image behind that could be taken for this one
rm -rf $T $OUT/boot-618.img $OUT/marks.txt && mkdir -p $T/mods/lib/modules/$REL/kernel $T/rd/lib/modules $OUT || exit 1

# test helper modules, built against our GKI build:
# MARKS=<n>: k618_mark copies (each waits 5 s) after every n modules of the first stage, in the order
# they load -- the time until a failed boot resets then says where it failed. MARKS_AT="<n>:<ms>,..."
# instead: a mark after the next n modules waiting ms, item by item (the rest without marks; n may be
# 0: a wait before any module)
# PANICSIG=1: k618_panicsig, loaded first: a panic's kind as a wait before the reset
# EXTRA="<file.c> ...": more modules built the same way (e.g. a test module for QEMU only)
MK=
[ -n "$MARKS_AT" ] && MARKS=${MARKS:-1}
if [ -n "$MARKS$PANICSIG$EXTRA" ]; then
	MD=$K618/mark; rm -rf $MD; mkdir -p $MD; : > $MD/Kbuild
	if [ -n "$MARKS" ]; then
		for i in $(seq -w 1 20); do cp $K618_DIR/k618_mark.c $MD/k618_mark$i.c; echo "obj-m += k618_mark$i.o" >> $MD/Kbuild; done
	fi
	if [ -n "$PANICSIG" ]; then
		cp $K618_DIR/k618_panicsig.c $MD/ && echo "obj-m += k618_panicsig.o" >> $MD/Kbuild || exit 1
		FIRST="k618_panicsig $FIRST"
	fi
	for f in $EXTRA; do
		# "<file.c>=<name>": the module is called <name> (k618_perf_helper.c=perf_helper: the
		# ROM's paths and SELinux labels name /sys/module/perf_helper)
		n=; case "$f" in *=*) n=${f#*=}; f=${f%%=*};; esac
		b=$(basename ${f%.c})
		if [ -n "$n" ] && [ "$n" != "$b" ]; then
			cp $f $MD/ && printf 'obj-m += %s.o\n%s-y := %s.o\n' $n $n $b >> $MD/Kbuild || exit 1
		else
			cp $f $MD/ && echo "obj-m += $b.o" >> $MD/Kbuild || exit 1
		fi
		# a module with a header of its own (k618_mi_thermal.h) wants it next to the .c
		if [ -f "${f%.c}.h" ]; then cp "${f%.c}.h" $MD/ || exit 1; fi
	done
	# a module that calls into the soc tree (k618_mi_thermal -> panel_event_notifier) needs its
	# symbol versions: without them modpost fails on the undefined symbols, and a module that got
	# through anyway would load with "no symbol version for ..." and no CRC check at all
	XS=; [ -f $S/Module.symvers ] && XS="KBUILD_EXTRA_SYMBOLS=$S/Module.symvers"
	PATH=$CLANG:${R:+$R/bin:}$TOOLS:/usr/bin:/bin KBUILD_GENDWARFKSYMS_STABLE=1 \
		make -C $W/common O=$O ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$MD $XS -j4 modules > $MD/build.log 2>&1 || { tail $MD/build.log; exit 1; }
	MK=$(ls $MD/*.ko)
fi
# BATCH=1 (with STAGE2): the second-stage modules are not put in init's list at all. They go into
# /lib/modules/k618_batch.load, and k618_load (in init's list instead of them) runs /k618_umh once
# per module and waits for it -- so a module whose init() faults kills that child and the batch goes
# on, where under init the fault is pid 1's and the boot ends. The loader is a 1 KB static program
# with no libc, since the first-stage ramdisk has neither a shell nor a library.
if [ -n "${BATCH:-}" ]; then
	$CLANG/clang --target=aarch64-linux-gnu -O2 -nostdlib -static -ffreestanding \
		-fno-stack-protector -fuse-ld=lld -Wl,--build-id=none -s \
		-o $T/rd/k618_umh $K618_DIR/k618_umh.c || { echo "k618_umh: build failed"; exit 1; }
	chmod 755 $T/rd/k618_umh
	echo "k618_umh: $(stat -c %s $T/rd/k618_umh) bytes"
fi
# the modules: ours, and GKI's own that ours need (GKIMODS="<name> ...", default zram zsmalloc;
# the second stage's cfg80211 needs rfkill, mac80211 libarc4)
GK=
for g in ${GKIMODS:-zram zsmalloc}; do
	f=$(find $O -name "$g.ko" | head -1)
	[ -n "$f" ] || { echo "GKIMODS: no $g.ko in $O"; exit 1; }
	GK="$GK $f"
done
# the techpack modules k618_tp.sh collected, when they are there (TPMODS=0 leaves them out, and
# TPMODS="<name> ..." takes only those -- with the ramdisk inside the kernel every module in it is
# a module in the kernel image, and all 80 of them make the boot image 19 MB bigger)
TK=
if [ "${TPMODS:-1}" = 1 ]; then
	TK=$(ls $K618/tpmods/*.ko 2>/dev/null)
elif [ "${TPMODS:-1}" != 0 ]; then
	for t in $TPMODS; do
		f=$K618/tpmods/$t.ko
		[ -f "$f" ] || { echo "TPMODS: no $t.ko in $K618/tpmods"; exit 1; }
		TK="$TK $f"
	done
fi
# MADRIDMODS="<file.ko> ...": prebuilt modules from the 18 Pro Max's firmware (k618_timg.sh's MADRID)
for f in $(find $S -name "*.ko" -newer $S/Kbuild) $GK $TK $MK ${MADRIDMODS:-}; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
	$CLANG/llvm-strip --strip-debug -o $T/mods/lib/modules/$REL/kernel/$(basename $f) $f || exit 1
done
depmod -b $T/mods -F $O/System.map -e $REL 2> $OUT/depmod.err
grep -v "could not open modules.\(builtin\|order\)" $OUT/depmod.err | head -20
M=$T/mods/lib/modules/$REL
cp $M/kernel/*.ko $T/rd/lib/modules/
sed -e 's|kernel/|/lib/modules/|g' $M/modules.dep > $T/rd/lib/modules/modules.dep
cp $M/modules.alias $M/modules.softdep $T/rd/lib/modules/
cp $STOCK/modules.blocklist $T/rd/lib/modules/
python3 -I - $STOCK/modules.load $T/rd/lib/modules $OUT "${MARKS:-0}" "${MARKS_AT:-}" "${SKIP:-}" "${FIRST:-}" "${OPTS:-}" "${LAST:-}" "${STAGE2:-}" "${BATCH:-}" "${BATCH_ONLY:-}" "${RENAME-ucsi_glink=ucsi_qti_glink qca_cld3_kiwi_v2=kiwi_v2}" <<'PY' || exit 1
import os, sys
stock, rd, out, marks, marks_at = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
batch = len(sys.argv) > 11 and sys.argv[11] != ""
batch_only = [s.replace("-", "_").removesuffix(".ko") for s in (sys.argv[12] if len(sys.argv) > 12 else "").split()]
rename = dict(p.replace("-", "_").split("=", 1) for p in (sys.argv[13] if len(sys.argv) > 13 else "").split())
skip = {s.replace("-", "_").removesuffix(".ko") for s in sys.argv[6].split()}
first = [s.replace("-", "_").removesuffix(".ko") for s in sys.argv[7].split()]
opts = [f"options {o.strip()}" for o in sys.argv[8].split(";") if o.strip()]
last = [s.replace("-", "_").removesuffix(".ko") for s in sys.argv[9].split()]
have = {f[:-3].replace("-", "_"): f for f in os.listdir(rd) if f.endswith(".ko") and not f.startswith("k618_mark")}
stage2 = sys.argv[10] if len(sys.argv) > 10 else ""
load, miss = [], []
for line in open(stock):
    n = line.strip()
    if not n:
        continue
    k = n[:-3].replace("-", "_")
    k = rename.get(k, k)
    if k in skip:
        print(f"left out: {n}")
        continue
    if k in batch_only:
        continue
    (load if k in have else miss).append(have.get(k, n))
# STAGE2: the phone's second-stage order after the first stage's, every module of it we have. The
# ones we do not are not reported again -- which they are and why is k618_modgap.sh's business.
if stage2:
    n2, in_batch = 0, []
    for line in open(stage2):
        n = line.strip()
        if not n.endswith(".ko"):
            continue
        k = os.path.basename(n)[:-3].replace("-", "_")
        k = rename.get(k, k)
        if k in skip or k not in have or have[k] in load or have[k] in in_batch:
            continue
        # BATCH: k618_load's list, not init's (k618_insmod and k618_load themselves stay in init's,
        # and so does anything named in LAST -- those are meant to be tried by init, late)
        if batch and k not in ("k618_insmod", "k618_load") and k not in last:
            in_batch.append(have[k])
            n2 += 1
            continue
        load.append(have[k])
        n2 += 1
    # BATCH_ONLY: the batch's end, whatever the phone's lists say (the ordering below puts each one
    # after what it needs)
    for k in batch_only:
        if batch and k in have and have[k] not in load and have[k] not in in_batch:
            in_batch.append(have[k])
            n2 += 1
    if batch:


        deps = {}
        for line in open(f"{rd}/modules.dep"):
            m, _, d = line.strip().partition(":")
            deps[os.path.basename(m)] = [os.path.basename(x) for x in d.split()]


        in_init, todo = set(), list(load)
        while todo:
            m = todo.pop()
            if m not in in_init:
                in_init.add(m)
                todo += deps.get(m, [])
        in_batch = [m for m in in_batch if m not in in_init]
        ordered, placed = [], set()

        def place(m, path=()):
            if m in placed or m in in_init or m in path:
                return
            for d in deps.get(m, []):
                place(d, path + (m,))
            placed.add(m)
            ordered.append(m)
        for m in in_batch:
            place(m)
        pulled = [m for m in ordered if m not in in_batch]
        moved = sum(1 for i, m in enumerate([m for m in ordered if m in in_batch]) if m != in_batch[i])
        in_batch = ordered
        open(f"{rd}/k618_batch.load", "w").write("".join(m + "\n" for m in in_batch))
        print(f"batch: {len(in_batch)} modules for k618_load (not in init's list), in dependency order: "
              f"{moved} moved, {len(pulled)} pulled in{(' (' + ' '.join(pulled) + ')') if pulled else ''}")
    print(f"second stage: {n2} modules after the first stage's")
for f in first:
    if f not in have:
        sys.exit(f"FIRST: no {f}.ko in the ramdisk")
for f in last:
    if f not in have:
        sys.exit(f"LAST: no {f}.ko in the ramdisk")
pre = [have[f] for f in first]
post = [have[f] for f in last]
load = pre + [m for m in load if m not in pre and m not in post]
if marks:
    # the order modprobe loads them in (dependencies first), a mark after every `marks` of them
    deps = {}
    for line in open(f"{rd}/modules.dep"):
        m, _, d = line.strip().partition(":")
        deps[os.path.basename(m)] = [os.path.basename(x) for x in d.split()]
    order = []

    def add(m):
        if m in order:
            return
        for d in deps.get(m, []):
            add(d)
        order.append(m)
    for m in pre:
        add(m)
    nfirst = len(order)
    for m in load:
        add(m)
    if marks_at:
        plan = [(nfirst if c == "F" else int(c), int(ms)) for c, ms in (item.split(":") for item in marks_at.split(","))]
    else:
        plan = [(marks, 5000)] * ((len(order) + marks - 1) // marks)
    if len(plan) > 20:
        sys.exit(f"{len(plan)} marks, but only 20 copies of k618_mark are built")
    full, groups, n, total = [], [], 0, 0
    for i, (count, ms) in enumerate(plan, 1):
        total += ms
        full += order[n:n + count] + [f"k618_mark{i:02d}.ko"]
        groups.append(f"before mark {i:02d} (waits {ms / 1000:g} s, {total / 1000:g} s in all): " + " ".join(order[n:n + count]))
        opts.append(f"options k618_mark{i:02d} ms={ms}")
        n += count
    full += order[n:]
    if order[n:]:
        groups.append("after the last mark: " + " ".join(order[n:]))
    if post:
        groups.append("last: " + " ".join(post))
    load = full
    open(f"{out}/marks.txt", "w").write("".join(g + "\n" for g in groups))
    print(f"marks: {len(plan)} in {len(order)} modules (in {out}/marks.txt)")
# the LAST modules (what they need is loaded by then, or modprobe loads it with them)
load += [m for m in post if m not in load]
open(f"{rd}/modules.options", "w").write("".join(o + "\n" for o in opts))
open(f"{rd}/modules.load", "w").write("".join(f + "\n" for f in load))
open(f"{rd}/modules.load.recovery", "w").write("".join(f + "\n" for f in load))
# charger mode (androidboot.mode=charger) reads this one when it is there: this phone's vendor
# ramdisk has none, but another's may, and a 5.15 list in it would end first-stage init
open(f"{rd}/modules.load.charger", "w").write("".join(f + "\n" for f in load))
open(f"{out}/first-stage-missing.txt", "w").write("".join(f + "\n" for f in miss))
print(f"first stage: {len(load)} modules of the phone's {len(load) + len(miss)}; missing: {' '.join(miss)}")
PY
# Our layer (overlay.cpio): /lib/modules and the batch loader, nothing else. k618_irbuild.sh builds
# it into the kernel, which unpacks it AFTER the ramdisk the bootloader hands over (k618_ports.py
# initramfs_last), and it is the boot image's own ramdisk too (one LZ4 legacy stream, as Android's
# are). So the phone's own vendor ramdisk and init_boot -- or TWRP's, in recovery; another model's
# on another phone -- are what runs, and only the same-named files under /lib/modules are ours.
# ramdisk.lz4 is for QEMU, which has no init_boot: the generic ramdisk, then our layer
# (k618_qemu.sh puts the vendor ramdisk in front).
(cd $T/rd && find lib ${BATCH:+k618_umh} | sort | cpio -o -H newc -R 0:0 --quiet) > $T/overlay.cpio || exit 1
lz4 -l -12 -cq $T/overlay.cpio > $T/overlay.lz4 || exit 1
python3 -I $MKB/unpack_bootimg.py --boot_img $IB --out $T/ib > /dev/null || exit 1
lz4 -dcq $T/ib/ramdisk > $T/ramdisk.cpio || exit 1
cat $T/overlay.cpio >> $T/ramdisk.cpio || exit 1
lz4 -l -12 -cq $T/ramdisk.cpio > $T/ramdisk.lz4 || exit 1
mkimg
ls $T/rd/lib/modules/*.ko | xargs -n1 basename > $OUT/modules.txt
echo "kernel $REL, $(wc -l < $OUT/modules.txt) modules, our ramdisk layer $(stat -c %s $T/overlay.lz4) bytes"
ls -la $OUT/boot-618.img; sha256sum $OUT/boot-618.img | cut -c1-16
