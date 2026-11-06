#!/bin/bash


set -eu
D=$(dirname "$(readlink -f "$0")")
. "$D/env.sh"


if [ "${GUARD:-0}" = 1 ]; then
	PO="k618_probe probe_ms=${PROBE_MS:-60000} deny_write_open=1 allow_vols=${ALLOW_VOLS:-persist}"
else
	PO="k618_probe probe_ms=${PROBE_MS:-60000}"
fi
PO="$PO deny_panic_ms=0 deny_wait_ms=0"


[ -n "${WATCH_COMM:-}" ] && PO="$PO watch_comm=$WATCH_COMM${WATCH_MAX:+ watch_max=$WATCH_MAX}"
# WATCH_BLOCK=<part of a path>: log both ends of its ioctls, so one that never returns shows up
[ -n "${WATCH_BLOCK:-}" ] && PO="$PO watch_block=$WATCH_BLOCK"
# WATCH_SKIP=<ioctl request>: count that one instead of logging it (the DRM property flood)
[ -n "${WATCH_SKIP:-}" ] && PO="$PO watch_skip=$WATCH_SKIP"
[ -n "${WATCH_LOG:-}" ] && PO="$PO watch_log=1${WATCH_LOG_TAG:+ watch_log_tag=$WATCH_LOG_TAG}${WATCH_LOG_MAX:+ watch_log_max=$WATCH_LOG_MAX}${WATCH_LOG_COMM:+ watch_log_comm=$WATCH_LOG_COMM}"


[ -n "${SUSPEND_VETO:-}" ] && PO="$PO suspend_veto=1"


[ "${WALT_COMPAT:-1}" = 1 ] && PO="$PO walt_compat=1"
# NV_VOLS=modemst1,modemst2,fsc,fsg: the modem's own file system, writable by rmt_storage only
# (NV_COMM to name another program). Off by default; set it only after those partitions are backed up.
[ -n "${NV_VOLS:-}" ] && PO="$PO nv_vols=$NV_VOLS${NV_COMM:+ nv_comm=$NV_COMM}"


[ -n "${RAW_DISKS:-}" ] && PO="$PO raw_disks=$RAW_DISKS${RAW_COMM:+ raw_comm=$RAW_COMM}"
# STACK_COMM=<process>,... at STACK_AT_MS: where a process that never finishes is waiting
[ -n "${STACK_COMM:-}" ] && PO="$PO stack_comm=$STACK_COMM stack_at_ms=${STACK_AT_MS:-13000}${STACK_MAX:+ stack_max=$STACK_MAX}"
# DUMP_AT_MS=<ms>,... (up to four): the display as the kernel sees it at those times. DUMP_DRM=<the
# display's platform device> prints the DRM device's atomic state (planes, CRTC, connector -- what
# debugfs dri/0/state shows), DUMP_FILES=<path>,... reads files into the log (the backlight's level)
[ -n "${DUMP_AT_MS:-}" ] && PO="$PO dump_at_ms=$DUMP_AT_MS${DUMP_DRM:+ dump_drm=$DUMP_DRM}${DUMP_FILES:+ dump_files=$DUMP_FILES}"


[ -n "${RET_WATCH:-}" ] && PO="$PO ret_watch=$RET_WATCH${RET_WATCH_AT_MS:+ ret_watch_at_ms=$RET_WATCH_AT_MS}${RET_WATCH_MAX:+ ret_watch_max=$RET_WATCH_MAX}${DRM_DBG_MASK:+ drm_dbg_mask=$DRM_DBG_MASK}${DRM_DBG_FRAMES:+ drm_dbg_frames=$DRM_DBG_FRAMES}${DRM_DBG_WINDOWS:+ drm_dbg_windows=$DRM_DBG_WINDOWS}"


PO="$PO log_part=rawdump log_from=metadata log_start_ms=${LOG_START_MS:-300} log_poll_ms=${LOG_POLL_MS:-20}"
PO="$PO log_every_ms=${LOG_EVERY_MS:-2000}"
PO="$PO log_slot_kb=4096 log_buf_kb=4096"


