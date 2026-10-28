#!/usr/bin/env bash
# [ADICIONADO em 2026-10-28] Compara o som da arvore atual com o de um commit:
#   tools/golden/compare.sh REF        (REF = hash/branch; padrao HEAD)
# Builda tools/golden/golden.cpp nas duas versoes (REF num git worktree em
# build/golden_cmp/ref), renderiza os cenarios e lista o que mudou. Sai 0 se tudo
# ficou identico bit a bit. REF precisa ja ter tools/golden/.
set -euo pipefail
cd "$(dirname "$0")/../.."
REF="${1:-HEAD}"
OUT=build/golden_cmp
WT="$OUT/ref"
mkdir -p "$OUT"
if [ -d "$WT" ]; then git worktree remove --force "$WT"; fi
git worktree add --detach "$WT" "$REF" >/dev/null
trap 'git worktree remove --force "$WT"' EXIT
REPO_ROOT=$(git rev-parse --show-toplevel)
SUB=$(realpath --relative-to="$REPO_ROOT" .)   # dx7fm/ dentro do repositorio
build() { # $1 = pasta dx7fm, $2 = binario
    g++ -std=c++20 -O2 -I"$1/src" -I"$1/app" "$1/tools/golden/golden.cpp" -o "$2" -lpthread
}
build "$WT/$SUB" "$OUT/golden_ref"
build . "$OUT/golden_cur"
rm -rf "$OUT/a" "$OUT/b"
"$OUT/golden_ref" render "$OUT/a" >/dev/null
"$OUT/golden_cur" render "$OUT/b" >/dev/null
echo "A = $REF, B = arvore atual"
"$OUT/golden_cur" diff "$OUT/a" "$OUT/b"
