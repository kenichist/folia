#!/bin/zsh
S=/private/tmp/claude-501/-Users-kenichitanzil-Desktop-note-taking-app--claude-worktrees-happy-napier-fcce48/0b348f49-1be5-41be-a1dd-a53ca7ff3c8d/scratchpad
export APP=$S/app-img.noindex/Folia.app/Contents/MacOS/Folia
export OUT=$S/images2.noindex
retry() { for i in 1 2 3; do $S/run2.sh "$@"; [[ $? == 3 ]] || return 0; sleep 20; done; }
for r in 1 2 3; do
  retry images2-default-$r default images media_dir=$S/media.noindex
  retry images2-off-$r off images media_dir=$S/media.noindex
done
