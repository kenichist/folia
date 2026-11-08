#!/bin/zsh
# speed.sh: interleaved A/B speed runs. default = MallocLargeCache=1, off = 0, reexec = unset.
S=/private/tmp/claude-501/-Users-kenichitanzil-Desktop-note-taking-app--claude-worktrees-happy-napier-fcce48/0b348f49-1be5-41be-a1dd-a53ca7ff3c8d/scratchpad
export APP=$S/app-new.noindex/Folia.app/Contents/MacOS/Folia
export OUT=$S/speed.noindex
R=retry
retry() { for i in 1 2 3; do $S/run2.sh "$@"; [[ $? == 3 ]] || return 0; sleep 20; done; }
MAN=$S/ws.noindex/manifest.json
ORD=$(python3 -I -c "import json;print(json.load(open('$MAN'))['ordinary_page'])")
phase=${1:-all}
if [[ $phase == all || $phase == long ]]; then
  for r in 1 2 3; do
    $R long-default-$r default pdf pdf_dir=$S/pdf300.noindex app_nap=0 kinds_only=long
    $R long-off-$r off pdf pdf_dir=$S/pdf300.noindex app_nap=0 kinds_only=long
  done
fi
if [[ $phase == all || $phase == images ]]; then
  for r in 1 2; do
    $R images-default-$r default images media_dir=$S/media.noindex
    $R images-off-$r off images media_dir=$S/media.noindex
  done
fi
if [[ $phase == all || $phase == editor ]]; then
  export WS=$S/ws.noindex/ws.db
  for r in 1 2; do
    for m in default off; do
      $R typing-$m-$r $m typing manifest=$MAN doc=ordinary n=1000
    done
    for m in default off; do
      $R scroll-$m-$r $m scroll manifest=$MAN frames=1200
    done
    for m in default off; do
      $R open-$m-$r $m open manifest=$MAN n=120
    done
  done
fi
if [[ $phase == all || $phase == launch ]]; then
  export WS=$S/ws.noindex/ws.db
  for r in 1 2 3 4 5; do
    for m in default off reexec; do
      $R launch-$m-$r $m launch manifest=$MAN start_page=$ORD
    done
  done
fi
