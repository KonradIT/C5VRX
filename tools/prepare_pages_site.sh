#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/pages-site}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
: "${GH_TOKEN:?GH_TOKEN is required}"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/firmware"
cp -a "${ROOT_DIR}/web/." "${OUT_DIR}/"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

echo "Fetching GitHub releases for Pages firmware mirror..."
gh api --paginate --slurp "repos/${REPO}/releases?per_page=100" \
  | jq 'add // []' > "${tmp_dir}/all-releases.json"

# Keep a bounded history of normal firmware plus recent PR prereleases.
# Legacy archive releases are intentionally excluded.
jq '
  [ .[] | select(.draft == false) ] as $all
  | ($all
      | map(select(.tag_name | test("^v[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")))
      | sort_by(.published_at)
      | reverse
      | .[:20]) as $versions
  | ($all
      | map(select(.prerelease == true and (.tag_name | test("^pr-[0-9]+$"))))
      | sort_by(.published_at)
      | reverse
      | .[:3]) as $prs
  | ($all
      | map(select(.prerelease == true and (.tag_name | test("^c5vrx4-(alpha|pr-[0-9]+)$"))))
      | sort_by(.published_at) | reverse) as $alphas
  | ($versions + $prs + $alphas)
' "${tmp_dir}/all-releases.json" > "${tmp_dir}/selected-releases.json"

count="$(jq 'length' "${tmp_dir}/selected-releases.json")"
echo "Mirroring ${count} firmware release(s) into GitHub Pages artifact..."

while IFS= read -r tag; do
  [[ -n "${tag}" ]] || continue
  dest="${OUT_DIR}/firmware/${tag}"
  mkdir -p "${dest}"
  if ! gh release download "${tag}" --repo "${REPO}" --dir "${dest}" --clobber; then
    echo "Warning: could not download release ${tag} (skipping to prevent rate limit failure)" >&2
    rm -rf "${dest}"
    jq --arg tag "${tag}" 'map(select(.tag_name != $tag))' \
      "${tmp_dir}/selected-releases.json" > "${tmp_dir}/remaining-releases.json"
    mv "${tmp_dir}/remaining-releases.json" "${tmp_dir}/selected-releases.json"
  fi
done < <(jq -r '.[].tag_name' "${tmp_dir}/selected-releases.json")

# Preserve GitHub release metadata, but add a same-origin local_url for every
# asset. web/app.js always prefers this URL in production.
jq '
  [ .[]
    | . as $release
    | .assets |= map(. + {
        local_url: ("firmware/" + $release.tag_name + "/" + .name)
      })
  ]
' "${tmp_dir}/selected-releases.json" > "${OUT_DIR}/firmware/releases.json"

echo "Pages site prepared at ${OUT_DIR}"
du -sh "${OUT_DIR}"
