#!/bin/sh
set -eu

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

tag=${name:-}
if [ -z "$tag" ] || [ "$(printf '%s\n' "$tag" | LC_ALL=C grep -Ex 'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)')" != "$tag" ]; then
    die 'Usage: make release name=v0.1.0'
fi
command -v gh >/dev/null || die 'Install GitHub CLI and run gh auth login.'
git cat-file -e HEAD:tools/build-release.sh 2>/dev/null ||
    die 'The release commit must include tools/build-release.sh.'

repo=mattmezza/launch
commit=$(git rev-parse HEAD)
gh api "repos/$repo/commits/$commit" --silent ||
    die 'Push the release commit to GitHub before creating its release.'
gh workflow view release.yml --repo "$repo" --ref main --yaml >/dev/null ||
    die 'The release workflow must be available on main.'

existing=$(gh release view "$tag" --repo "$repo" --json isDraft,targetCommitish \
    --jq '[.isDraft, .targetCommitish] | @tsv' 2>/dev/null || :)
case "$existing" in
    "true	$commit") ;;
    '')
        refs=$(gh api "repos/$repo/git/matching-refs/tags/$tag" --jq '.[].ref')
        if printf '%s\n' "$refs" | grep -Fxq "refs/tags/$tag"; then
            die "The tag $tag already exists on GitHub."
        fi
        gh release create "$tag" --repo "$repo" --draft --target "$commit" \
            --title "$tag" --generate-notes \
            --notes "Static Linux executables and SHA-256 checksums are attached below. See the relink archives for library sources, licenses, and Launch object files."
        ;;
    *) die "Release $tag already exists or its draft targets a different commit." ;;
esac

gh workflow run release.yml --repo "$repo" --ref main -f "tag=$tag"