PO="$PO log_ring_kb=${LOG_RING_KB:-131072} log_ring_ms=${LOG_RING_MS:-20} log_ring_keep_kb=${LOG_RING_KEEP_KB:-32768}"
PO="$PO log_ring_skip=${LOG_RING_SKIP:-xiaomi_touch_dev_open,set_src_clk_h8_,qesdsys}"
PO="$PO log_ring_batch=${LOG_RING_BATCH:-24} log_stall_ms=${LOG_STALL_MS:-5000}"


PS="${PANICSIG_OPTS:-mode=0 kind_ms=0 context_ms=0 restart_ms=0}"
PO="$PO;k618_panicsig $PS"


CL="${CMDLINE:-log_buf_len=16M sysctl.kernel.panic_on_oops=0 printk.devkmsg=on sysctl.kernel.printk=1 panic=5}"
[ "${ICE:-1}" = 0 ] && CL="$CL qcom_ice.k618_ufs_ice=0"


[ "${SERIAL_CONSOLE:-0}" = 0 ] && CL="$CL initcall_blacklist=qcom_geni_serial_init"


CL="$CL msm_drm.k618_cont_splash=${CONT_SPLASH:-0}"
# ...and nothing sets this OLED panel's brightness without Xiaomi's own display extension, so the
# driver sets a fixed level itself when the panel comes up (0 = leave it alone)
CL="$CL msm_drm.k618_bl_level=${BL_LEVEL:-1200}"


[ "${KSU:-1}" = 1 ] && CL="$CL kernelsu.allow_shell=1"
[ "${KSU:-1}" = 1 ] && [ "${KSUD:-1}" != 1 ] && CL="$CL kernelsu.noksud=1"
# RT tasks' uclamp floor 0, as the vendor's post-boot script sets it: the kernel's default 1024 lets
# only cpu7 "fit" an RT task and holds its cluster at the top frequency
CL="$CL sysctl.kernel.sched_util_clamp_min_rt_default=0"


CL="$CL android_arch_task_struct_size=1024 sched_proxy_exec=0 erofs.reserved_pages=64 restrict_cma_redirect=false"
CL="$CL trace_buf_size=8K rcu_nocbs=all rcutree.enable_rcu_lazy=1"


[ -n "${STALL:-}" ] && CL="$CL sysctl.kernel.hung_task_timeout_secs=8 sysctl.kernel.hung_task_all_cpu_backtrace=1 watchdog_thresh=4 softlockup_all_cpu_backtrace=1 hardlockup_all_cpu_backtrace=1 workqueue.watchdog_thresh=8"


S2=; FI=; LA=k618_probe; EX="$D/k618_probe.c"; BA=


