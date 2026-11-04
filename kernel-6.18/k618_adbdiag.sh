#!/bin/bash


set -u
A=${ADB:-adb}
TAG=${1:?usage: k618_adbdiag.sh <test tag> [look|gl|gpu|size]}
STEP=${2:-look}
PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$PROJECT_DIR/logs/diagnostics/$TAG-adb}
mkdir -p "$OUT" || exit 1
$A devices -l > "$OUT/$STEP-devices.txt" 2>&1
K=$($A shell uname -r 2>/dev/null | tr -d '\r')
# ANY_KERNEL=1: whatever kernel is up -- for trying the script out on the phone's own 5.15
case "${ANY_KERNEL:+any}$K" in
6.18*|any?*) ;;
"") echo "k618_adbdiag: no phone on adb"; cat "$OUT/$STEP-devices.txt"; exit 1;;
*) echo "k618_adbdiag: the phone runs $K, not the 6.18 test kernel -- nothing taken"; exit 1;;
esac
U=$($A shell id -u 2>/dev/null | tr -d '\r')
echo "k618_adbdiag: $K, adb shell uid $U, $(date +%T)"

# shot <name>: what SurfaceFlinger composed, as a PNG
shot() {
	$A shell "screencap -p /data/local/tmp/k618-$1.png" &&
		$A pull /data/local/tmp/k618-$1.png "$OUT/$1.png" > /dev/null &&
		$A shell "rm -f /data/local/tmp/k618-$1.png"
	echo "  $1.png: $(stat -c %s "$OUT/$1.png" 2>/dev/null || echo none) bytes"
}

case "$STEP" in
look)
	$A shell 'cat /proc/uptime; getprop' > "$OUT/look-props.txt" 2>&1
	shot look
	for d in SurfaceFlinger display window power; do
		$A shell "dumpsys $d" > "$OUT/look-$d.txt" 2>&1
	done
	$A shell 'logcat -d -b all -v threadtime' > "$OUT/look-logcat.txt" 2>&1
	# shell can read these: which display nodes exist and what they hold
	$A shell 'for f in /sys/class/backlight/*/brightness /sys/class/backlight/*/max_brightness \
		/sys/class/backlight/*/actual_brightness /sys/class/drm/card0-DSI-1/status \
		/sys/class/drm/card0-DSI-1/enabled /sys/class/drm/card0-DSI-1/dpms \
		/sys/class/mi_display/disp-DSI-0/brightness_clone; do echo "$f: $(cat $f 2>&1)"; done;
		ls -l /sys/class/backlight/ /sys/class/drm/' > "$OUT/look-sysfs.txt" 2>&1
	if [ "$U" = 0 ]; then
		# the driver's view: planes, CRTC, connector -- debugfs, mounted for this if it is not
		$A shell 'D=$(awk "\$3==\"debugfs\" {print \$2; exit}" /proc/mounts);
			[ -n "$D" ] || { D=/data/local/tmp/k618dbg; mkdir -p $D; mount -t debugfs none $D; };
			cat $D/dri/0/state; echo; ls $D/dri/0' > "$OUT/look-drm.txt" 2>&1
	fi
	grep -m1 -E "Display .* HWC layers" -A 40 "$OUT/look-SurfaceFlinger.txt" | head -40
	;;
gl)
	echo "  renderer was: $($A shell getprop debug.hwui.renderer | tr -d '\r')"
	echo "  $(date +%T) apps' renderer OpenGL ES, SystemUI restarted"
	$A shell 'setprop debug.hwui.renderer skiagl; am crash com.android.systemui' 2>&1 | tee "$OUT/gl-call.txt"
	sleep 10
	shot gl
	$A shell 'dumpsys SurfaceFlinger' > "$OUT/gl-SurfaceFlinger.txt" 2>&1
	$A shell 'logcat -d -b all -v threadtime' > "$OUT/gl-logcat.txt" 2>&1
	;;
gpu)
	echo "  $(date +%T) GPU composition on"
	$A shell 'service call SurfaceFlinger 1008 i32 1' | tee "$OUT/gpu-call-on.txt"
	sleep 5
	shot gpu-on
	$A shell 'dumpsys SurfaceFlinger' > "$OUT/gpu-SurfaceFlinger.txt" 2>&1
	sleep 10
	$A shell 'service call SurfaceFlinger 1008 i32 0' | tee "$OUT/gpu-call-off.txt"
	echo "  $(date +%T) GPU composition off (back to the display hardware)"
	;;
size)
	# the override as it is now ("Override size: 1080x2400", "Override density: 420"), to put back
	S=$($A shell 'wm size' | tr -d '\r' | awk '/Override size/ {print $3}')
	DN=$($A shell 'wm density' | tr -d '\r' | awk '/Override density/ {print $3}')
	echo "  override now: size ${S:-none}, density ${DN:-none}"
	echo "  $(date +%T) native size"
	$A shell 'wm size reset; wm density reset'
	sleep 5
	shot size-native
	$A shell 'dumpsys SurfaceFlinger' > "$OUT/size-SurfaceFlinger.txt" 2>&1
	sleep 10
	[ -n "$S" ] && $A shell "wm size $S"
	[ -n "$DN" ] && $A shell "wm density $DN"
	echo "  $(date +%T) put back: $($A shell 'wm size; wm density' | tr -d '\r' | grep Override | tr '\n' ' ')"
	;;
*)
	echo "k618_adbdiag: unknown step $STEP (look, gl, gpu, size)"; exit 1;;
esac
ls -la "$OUT" | tail -n +2 | awk '{print "  " $5, $NF}'
