#!/bin/zsh
# speed.sh: interleaved typing A/B. fix = painter clip dropped (default), clip = painter_clip=1 (as before);
# default = MallocLargeCache=1, off = MallocLargeCache=0.
S=/private/tmp/claude-501/-Users-kenichitanzil-Desktop-note-taking-app--claude-worktrees-sweet-lewin-82ad11/79720245-f24b-4143-8925-85f5dab3d9a2/scratchpad
export OUT=$S/speed.noindex
export WS=$S/ws.noindex/ws.db
retry() { for i in 1 2 3; do $S/run.sh "$@"; [[ $? == 3 ]] || return 0; sleep 20; done; }
MAN=$S/ws.noindex/manifest.json
for r in ${=REPS:-1 2 3}; do
  retry typing-fix-off-$r off typing manifest=$MAN doc=ordinary n=1000
  retry typing-clip-default-$r default typing manifest=$MAN doc=ordinary n=1000 painter_clip=1
  retry typing-fix-default-$r default typing manifest=$MAN doc=ordinary n=1000
  retry typing-clip-off-$r off typing manifest=$MAN doc=ordinary n=1000 painter_clip=1
done
