#!/bin/zsh
# run2.sh <label> <mlc: default (MallocLargeCache=1) | off (=0) | reexec (unset)> <scenario> [bench-args...]
# Runs one Folia bench in a fresh home under caffeinate, logging the pressure level every 5 s.
# A gap of more than 15 s in that log means the Mac slept: the run's files move to $OUT/invalid
# and the script exits 3 (retry it).
S=/private/tmp/claude-501/-Users-kenichitanzil-Desktop-note-taking-app--claude-worktrees-happy-napier-fcce48/0b348f49-1be5-41be-a1dd-a53ca7ff3c8d/scratchpad
APP=${APP:-/Users/kenichitanzil/Desktop/note-taking-app/.claude/worktrees/happy-napier-fcce48/build/src/Folia.app/Contents/MacOS/Folia}
label=$1; mlc=$2; scenario=$3; shift 3
OUT=${OUT:-$S/out.noindex}; mkdir -p $OUT
home=$S/homes.noindex/$label; rm -rf $home; mkdir -p $home
[[ -n $WS ]] && cp $WS $home/workspace.db
args=()
for a in "$@"; do args+=(--bench-arg "$a"); done
( while true; do echo "$(date +%T) $(sysctl -n kern.memorystatus_vm_pressure_level)"; sleep 5; done ) > $OUT/$label.pressure-level.log &
mon=$!
echo "start $(date +%T) pressure=$(sysctl -n kern.memorystatus_vm_pressure_level) load=$(sysctl -n vm.loadavg)" > $OUT/$label.log
case $mlc in
  off) envset=(MallocLargeCache=0) ;;
  default) envset=(MallocLargeCache=1) ;;
  reexec) envset=(-u MallocLargeCache) ;;
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
