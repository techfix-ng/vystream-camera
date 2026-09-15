#!/usr/bin/env bash
set -euo pipefail

command -v gh >/dev/null || { echo "GitHub CLI (gh) is required." >&2; exit 1; }
gh auth status >/dev/null

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "Commit and push the workflow files before starting the remote build." >&2
  exit 1
fi

git commit --allow-empty -m "Trigger VyStream 2.9.0 native installer builds"
git push
sleep 5
run_id="$(gh run list --workflow push.yaml --branch "$(git branch --show-current)" --event push --limit 1 --json databaseId --jq '.[0].databaseId')"
[[ -n "$run_id" ]] || { echo "Could not find the dispatched workflow run." >&2; exit 1; }

echo "VyStream native build run: $run_id"
gh run watch "$run_id" --exit-status

output="$repo_root/2.9.0-builds"
mkdir -p "$output"
gh run download "$run_id" --dir "$output"

echo "Builds downloaded to: $output"
find "$output" -maxdepth 3 -type f -print
