#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release point's Pi runbook, part B: §8's programs from the corpus - NPB
# IS, PRK, ISx, the examples, OSU, ssca1, the SOS suite - built with oshcc in
# copies under ~/osoe/build/corpus (the runbook's W; nothing outside ~/osoe),
# outputs in ~/osoe/build/release-run/corpus. Usage: pi-b.sh (after part A).
set -u
C=<repo>/corpus
OUT=<repo>/build/scratch/release-pi/b
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
$PI 'rm -rf osoe/build/corpus && mkdir -p osoe/build/corpus osoe/build/release-run/corpus' || exit 1
for pair in "openshmem-npbs/C:npb" "prk:prk" "openshmem-examples/c:examples" "isx/SHMEM:isx" \
            "osu-omb/osu-micro-benchmarks-7.5.2:osu" "osb/ssca1:ssca1" "tests-sos/test:sos"; do
  rsync -a "$C/${pair%%:*}/" "pi-login:osoe/build/corpus/${pair##*:}/" || { echo "rsync ${pair} failed" >> $LOG; exit 1; }
done
$PI ': > osoe/build/corpus/prk/common/make.defs'
ENV='cd osoe && W=$PWD/build/corpus; R=$PWD/build/release-run/corpus; OSHCC=$PWD/build/release/tools/wrapper/oshcc'
$PI "$ENV"'; cd $W/npb && mkdir -p stub && : > stub/mpi.h &&
  sed -e "s|^SHMEMCC.*|SHMEMCC = $OSHCC|" -e "s|^CFLAGS.*|CFLAGS = -O2 -g -fpermissive|" -e "s|^CLINKFLAGS.*|CLINKFLAGS = -O2 -g|" -e "s|^CMPI_INC.*|CMPI_INC = -I$W/npb/stub|" config/make.def.template > config/make.def &&
  make is CLASS=A NPROCS=16 > $R/npb-build-A.txt 2>&1 && srun -K1 -N4 --ntasks-per-node=4 bin/is.A.16 > $R/is-A-16.txt 2>&1;
  make is CLASS=B NPROCS=16 > $R/npb-build-B.txt 2>&1 && srun -K1 -N4 --ntasks-per-node=4 bin/is.B.16 > $R/is-B-16.txt 2>&1;
  echo "npb: A $(grep -c "Verification    =               SUCCESSFUL\|SUCCESSFUL" $R/is-A-16.txt) B $(grep -c SUCCESSFUL $R/is-B-16.txt)"' >> $LOG
$PI "$ENV"'; cd $W/prk/SHMEM && for k in Stencil:stencil Synch_p2p:p2p Transpose:transpose; do (cd ${k%%:*} && make SHMEMCC="$OSHCC -std=gnu11" DEFAULT_OPT_FLAGS=-O2 LIBS=-lm ${k##*:}) > $R/prk-build-${k##*:}.txt 2>&1; done;
  srun -K1 -N4 --ntasks-per-node=4 Stencil/stencil 20 2000 > $R/prk-stencil.txt 2>&1;
  srun -K1 -N4 --ntasks-per-node=4 Synch_p2p/p2p 10 2000 2000 > $R/prk-p2p.txt 2>&1;
  srun -K1 -N4 --ntasks-per-node=4 Transpose/transpose 10 2048 > $R/prk-transpose.txt 2>&1;
  echo "prk: $(grep -l "Solution validates" $R/prk-stencil.txt $R/prk-p2p.txt $R/prk-transpose.txt | wc -l) of 3 validate"' >> $LOG
$PI "$ENV"'; cd $W/isx && make CC=$OSHCC CFLAGS="-O2 -std=gnu99 -Wall" > $R/isx-build.txt 2>&1; export SHMEM_SYMMETRIC_SIZE=1200M;
  srun -K1 -N4 --ntasks-per-node=4 bin/isx.weak 16777216 $R/isx-weak.log > $R/isx-weak.txt 2>&1; a=$?;
  srun -K1 -N4 --ntasks-per-node=4 bin/isx.strong 268435456 $R/isx-strong.log > $R/isx-strong.txt 2>&1; b=$?;
  echo "isx: weak $a strong $b"' >> $LOG
