#!/usr/bin/env bash
# Brings TrevorbiltKit, the launcher every Trevorbilt Vision Pro port shares, into
# visionos/TrevorbiltKit at one commit of a PUBLIC repository, and records which in
# TrevorbiltKit/VERSION. The kit isn't edited here: changes go to its own repository and come back
# through this script.
#
#   visionos/scripts/sync-trevorbilt-kit.sh [<checkout or URL>] [<commit>]
#
# Defaults: the SHAR port's public release (the kit's home until it has a repository of its own) at
# the commit pinned below. The kit comes from the commit itself (git archive), never a working
# tree, so nothing uncommitted comes along; and only from an allowed public repository, so VERSION
# always names a commit anyone can check out.
set -euo pipefail

default_source="https://github.com/edgytoast/shar-visionos.git"
default_commit="209543f71a7f004958e1bcb2294d56e898fb301d"
# Where the kit may come from (lower case, no .git): its public homes, now and to come.
public_re='^https://github\.com/edgytoast/(shar-visionos|trevorbilt-kit)$'

source="${1:-$default_source}"
commit="${2:-$default_commit}"
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
target="$root/visionos/TrevorbiltKit"

normalise() { printf '%s' "$1" | sed -E 's#://[^/@]*@#://#; s#^git@github\.com:#https://github.com/#; s#\.git$##; s#/$##' |
    tr '[:upper:]' '[:lower:]'; }

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
if [ -d "$source" ]; then
    checkout="$source"
    origin="$(git -C "$checkout" remote get-url origin 2>/dev/null || true)"
    commit="$(git -C "$checkout" rev-parse --verify "$commit^{commit}")"
    # A commit only this checkout has isn't public yet.
    git -C "$checkout" branch -r --contains "$commit" 2>/dev/null | grep -q . ||
        { echo "$commit isn't on any remote branch of $checkout: push it to the public repository first" >&2; exit 1; }
else
    origin="$source"
    checkout="$staging/source"
    git init -q "$checkout"
    git -C "$checkout" fetch -q --depth 1 "$source" "$commit"
    commit="$(git -C "$checkout" rev-parse --verify "FETCH_HEAD^{commit}")"
fi
origin="$(normalise "$origin")"
[[ "$origin" =~ $public_re ]] ||
    { echo "TrevorbiltKit comes only from its public repository, not ${origin:-a checkout without origin}" >&2; exit 1; }

# Where the kit lives in that repository: visionos/TrevorbiltKit in a port's, the root of its own.
if git -C "$checkout" cat-file -e "$commit:visionos/TrevorbiltKit/Package.swift" 2>/dev/null; then
    prefix="visionos/TrevorbiltKit"
elif git -C "$checkout" cat-file -e "$commit:Package.swift" 2>/dev/null; then
    prefix="."
else
    echo "No TrevorbiltKit at $commit in $origin" >&2
    exit 1
fi

mkdir -p "$staging/tree" "$target"
git -C "$checkout" archive "$commit" "$prefix" | tar -x -C "$staging/tree"
rsync -a --delete --exclude VERSION "$staging/tree/$prefix/" "$target/"
{
    echo "TrevorbiltKit, vendored by visionos/scripts/sync-trevorbilt-kit.sh; edit it in its own repository."
    echo "repository: $origin.git"
    echo "path: $prefix"
    echo "commit: $commit"
    if [ "$prefix" != "." ]; then
        echo "(TrevorbiltKit has no repository of its own yet: this is the port's repository it came from.)"
    fi
} > "$target/VERSION"
echo "TrevorbiltKit at ${commit:0:10} ($origin) is in visionos/TrevorbiltKit."
