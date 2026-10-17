#!/usr/bin/env bash
# Reproducible ucrt64 build/test helper for the experimental Ares worktree.
#
# Why this exists:
# - AGENTS.md says C:\msys64 but MSYS2 actually lives on P: on this machine.
# - Native Windows git rejects the P:\ source tree with "dubious ownership"
#   unless HOME points at the real user profile (/etc/profile resets it).
# - CMake passes MSYS-style paths to native git, so PATH must resolve git first.
# - Building with many parallel jobs exhausts the process address space and
#   kills unrelated translation units; build with -j1 unless a page file is grown.
set -e
export PATH="/ucrt64/bin:/p/Program Files/Git/cmd:/usr/bin:/bin:$PATH"
export MSYSTEM=UCRT64
export HOME='C:\Users\lupixele'
cd /p/Magnanimity/Projects/Aresthena/Ares/.migration-worktrees/sunshine
exec "$@"