MI=
[ -n "${MIDISP:-}" ] && { EX="$EX $D/k618_midisp.c"; MI=k618_midisp; }
# SUSPEND_TEST=1 (QEMU only, never for the phone): a module that asks for a suspend 1 s after it
# loads, to see the watch fire and pm_suspend() come back refused on a boot with no user space
[ -n "${SUSPEND_TEST:-}" ] && { EX="$EX $D/k618_suspendtest.c"; MI="$MI k618_suspendtest"; }
# ZRAM_TEST=1 (QEMU only, never for the phone): writes 16 MB through zram0, has two rounds of
# k618_ports.py's cold tier run and reads every page back (k618_zramtest.c)
[ -n "${ZRAM_TEST:-}" ] && { EX="$EX $D/k618_zramtest.c"; MI="$MI k618_zramtest"; }
if [ "${STAGE2:-0}" != 0 ]; then
	# STAGE2=1 is the phone's whole second-stage order; STAGE2=<file> a list of our own (one .ko
	# name per line), for trying a subsystem at a time -- which is the safer way round, because
	# first-stage init treats a single module it cannot insmod as fatal
	S2=$K618/modgap/rom_modules.load
	[ "${STAGE2}" != 1 ] && S2=$STAGE2
	[ -f "$S2" ] || { echo "no $S2 -- run k618_modgap.sh first"; exit 1; }
	# with the second stage's modules in the list, the probe goes FIRST instead of last: its log
	# has to be running before them, or a module that takes the phone down leaves nothing behind
	FI="k618_probe $MI"; LA=


	if [ -n "${GKILOAD-tipc}" ]; then
		{ cat $S2; for m in ${GKILOAD-tipc}; do echo "$m.ko"; done; } > $K618/stage2-gki.load
		S2=$K618/stage2-gki.load
		echo "gkiload: ${GKILOAD-tipc}"
	fi
	# LATE="<module> ...": moved to the end of the batch -- for suspects, so that every other
	# module is tried (and logged) before them and an end that comes last is theirs
	if [ -n "${LATE:-}" ]; then
		awk -v late="$LATE" '
			function norm(s) { gsub(/-/, "_", s); sub(/\.ko$/, "", s); return s }
			BEGIN { n = split(late, a, " "); for (i = 1; i <= n; i++) l[norm(a[i])] = 1 }
			{ if (norm($0) in l) { tail = tail $0 "\n"; next } print }
			END { printf "%s", tail }' $S2 > $K618/stage2-late.load
		S2=$K618/stage2-late.load
		echo "late: $LATE"
	fi


	if [ "${BATCH:-1}" != 0 ]; then
		BA=1
		EX="$EX $D/k618_load.c"
		PO="$PO;k618_load list=/lib/modules/k618_batch.load dir=/lib/modules helper=/k618_umh"
		LA=k618_load			# the only one of the batch init loads, and last
	else
		LA="${LATE:-}"
	fi
	if [ "${TOLERATE:-1}" != 0 ]; then
		EX="$EX $D/k618_insmod.c"
		# EXTRA_Q=1: k618_badmod too -- a load that is certain to fail (and with fault=1 one
		# that faults), for a QEMU run that proves the mechanism on a boot where nothing else does
		[ -n "${EXTRA_Q:-}" ] && EX="$EX $D/k618_badmod.c"


		PO="$PO;k618_insmod tolerate=$([ -n "$BA" ] && echo 0 || echo 1) max=${TOLERATE_MAX:-0}"


		PO="$PO tolerate_comm=${TOLERATE_COMM-modprobe}"
		{ echo k618_insmod.ko; cat $S2; } > $K618/stage2-tolerant.load
		S2=$K618/stage2-tolerant.load
	fi
fi
# MADRID="<module> ...": the 18 Pro Max's (madrid's) own 6.18 builds of these -- Xiaomi's closed
# modules and its WALT. Our kernel's KMI is madrid's, so one loads as it is when every symbol it
# imports is ours with the same CRC (k618_madrid_load.py checks them: all but mist). They go
# into k618_load's batch only, after what they need, never into init's first-stage list, where one
# module that will not load ends the boot. MADRID_ROOT: the unpacked firmware.
XM=; BO=
if [ -n "${MADRID:-}" ]; then
	[ -n "$BA" ] || { echo "MADRID needs the batch (STAGE2=1, BATCH=1)"; exit 1; }
	MR=${MADRID_ROOT:-$NB/madrid-OS4.0.24.0.XEOCNXM}
	for m in $MADRID; do
		f=$(find $MR/vendor_boot/ramdisk0/lib/modules $MR/vendor_dlkm/lib/modules \
			$MR/system_dlkm/lib/modules -name "$m.ko" 2>/dev/null | head -1)
		[ -n "$f" ] || { echo "MADRID: no $m.ko in $MR"; exit 1; }
		XM="$XM $f"; BO="$BO $m"
	done
	# madrid's WALT calls one function of mist's, which is not loaded (k618_mi_shim.c says why)
	case " $MADRID " in *" sched-walt "*) EX="$EX $D/k618_mi_shim.c"; BO="$BO k618_mi_shim";; esac
	echo "madrid: $(echo $MADRID | wc -w) modules into the batch"
fi


if [ -n "$BA" ] && [ "${MITHERMAL:-1}" != 0 ]; then
	EX="$EX $D/k618_mi_thermal.c"; BO="$BO k618_mi_thermal"
	echo "mithermal: k618_mi_thermal into the batch"