$PI "$ENV"'; cd $W/examples && for src in *.c; do p=${src%.c};
    $OSHCC -std=gnu11 -O1 $src -o $p -lm 2> $p.build || { echo "$p: build failed"; continue; };
    timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./$p > $p.out 2>&1 < /dev/null; echo "$p: exit $?"; done > $R/examples.txt 2>&1;
  echo "examples: $(grep -c ": exit 0" $R/examples.txt) exit 0 of $(grep -c ": exit\|build failed" $R/examples.txt); not 0: $(grep -v ": exit 0" $R/examples.txt | tr "\n" " ")"' >> $LOG
$PI "$ENV"'; cd $W/osu && (./configure CC=$OSHCC CXX=g++ && find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} + &&
    find . -name configure -exec touch {} + && find . -name Makefile.in -exec touch {} + && find . -name Makefile -exec touch {} + && make -C c/openshmem -j4) > $R/osu-build.txt 2>&1; echo "osu build: $?";
  cd c/openshmem && for p in put get put_bw get_bw put_nb get_nb put_nb_bw get_nb_bw put_overlap get_overlap atomics put_mr put_mr_nb get_mr_nb; do
    timeout 600 srun -K1 -N2 --ntasks-per-node=1 ./osu_oshm_$p heap > $R/osu-$p.txt 2>&1; echo "osu $p: $?"; done;
  for p in barrier broadcast collect fcollect reduce; do
    timeout 600 srun -K1 -N4 --ntasks-per-node=1 ./osu_oshm_$p -m :1048576 > $R/osu-$p-4x1.txt 2>&1; a=$?;
    timeout 600 srun -K1 -N4 --ntasks-per-node=4 ./osu_oshm_$p -m :1048576 > $R/osu-$p-4x4.txt 2>&1; echo "osu $p: $a $?"; done' >> $LOG
$PI "$ENV"'; cd $W/ssca1 && sed -i "/sequence->sequence\[idx\] = rand()%64;/{n;s/^  }$/  }\n  BARRIER_ALL();/}" gen_scal_data.c && make COMPILER=oshcc CC=$OSHCC > $R/ssca1-build.txt 2>&1;
  SCALE=14 srun -K1 -N2 --ntasks-per-node=2 ./ssca1 > $R/ssca1-2x2.txt 2>&1; SCALE=14 srun -K1 -N4 --ntasks-per-node=1 ./ssca1 > $R/ssca1-4x1.txt 2>&1;
  echo "ssca1: $(grep -c "Found 3 acceptable alignments with scores from 55 to 53" $R/ssca1-2x2.txt $R/ssca1-4x1.txt | tr "\n" " ") succeeded $(grep -c succeeded $R/ssca1-2x2.txt) $(grep -c succeeded $R/ssca1-4x1.txt)"' >> $LOG
$PI "$ENV"'; cd $W/sos && for src in unit/*.c unit/*.cpp spec-example/*.c apps/*.c; do p=${src%.*}; cc=; extra=;
    case $p in unit/mt_lock|spec-example/hybrid_mpi_*) continue ;; unit/mt_lock_test) extra=unit/mt_lock.c ;; spec-example/shmem_ctx) extra=-fopenmp ;; esac;
    case $src in *.cpp) cc=OSOE_CC=g++ ;; esac;
    env $cc $OSHCC -O1 -Iinclude $src $extra -o $p -lm -pthread 2> $p.build || { echo "$p: build failed"; continue; };
    timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./$p > $p.out 2>&1 < /dev/null; echo "$p: exit $?"; done > $R/sos.txt 2>&1;
  for x in "rma_coverage:rma_coverage_pshmem:-DTEST_PSHMEM" "query_thread:query_thread_funneled:-DENABLE_THREADS"; do
    IFS=: read src p flag <<< "$x"; $OSHCC -O1 -Iinclude $flag unit/$src.c -o unit/$p -lm -pthread 2> unit/$p.build || { echo "unit/$p: build failed" >> $R/sos.txt; continue; };
    timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./unit/$p > unit/$p.out 2>&1 < /dev/null; echo "unit/$p: exit $?" >> $R/sos.txt; done;
  timeout 120 srun -K1 -N4 --ntasks-per-node=2 ./unit/bigget -l 100 > $R/sos-bigget-l100.txt 2>&1; echo "unit/bigget -l 100: exit $?" >> $R/sos-alone.txt;
  echo "sos: $(grep -c ": exit 0" $R/sos.txt) exit 0; not: $(grep -v ": exit 0" $R/sos.txt | tr "\n" " "); $(cat $R/sos-alone.txt)"' >> $LOG
rsync -a 'pi-login:osoe/build/release-run/corpus/' $OUT/corpus/
echo "end $(date +%H:%M:%S)" >> $LOG
