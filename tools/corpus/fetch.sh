#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Fetch the reading corpus: public OpenSHMEM code bases pinned in manifest.tsv.
#
# Each entry lands in corpus/<name>/ at exactly its pinned commit or tarball
# digest. corpus/ sits at the root of the main checkout, even when this runs
# from a worktree, so every worktree shares one copy; git ignores it. The corpus
# is for reading only: never build it and never copy from it into this
# repository. Traces cite it by repository, commit, and file:line
# (docs/design/workloads/traces/README.md).
#
# Idempotent: an entry already at its pin is left alone; one at another pin is
# moved to the pinned one. Needs network access, git, tar, curl or wget, and a
# SHA-256 tool (sha256sum, shasum, or openssl).
#
# Usage: tools/corpus/fetch.sh [name...]       (default: every entry)
#        OSOE_CORPUS=/some/dir tools/corpus/fetch.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# The main checkout, not the current worktree: worktrees come and go, and all of
# them share one corpus.
common=$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)
repo=$(dirname "$common")
manifest=$here/manifest.tsv
dest=${OSOE_CORPUS:-$repo/corpus}
tab=$(printf '\t')

say() { printf '%s: %s\n' "$name" "$*"; }
fail() { printf '%s: error: %s\n' "$name" "$*" >&2; return 1; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        openssl dgst -sha256 -r "$1" | cut -d' ' -f1
    fi
}

download() { # url file
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# kind git: the whole tree at one commit. kind git-sparse: only the listed
# paths, with blobs fetched on checkout (for very large repositories).
fetch_git() { # dir url sha kind paths
    case $3 in *[!0-9a-f]*) fail "pin must be a full lowercase commit SHA" || return 1 ;; esac
    [ ${#3} -eq 40 ] || { fail "pin must be a full 40-digit commit SHA"; return 1; }

    if [ -d "$1/.git" ] &&
        [ "$(git -C "$1" rev-parse -q --verify 'HEAD^{commit}' 2>/dev/null)" = "$3" ]; then
        say "at $3"
        return 0
    fi

    say "fetching $3"
    [ -d "$1/.git" ] || git init -q "$1" || return 1
    git -C "$1" remote remove origin 2>/dev/null || :
    git -C "$1" remote add origin "$2" || return 1

    if [ "$4" = git-sparse ]; then
        git -C "$1" config core.repositoryformatversion 1 &&
            git -C "$1" config extensions.partialClone origin &&
            git -C "$1" config remote.origin.promisor true &&
            git -C "$1" config remote.origin.partialCloneFilter blob:none &&
            git -C "$1" fetch -q --depth 1 --filter=blob:none origin "$3" || return 1
        # Word splitting on commas is intended: paths contain no spaces.
        old_ifs=$IFS
        IFS=,
        set -- "$1" "$2" "$3" "$4" $5
        IFS=$old_ifs
        dir=$1 sha=$3
        shift 4
        git -C "$dir" sparse-checkout set --no-cone "$@" || return 1
        git -C "$dir" -c advice.detachedHead=false checkout -q --detach "$sha" || return 1
    else
        git -C "$1" fetch -q --depth 1 origin "$3" &&
            git -C "$1" -c advice.detachedHead=false checkout -q --detach "$3" || return 1
    fi
}

fetch_tarball() { # dir url sha256
    case $3 in *[!0-9a-f]*) fail "pin must be a lowercase SHA-256 digest" || return 1 ;; esac
    [ ${#3} -eq 64 ] || { fail "pin must be a 64-digit SHA-256 digest"; return 1; }

    if [ -f "$1/.osoe-pin" ] && [ "$(cat "$1/.osoe-pin")" = "$3" ]; then
        say "at sha256:$3"
        return 0
    fi

    say "downloading $2"
    part=$1.download
    new=$1.new
    rm -rf "$part" "$new"
    download "$2" "$part" || { rm -f "$part"; fail "download failed"; return 1; }
    got=$(sha256 "$part")
    if [ "$got" != "$3" ]; then
        rm -f "$part"
        fail "digest mismatch: expected $3, got $got"
        return 1
    fi
    mkdir "$new" && tar -xzf "$part" -C "$new" || { rm -rf "$part" "$new"; return 1; }
    printf '%s\n' "$3" >"$new/.osoe-pin"
    rm -rf "$1" "$part"
    mv "$new" "$1"
}

selected() { # name: true when no names were given, or when it is one of them
    [ $# -eq 1 ] && return 0
    want=$1
    shift
    for n in "$@"; do [ "$n" = "$want" ] && return 0; done
    return 1
}

mkdir -p "$dest"
failed=
seen=
while IFS=$tab read -r name kind source pin paths <&3; do
    case $name in '' | '#'*) continue ;; esac
    selected "$name" "$@" || continue
    seen="$seen $name"
    case $kind in
    git | git-sparse) fetch_git "$dest/$name" "$source" "$pin" "$kind" "$paths" ;;
    tarball) fetch_tarball "$dest/$name" "$source" "$pin" ;;
    *) fail "unknown kind '$kind'" ;;
    esac || failed="$failed $name"
done 3<"$manifest"

for n in "$@"; do
    case " $seen " in *" $n "*) ;; *)
        printf '%s: error: not in %s\n' "$n" "$manifest" >&2
        failed="$failed $n"
        ;;
    esac
done

if [ -n "$failed" ]; then
    printf 'failed:%s\n' "$failed" >&2
    exit 1
fi
