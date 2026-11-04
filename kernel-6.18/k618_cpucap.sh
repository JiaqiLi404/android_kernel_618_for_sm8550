#!/system/bin/sh


S=${1:-30}
O=/data/local/tmp/nuwa/cpucap.txt
T=/sys/kernel/tracing
[ -d $T/events ] || T=/sys/kernel/debug/tracing
state() {
	echo "== $1 $(cut -d' ' -f1 /proc/uptime)"
	for p in /sys/devices/system/cpu/cpufreq/policy*; do
		echo "$(basename $p) max $(cat $p/scaling_max_freq) cpuinfo_max $(cat $p/cpuinfo_max_freq) min $(cat $p/scaling_min_freq) cur $(cat $p/scaling_cur_freq)"
	done
	echo "cpu_limits: $(cat /sys/class/thermal/thermal_message/cpu_limits 2>&1 | tr '\n' ' ')"
	for c in /sys/class/thermal/cooling_device*; do
		t=$(cat $c/type 2>/dev/null)
		case $t in *cpu*|*cluster*|*CPU*) echo "$(basename $c) $t $(cat $c/cur_state)/$(cat $c/max_state)";; esac
	done
}
{
echo "== $(cat /proc/version | cut -c1-60)"
state before
echo 0 > $T/tracing_on
echo > $T/trace
echo > $T/kprobe_events
echo 'p:k618cap/maxw store_scaling_max_freq val=+0(%x1):string' >> $T/kprobe_events && echo "kprobe store_scaling_max_freq: ok"
echo 'p:k618cap/limw cpu_limits_store val=+0(%x2):string' >> $T/kprobe_events 2>/dev/null && echo "kprobe cpu_limits_store: ok"
echo 'p:k618cap/qos freq_qos_update_request req=%x0 val=%x1' >> $T/kprobe_events && echo "kprobe freq_qos_update_request: ok"
echo 1 > $T/events/k618cap/enable
echo 1 > $T/events/power/cpu_frequency_limits/enable
echo 1 > $T/tracing_on
sleep $S
echo 0 > $T/tracing_on
echo "== trace ($S s)"
grep -v "^#" $T/trace | tail -400
echo 0 > $T/events/power/cpu_frequency_limits/enable
echo 0 > $T/events/k618cap/enable
echo > $T/kprobe_events
echo > $T/trace
echo 1 > $T/tracing_on
state after
} > $O 2>&1
echo done
