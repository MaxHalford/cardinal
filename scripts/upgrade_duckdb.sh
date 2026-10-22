#!/bin/sh
# Moves the repository to a DuckDB release, on a branch, e.g. duckdb-v2.1.0.
#
#   scripts/upgrade_duckdb.sh            the latest release, if newer than the current one
#   scripts/upgrade_duckdb.sh v2.1.0     a given release
#
# Prints the branch name, or nothing when there is nothing to do. Needs gh, logged in.
set -eu

pinned() {
    sed -n 's/.*duckdb_version: \(v[0-9.]*\).*/\1/p' .github/workflows/MainDistributionPipeline.yml | head -1
}

current=$(pinned)
target=${1:-$(gh api repos/duckdb/duckdb/releases/latest --jq .tag_name)}
if [ "$target" = "$current" ]; then
    exit 0
fi
branch="duckdb-$target"
if git ls-remote --exit-code --heads origin "$branch" > /dev/null 2>&1; then
    exit 0
fi

# extension-ci-tools: a branch named after the release, else after its minor version,
# e.g. v1.5.6, or v2.0-cyanoptera for v2.0.0.
minor=$(echo "$target" | sed 's/^\(v[0-9]*\.[0-9]*\).*/\1/')
branches=$(gh api repos/duckdb/extension-ci-tools/branches --paginate --jq '.[].name')
tools=$(echo "$branches" | grep -x "$target" || echo "$branches" | grep "^$minor-" | head -1)
if [ -z "$tools" ]; then
    echo "no extension-ci-tools branch for $target" >&2
    exit 1
fi

git checkout -q -b "$branch"
git submodule update -q --init duckdb extension-ci-tools
git -C duckdb fetch -q --depth 1 origin tag "$target"
git -C duckdb checkout -q "$target"
git -C extension-ci-tools fetch -q origin "$tools"
git -C extension-ci-tools checkout -q FETCH_HEAD

# Version pins: CI, the gate, the research tools, the submodule branch.
version=${target#v}
sed -i.bak "s/duckdb_version: $current/duckdb_version: $target/; s/ci_tools_version: .*/ci_tools_version: $tools/; s|_extension_distribution.yml@.*|_extension_distribution.yml@$tools|" \
    .github/workflows/MainDistributionPipeline.yml
sed -i.bak "s/duckdb==[0-9.]*/duckdb==$version/" scripts/gate.py research/pyproject.toml
sed -i.bak "s/branch = .*/branch = $tools/" .gitmodules
rm -f .github/workflows/*.bak scripts/*.bak research/*.bak .gitmodules.bak

git add -A duckdb extension-ci-tools .github .gitmodules scripts/gate.py research/pyproject.toml
git commit -q -m "Upgrade to DuckDB $target" -m "Moves the duckdb and extension-ci-tools submodules and the version pins. Built and checked by CI on this branch."
echo "$branch"
