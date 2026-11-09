#!/bin/zsh
# run.sh <label> <mode> <scenario> [bench-args...]
# One Folia bench in a fresh --home under caffeinate -d -i, logging the memory
# pressure level every 5 s. Modes (allocator variables at launch; Folia then
# re-executes itself with MallocLargeCache=0 unless that variable is set):
#   base  = nothing set (what ships: MallocLargeCache=0 via the re-exec)
#   nano  = MallocNanoZone=0
#   aggr  = MallocAggressiveMadvise=1
#   space = MallocSpaceEfficient=1
#   sys   = MallocLargeCache=1 (the system default, no re-exec)
# A gap over 15 s in the pressure log means the Mac slept: the run's files move
# to $OUT/invalid and the script exits 3 (retry it).
S=${S:?}
APP=${APP:-/Users/kenichitanzil/Desktop/note-taking-app/.claude/worktrees/wizardly-shamir-b53c0d/build-qa/src/Folia.app/Contents/MacOS/Folia}
label=$1; mode=$2; scenario=$3; shift 3
OUT=${OUT:-$S/out.noindex}; mkdir -p $OUT
home=$S/homes.noindex/$label; rm -rf $home; mkdir -p $home
[[ -n $WS ]] && cp $WS $home/workspace.db
args=()
for a in "$@"; do args+=(--bench-arg "$a"); done
( while true; do echo "$(date +%T) $(sysctl -n kern.memorystatus_vm_pressure_level)"; sleep 5; done ) > $OUT/$label.pressure-level.log &
mon=$!
echo "start $(date +%T) mode=$mode pressure=$(sysctl -n kern.memorystatus_vm_pressure_level) load=$(sysctl -n vm.loadavg) batt=$(pmset -g batt | grep -o '[0-9]*%')" > $OUT/$label.log
case $mode in
  base) envset=(-u MallocNanoZone -u MallocLargeCache) ;;
  nano) envset=(-u MallocNanoZone -u MallocLargeCache MallocNanoZone=0) ;;
  aggr) envset=(-u MallocNanoZone -u MallocLargeCache MallocAggressiveMadvise=1) ;;
  space) envset=(-u MallocNanoZone -u MallocLargeCache MallocSpaceEfficient=1) ;;
  sys) envset=(-u MallocNanoZone MallocLargeCache=1) ;;
  *) echo "unknown mode $mode"; kill $mon; exit 2 ;;
esac
env $envset caffeinate -d -i $APP --home $home --no-watch --bench $scenario --bench-out $OUT/$label.json ${=EXTRA} "${args[@]}" >> $OUT/$label.log 2>&1
rc=$?
kill $mon
echo "end $(date +%T) rc=$rc pressure=$(sysctl -n kern.memorystatus_vm_pressure_level) load=$(sysctl -n vm.loadavg)" >> $OUT/$label.log
rm -rf $home
gap=$(awk '{split($1,t,":"); s=t[1]*3600+t[2]*60+t[3]; if (p && s-p>15) g=g " " prev "->" $1; p=s; prev=$1} END{print g}' $OUT/$label.pressure-level.log)
if [[ -n $gap ]]; then
  echo "INVALID: the Mac slept:$gap" >> $OUT/$label.log
  mkdir -p $OUT/invalid
  stamp=$(date +%H%M%S)
  for f in $OUT/$label.*; do mv $f $OUT/invalid/$(basename $f).$stamp; done
  echo "$label INVALID (sleep:$gap)"
  exit 3
fi
echo "$label $(tail -1 $OUT/$label.log)"
exit $rc
