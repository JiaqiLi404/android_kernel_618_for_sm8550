#!/bin/bash
# k618_wide.sh [jobs]: k618_base.sh with the modules listed in k618_more.txt as well (the rest of the
# 13 Pro's first stage and the drivers its device tree asks for). Run it in a memory-capped scope.
D=$(dirname "$(readlink -f "$0")")
exec bash $D/k618_base.sh ${1:-8} $(cat $D/k618_more.txt)
