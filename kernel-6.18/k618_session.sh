#!/system/bin/sh
# k618_session.sh <part>: the 6.18 checks of todo section 2 / P2 that adb can do, on a running test image.
#   gov      CPU governors and frequencies, busy and after 8 s idle
#   thermal  thermal_message nodes with the screen on, then off (power key), then on again
#   mem      mi_memory.ko and extend_reclaim.ko loaded by hand, their nodes dumped as ref515_nodes.sh
#            dumps them on 5.15, then both unloaded
#   charge   battery / usb nodes (twice, 10 s apart)
# Output in /data/local/tmp/nuwa/session_<part>.txt.
P=${1:-gov}
O=/data/local/tmp/nuwa/session_$P.txt
N=/data/local/tmp/nuwa
dumpdir() {
	for f in $(ls $1 2>/dev/null); do
		[ -f "$1/$f" ] || continue
		echo "--- $1/$f"
		cat "$1/$f" 2>&1 | head -60
	done
}
govs() {
	for p in /sys/devices/system/cpu/cpufreq/policy*; do
		echo "$p $(cat $p/scaling_governor) cur $(cat $p/scaling_cur_freq) min $(cat $p/scaling_min_freq) max $(cat $p/scaling_max_freq)"
	done
}
{
echo "== $P $(cat /proc/version | cut -c1-60) up $(cut -d' ' -f1 /proc/uptime)"
case $P in
gov)
	govs
	sleep 8
	echo "-- after 8 s"; govs
	;;
thermal)
	for f in screen_state board_sensor_temp sconfig balance_mode charger_temp; do echo "on  $f: $(cat /sys/class/thermal/thermal_message/$f 2>&1)"; done
	input keyevent 26; sleep 3
	for f in screen_state board_sensor_temp; do echo "off $f: $(cat /sys/class/thermal/thermal_message/$f 2>&1)"; done
	input keyevent 26; sleep 2
	echo "back on screen_state: $(cat /sys/class/thermal/thermal_message/screen_state 2>&1)"
	;;
mem)
	insmod $N/mi_memory.ko && echo "mi_memory loaded" || echo "mi_memory: insmod failed"
	insmod $N/extend_reclaim.ko && echo "extend_reclaim loaded" || echo "extend_reclaim: insmod failed"
	echo "== /proc/mv"; cat /proc/mv 2>&1
	echo "== mi_memory"; ls -la /sys/class/mi_memory/mi_memory_device/ 2>&1
	dumpdir /sys/class/mi_memory/mi_memory_device/ufshcd0
	dumpdir /sys/class/mi_memory/mi_memory_device/ddr
	echo "== extend_reclaim"; ls -la /sys/kernel/extend_reclaim/ 2>&1; dumpdir /sys/kernel/extend_reclaim
	rmmod extend_reclaim; rmmod mi_memory
	echo "after rmmod: $(lsmod | grep -c -E '^(mi_memory|extend_reclaim) ') left"
	dmesg | grep -E "mi_memory|extend_reclaim" | tail -8
	;;
charge)
	for n in 1 2; do
		echo "-- reading $n"
		for f in status charge_type current_now voltage_now power_now constant_charge_current temp; do echo "battery $f: $(cat /sys/class/power_supply/battery/$f 2>&1)"; done
		for f in online type usb_type input_current_limit current_max voltage_max; do echo "usb $f: $(cat /sys/class/power_supply/usb/$f 2>&1)"; done
		ls /sys/class/qcom-battery/ 2>/dev/null | head -40 | tr '\n' ' '; echo
		for f in real_type authentic fastcharge_mode quick_charge_type power_max; do echo "qcom-battery $f: $(cat /sys/class/qcom-battery/$f 2>&1)"; done
		sleep 10
	done
	;;
esac
} > $O 2>&1
echo done
