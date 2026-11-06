#!/bin/bash
# k618_sugov_qemu.sh [Image] [seconds, 900]: schedutilmium (k618_sched.py) in QEMU on a 6.18 GKI Image,
# two boots, each with k618_sugov_test.c as its /init (no Android, no shell: the test mounts what it
# needs, writes its lines into the kernel log and powers off -- so no first-stage init dies at 7.5 s):
#
#   1. main  the governor's checks: cpufreq_testbench.enable=1 (CPU 0 a policy of its own, CPUs 1-3 one
#            shared policy), the built-in values of both clusters, "walt", then the 5.15 boot test's
#            checks. PASS: "SUGOV TEST OK: <n> checks ..." and no WARNING / BUG / Oops in the log.
#   2. eas   EAS with it: QEMU's own virt device tree with CPU 0 at half the capacity of CPUs 1-3
#            (capacity-dmips-mhz 512 / 1024), the test bench's energy model (cpufreq_testbench.em=1),
#            sched_verbose, cpufreq.walt_governor=schedutil, sugov_mode=eas. PASS: "SUGOV EAS OK: <n>
#            checks", the test bench's two energy models registered (": 0"), EAS started at boot under
#            schedutil, and no WARNING / BUG / Oops.
#
# The Image: out-gki's by default -- the plain GKI kernel, k618_sched.py applied before it was built (the
# out-gki-ir Image unpacks our module layer over the initrd; nothing of that is needed here). The QEMU
# command line is k618_qemu.sh's (virt, cpu max, 4 CPUs, 3 GB, the same -append start), run here with
# logs of its own: $K618/sugov-qemu/{main,eas}.log (SUGOV_Q= elsewhere). Exit 0 when both
# pass. Runs one boot after the other; under TCG the first takes a few minutes.
set -u
. "$(dirname "$(readlink -f "$0")")/env.sh"
KD=$(dirname "$(readlink -f "$0")")
IMG=${1:-$O/arch/arm64/boot/Image}
T=${2:-900}
Q=${SUGOV_Q:-$K618/sugov-qemu}
mkdir -p $Q
[ -f "$IMG" ] || { echo "no Image $IMG"; exit 1; }
grep -aq 'schedutilmium' "$IMG" || echo "warning: $IMG has no schedutilmium in it (built before k618_sched.py?)"

# the test as /init: a static binary, and an initrd with the device nodes it needs (no devtmpfs here)
aarch64-linux-gnu-gcc -O2 -static -Wall -pthread -o $Q/init $KD/k618_sugov_test.c || exit 1
printf '%s\n' "dir /dev 0755 0 0" "nod /dev/console 0600 0 0 c 5 1" "nod /dev/kmsg 0644 0 0 c 1 11" \
	"nod /dev/null 0666 0 0 c 1 3" "dir /proc 0755 0 0" "dir /sys 0755 0 0" "dir /tmp 1777 0 0" \
	"file /init $Q/init 0755 0 0" > $Q/list
$O/usr/gen_init_cpio $Q/list > $Q/initrd.cpio || exit 1

# the EAS boot's device tree: QEMU's for this machine, CPU 0 at half the others' capacity
MACHINE="-cpu max,pauth-impdef=on -smp 4 -m 3072 -nic none"
qemu-system-aarch64 -M virt,dumpdtb=$Q/virt.dtb $MACHINE -nographic > /dev/null 2>&1
$O/scripts/dtc/dtc -q -I dtb -O dts -o $Q/virt.dts $Q/virt.dtb || exit 1
python3 -I - $Q/virt.dts $Q/virt-cap.dts <<'EOF' || exit 1
import re, sys
t = open(sys.argv[1]).read()
n = 0
def cap(m):
    global n
    n += 1
    return m.group(0) + "\n\t\t\tcapacity-dmips-mhz = <%d>;" % (512 if int(m.group(2), 16) == 0 else 1024)
t = re.sub(r"(\t\tcpu@[0-9a-f]+ \{\n(?:\t\t\t[^\n]*\n)*?\t\t\treg = <0x([0-9a-f]+)>;)", cap, t)
if n != 4:
    sys.exit(f"capacities: {n} CPU nodes found, not 4")
open(sys.argv[2], "w").write(t)
EOF
$O/scripts/dtc/dtc -q -I dts -O dtb -o $Q/virt-cap.dtb $Q/virt-cap.dts || exit 1

boot() {	# <name> <dtb or ""> <more of the command line>
	local start
	start=$(date +%s)
	timeout $T qemu-system-aarch64 -M virt $MACHINE ${2:+-dtb $2} -nographic -no-reboot \
		-kernel $IMG -initrd $Q/initrd.cpio \
		-append "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1 printk.devkmsg=on $3" > $Q/$1.log 2>&1
	echo "== $1: qemu exit $? after $(( $(date +%s) - start )) s, $(wc -l < $Q/$1.log) lines in $Q/$1.log"
}
traces() {	# <log>
	grep -a -c -E 'WARNING:|BUG:|Oops|Call trace:|Unable to handle|Kernel panic|rcu: .*stall' $1
}
lines() {	# <log>: the test's own lines, without the time stamp and the caller id ("[    1.13][    T1] ")
	grep -a -E 'sugov|SUGOV' $1 | grep -a -v -E 'Kernel command line|Command line' | sed -E 's/^(\[[^]]*\])+ ?//'
}

boot main "" "cpufreq_testbench.enable=1"
lines $Q/main.log > $Q/main.txt
grep -a -E 'FAILED|SUGOV TEST' $Q/main.txt
m_ok=0
grep -aq '^SUGOV TEST OK' $Q/main.txt && [ "$(traces $Q/main.log)" = 0 ] && m_ok=1
echo "main: $(grep -c '^sugov ' $Q/main.txt) check lines, $(traces $Q/main.log) kernel traces -> $([ $m_ok = 1 ] && echo PASS || echo FAIL)"

boot eas $Q/virt-cap.dtb "cpufreq_testbench.enable=1 cpufreq_testbench.em=1 sched_verbose cpufreq.walt_governor=schedutil sugov_mode=eas"
lines $Q/eas.log > $Q/eas.txt
grep -a -E 'sugov|SUGOV' $Q/eas.txt | grep -a -E 'EAS'
# (the serial console's lines end in \r\n)
ems=$(tr -d '\r' < $Q/eas.log | grep -a -c -E 'energy model for CPUs [0-9-]+: 0$')
booted=$(grep -a -c 'sched_energy_set: starting EAS' $Q/eas.log)
echo "eas: energy models registered $ems (2 wanted), 'starting EAS' $booted times in all, $(traces $Q/eas.log) kernel traces"
e_ok=0
grep -aq '^SUGOV EAS OK' $Q/eas.txt && [ "$ems" = 2 ] && [ "$booted" -ge 2 ] && [ "$(traces $Q/eas.log)" = 0 ] && e_ok=1
echo "eas: $([ $e_ok = 1 ] && echo PASS || echo FAIL)"
[ $m_ok = 1 ] && [ $e_ok = 1 ]
