#!/usr/bin/env bash
# Copy a Git working-tree directory without carrying ignored build artifacts.
# Tracked files come from the live working tree, so local edits are preserved.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 SOURCE_DIR DESTINATION_DIR" >&2
    exit 2
fi

requested_source="${1%/}"
requested_destination="${2%/}"
if [ ! -d "$requested_source" ]; then
    echo "Source directory does not exist: $requested_source" >&2
    exit 1
fi
if [ -z "$requested_destination" ]; then
    echo "Refusing an empty destination" >&2
    exit 1
fi
source_dir="$(realpath -e -- "$requested_source")"
destination_dir="$(realpath -m -- "$requested_destination")"
if [ "$source_dir" = "/" ] || [ "$destination_dir" = "/" ]; then
    echo "Refusing to materialize from or replace the filesystem root" >&2
    exit 1
fi

# The destination is recursively replaced below.  Resolve symlinks and reject
# equal or nested trees in either direction before running rm.
case "$destination_dir/" in
    "$source_dir/"|"$source_dir/"*)
        echo "Refusing destination at or below source: $destination_dir" >&2
        exit 1
        ;;
esac
case "$source_dir/" in
    "$destination_dir/"*)
        echo "Refusing destination which contains source: $destination_dir" >&2
        exit 1
        ;;
esac

git_root="$(git -C "$source_dir" rev-parse --show-toplevel)"
git_prefix="$(git -C "$source_dir" rev-parse --show-prefix)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

# Use two manifests so a git failure cannot be hidden by process-substitution
# or pipeline semantics.  --cached keeps locally modified tracked files in the
# list; the subsequent rsync reads their contents from the working tree.
if [ -n "$git_prefix" ]; then
    git -C "$git_root" ls-files -z --cached --others --exclude-standard \
        -- "$git_prefix" > "$tmp_dir/git-files.nul"
else
    git -C "$git_root" ls-files -z --cached --others --exclude-standard \
        > "$tmp_dir/git-files.nul"
fi

while IFS= read -r -d '' path; do
    if [ -n "$git_prefix" ]; then
        case "$path" in
            "$git_prefix"*) path="${path#"$git_prefix"}" ;;
            *)
                echo "Git returned a path outside $source_dir: $path" >&2
                exit 1
                ;;
        esac
    fi
    # A tracked file may be intentionally deleted in the local worktree.
    # Preserve that deletion instead of making rsync fail or copying HEAD.
    if [ -e "$source_dir/$path" ] || [ -L "$source_dir/$path" ]; then
        printf '%s\0' "$path"
    fi
done < "$tmp_dir/git-files.nul" > "$tmp_dir/materialize.nul"

rm -rf -- "$destination_dir"
mkdir -p -- "$destination_dir"
rsync -a --from0 --files-from="$tmp_dir/materialize.nul" \
    "$source_dir/" "$destination_dir/"
