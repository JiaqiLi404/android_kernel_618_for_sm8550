#!/bin/bash
# k618_stage2.sh [jobs]: k618_wide.sh's set plus the second-stage modules soc-repo's registry has
# (k618_stage2.txt, from k618_modgap.sh's "soc" class less two the phone does not use, and less
# hung_task_enh, which reads WALT's task data -- back with WALT). Run it in a memory-capped scope.
D=$(dirname "$(readlink -f "$0")")
exec bash $D/k618_base.sh ${1:-8} $(cat $D/k618_more.txt $D/k618_stage2.txt)
