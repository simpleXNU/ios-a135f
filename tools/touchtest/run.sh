#!/bin/sh
# Host test of bsd/dev/a135f_touch.c against a TD4150 model. Run from the repo root after the
# patches are applied to an xnu tree: ./run.sh /path/to/xnu/bsd/dev/a135f_touch.c
set -e
d=$(mktemp -d); mkdir -p $d/inc/sys $d/inc/kern $d/inc/machine $d/inc/pexpert
for h in sys/param.h sys/systm.h kern/clock.h kern/thread.h kern/sched_prim.h kern/kalloc.h machine/machine_routines.h pexpert/pexpert.h pexpert/device_tree.h; do : > $d/inc/$h; done
cp "$1" $d/a135f_touch.c; cp "$(dirname "$0")/test-touch-tcm.c" $d/
cd $d && gcc -Iinc -Wall -Wextra -Wno-unused-function -Wno-unused-variable -o t test-touch-tcm.c && ./t | tail -3
