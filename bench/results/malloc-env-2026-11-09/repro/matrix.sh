#!/bin/zsh
# matrix.sh: the allocator A/B, modes interleaved per round so drift hits all.
# Rounds 1-2: base, space, nano, aggr (import, idle, typing) + sys (import);
# round 3: base, space. A run whose Mac slept (exit 3) is retried once.
export S=${S:?}
export OUT=${OUT:-$S/out.noindex}
FIX=$S/fixture
one() { # label mode scenario args...
  local label=$1
  [[ -f $OUT/$label.json ]] && { echo "$label exists, skipped"; return; }
  $S/repro/run.sh "$@"; local rc=$?
  [[ $rc == 3 ]] && { $S/repro/run.sh "$@"; rc=$?; }
  return $rc
}
for round in ${=ROUNDS:-1 2 3}; do
  if [[ $round == 3 ]]; then modes=(base space); else modes=(base space nano aggr); fi
  for mode in $modes; do
    export WS=; one import-$mode-$round $mode import vm_snapshots=1
    export WS=$FIX/workspace.db; one idle-$mode-$round $mode idle manifest=$FIX/manifest.json seconds=60 vm_snapshots=1
    one typing-$mode-$round $mode typing manifest=$FIX/manifest.json doc=ordinary n=1000
  done
  if [[ $round != 3 ]]; then export WS=; one import-sys-$round sys import vm_snapshots=1; fi
done
# The setup of §6e's runs (the first tab without a page), on this build.
export WS=; one import0-base-1 base import vm_snapshots=1 first_page=0
echo "matrix done $(date +%T)"
