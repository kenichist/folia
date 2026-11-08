#!/bin/zsh
# allocrun.sh <label> <app dir> <n>: typing bench with the bigalloc interposer, MallocLargeCache=1.
S=/private/tmp/claude-501/-Users-kenichitanzil-Desktop-note-taking-app--claude-worktrees-sweet-lewin-82ad11/79720245-f24b-4143-8925-85f5dab3d9a2/scratchpad
label=$1; appdir=$2; n=$3
OUT=${OUT:-$S/alloc.noindex}; mkdir -p $OUT
home=$S/homes.noindex/$label; rm -rf $home; mkdir -p $home
cp $S/ws.noindex/ws.db $home/workspace.db
echo "start $(date +%T) pressure=$(sysctl -n kern.memorystatus_vm_pressure_level)" > $OUT/$label.log
# caffeinate is a SIP binary: it would strip DYLD_INSERT_LIBRARIES, so it only watches the pid.
env MallocLargeCache=1 DYLD_INSERT_LIBRARIES=$S/bin/bigalloc.dylib BIGALLOC_OUT=$OUT/$label.txt \
  $appdir/Folia.app/Contents/MacOS/Folia --home $home --no-watch --bench typing \
  --bench-out $OUT/$label.json --bench-arg manifest=$S/ws.noindex/manifest.json --bench-arg doc=ordinary \
  --bench-arg n=$n ${=EXTRA} >> $OUT/$label.log 2>&1 &
pid=$!
caffeinate -d -i -w $pid &
wait $pid
rc=$?
echo "end $(date +%T) rc=$rc pressure=$(sysctl -n kern.memorystatus_vm_pressure_level)" >> $OUT/$label.log
rm -rf $home
echo "$label: $(head -1 $OUT/$label.txt)"