fi
# MIPOWER=1 (default on with the batch): mi_power, the power_debug class with temp_aware -- the
# mode PowerKeeper's low-temperature handler writes and mi_thermald, CameraMind and libcameraopt
# read (init.qcom.rc gives it to system, the vendor labels it sysfs_poweraware); a rewrite of that
# part of the phone's own 5.15 mi_power.ko (k618_mi_power.c, under the stock name).
if [ -n "$BA" ] && [ "${MIPOWER:-1}" != 0 ]; then
	EX="$EX $D/k618_mi_power.c=mi_power"; BO="$BO mi_power"
	echo "mipower: mi_power into the batch"
fi
# PLACE=1 (default on with the batch): k618_place (k618_place.c) -- the kernel's own EAS keeps
# placing tasks, with four changes around it and the CPU energy model taken from measured power
# (k618_place_em.py): RT tasks little cores first and never onto a CPU running a draw thread (the
# top-app main thread or RenderThread), a draw thread waiting 2 ms pulled to an idle CPU of its
# cluster, EEVDF 1 ms slices for draw threads, no sync hint from a middle / prime waker for a task that
# fits a little core. Each part is a parameter of /sys/module/k618_place/parameters (rt pull slice sync
# em ...; "state" says what each did, "em_table" shows every OPP); MODOPTS="k618_place em=0" etc. starts
# with a part off. The restricted hooks it takes cannot come out again, so it never unloads. In the
# batch after MADRID's modules (cameramsger's newidle hook is the other one of its two slots) and after
# cpufreq is up (the energy model it replaces has to exist; it retries for 120 s otherwise). It
# includes kernel/sched/sched.h, so it is built with the Image's own tree every time (EXTRA does).
if [ -n "$BA" ] && [ "${PLACE:-1}" != 0 ]; then
	EX="$EX $D/k618_place.c"; BO="$BO k618_place"
	echo "place: k618_place into the batch"
fi
# PLACE_TEST=1 (QEMU only, never for the phone; run with QEMU_SMP=8): k618_place_test right after
# k618_place -- three energy-model domains like the phone's, then the model swap and its way back, the
# slices, both pull paths and the RT placement are checked (k618_place_test.c); the batch takes ~15 s
# longer. Results: "k618_place_test: PASS ..." / "FAIL ..." and "N passed, M failed".
if [ -n "$BA" ] && [ -n "${PLACE_TEST:-}" ]; then
	[ -n "${PLACE:-1}" ] && [ "${PLACE:-1}" != 0 ] || { echo "PLACE_TEST needs PLACE"; exit 1; }
	EX="$EX $D/k618_place_test.c"; BO="$BO k618_place_test"
	echo "place_test: k618_place_test after it (QEMU only)"
fi


if [ -n "$BA" ] && [ "${PERFHELPER:-1}" != 0 ]; then
	EX="$EX $D/k618_perf_helper.c=perf_helper"; BO="$BO perf_helper"
	echo "perfhelper: perf_helper into the batch"
fi
# CAMLOG=1 (default on with the batch): cameralog, a rewrite of the phone's own 5.15 cameralog.ko
# (k618_camlog.c, built under the stock name): /dev/camlog, the one-message mailbox the camera HAL's
# libcamlog.so and the port's libcameraopt open; the vendor's init.qcom.rc and file_contexts already
# give the node its mode and label.
if [ -n "$BA" ] && [ "${CAMLOG:-1}" != 0 ]; then
	EX="$EX $D/k618_camlog.c=cameralog"; BO="$BO cameralog"
	echo "camlog: cameralog into the batch"
fi


if [ -n "$BA" ] && [ "${MIWILL:-1}" != 0 ]; then
	EX="$EX $D/k618_miwill.c=miwill"; BO="$BO miwill"
	echo "miwill: miwill into the batch"
fi
# MEXLOG=1 (default on with the batch): mi_exception_log, a rewrite of the phone's own 5.15
# mi_exception_log.ko (k618_mi_exception_log.c, stock name): /dev/mi_exception_log, the 100-message
# ring HyperOS's framework opens; the vendor's init.qcom.rc already gives the node its mode.
if [ -n "$BA" ] && [ "${MEXLOG:-1}" != 0 ]; then
	EX="$EX $D/k618_mi_exception_log.c=mi_exception_log"; BO="$BO mi_exception_log"
	echo "mexlog: mi_exception_log into the batch"
fi
# MIMEMORY=1: mi_memory, a rewrite of the phone's own 5.15 mi_memory.ko (k618_mi_memory.c): UFS
# descriptors, serial, host state, DDR info, /proc/mv for PowerKeeper, DeviceInfoQR, dumpstate,
# MiuiCit. EXTEND_RECLAIM=1: extend_reclaim (k618_extend_reclaim.c, fuxi's source on 6.18; nothing
# on this ROM writes it).
if [ -n "$BA" ] && [ "${MIMEMORY:-0}" != 0 ]; then
	EX="$EX $D/k618_mi_memory.c=mi_memory"; BO="$BO mi_memory"
	echo "mimemory: mi_memory into the batch"
fi
if [ -n "$BA" ] && [ "${EXTEND_RECLAIM:-0}" != 0 ]; then
	EX="$EX $D/k618_extend_reclaim.c=extend_reclaim"; BO="$BO extend_reclaim"
	echo "extend_reclaim: into the batch"
fi
# DEVICES=1 (default): the drivers of the 13's and 13 Ultra's own hardware that this phone's
# lists do not name -- the 13 Ultra's camera PMIC, PM8008 (qcom-i2c-pmic, the MFD parent, and
# qcom_pm8008-regulator, as its vendor_dlkm loads them), and its ToF sensor (stmvl53l5,
# k618_tof.sh). They bind only where the device tree has the part; on the 13 Pro they load and
# stay idle.
if [ -n "$BA" ] && [ "${DEVICES:-1}" != 0 ]; then
	BO="$BO qcom-i2c-pmic qcom_pm8008-regulator stmvl53l5"
	echo "devices: the 13 Ultra's PM8008 and ToF drivers into the batch"
fi


[ -n "${MODOPTS:-}" ] && PO="$PO;$MODOPTS"
[ -n "$S2" ] && TPMODS=${TPMODS:-1}		# a second-stage list needs the techpack modules in
TPMODS="${TPMODS:-0}" GKIMODS="${GKIMODS:-zram zsmalloc rfkill libarc4 ${GKILOAD-tipc}}" PANICSIG=1 \
	EXTRA="$EX" FIRST="$FI" LAST="$LA" OPTS="$PO" CMDLINE="$CL" STAGE2="$S2" \
	BATCH="$BA" MADRIDMODS="$XM" BATCH_ONLY="$BO" \
	bash "$D/k618_bootimg.sh" "$@" || exit 1
# Every versioned import of every module in the stage against its provider (another module of the
# stage, or the kernel): a module whose import differs is refused at load. A module built against an
# earlier build of another (a Wi-Fi driver against an earlier cfg80211, say) is refused on the phone,
# while in QEMU the Wi-Fi group is refused anyway (no PCIe there), so a boot does not show it.
# CRC_MISMATCH_OK=1 builds the image anyway.
if [ -d $K618/boot-test/stage/rd/lib/modules ]; then
	python3 -I "$D/k618_stage_crc.py" $O/Module.symvers $K618/boot-test/stage/rd/lib/modules | tail -12
	if [ "${PIPESTATUS[0]}" != 0 ] && [ "${CRC_MISMATCH_OK:-0}" = 0 ]; then
		echo "k618_timg: a module's import differs from its provider (above) -- not building the image"
		exit 1
	fi
fi
# IR=0 stops here: QEMU takes the stage ramdisk as its initrd and does not need it inside the
# kernel, so a run that is only going to QEMU saves the relink (k618_qemu.sh with no QEMU_KERNEL)
[ "${IR:-1}" = 0 ] && exit 0
# 2. a kernel that carries that ramdisk itself
bash "$D/k618_irbuild.sh"
# 3. the boot image again around that kernel, from the same stage
REUSE_STAGE=1 KERNEL=$K618/out-gki-ir/arch/arm64/boot/Image CMDLINE="$CL" bash "$D/k618_bootimg.sh" "$@"


if [ -n "${TAG:-}" ]; then
	cp $K618/boot-test/boot-618.img $K618/boot-test/boot-618-$TAG.img || exit 1
	echo "$TAG: $(sha256sum $K618/boot-test/boot-618-$TAG.img | cut -c1-16) \
$(stat -c %s $K618/boot-test/boot-618-$TAG.img) bytes"
fi
