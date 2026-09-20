#!/usr/bin/env bash
set -euo pipefail

# Publishes the panel to the R2 bucket behind coyopedal.playtaurus.com.
#
# web/ is the app's SOURCE directory -- it holds the C++ the module is built
# from as well as the page that loads it -- so the site is staged rather than
# uploaded from there: index.html, the _headers file and dist/ (the module, and
# the two factory images the page installs into the board), and nothing else.
#
# The pedal runs on an AudioWorklet over shared memory and a browser only hands
# out SharedArrayBuffer to a cross-origin-isolated document, so without COOP and
# COEP the page loads, the panel draws, and the audio thread never starts. R2
# serves only the handful of headers an object carries as metadata, and those
# two are not among them, so they come from a Response Header Transform Rule on
# the zone instead -- configured once, in the dashboard, where nothing CI holds
# a credential for can reach them. web/_headers is kept as the record of what
# that rule has to say, and is not uploaded.
#
# Cache-Control is per-object, so it is set here rather than in a rule.

# With --stage it stops after staging, which is what CI does to keep the exact
# bytes of a pull request's site downloadable without giving the runner a token.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SITE="$REPO/build/site"
BUCKET="${CLOUDFLARE_R2_BUCKET:-coyopedal-site}"
STAGE_ONLY=0
if [[ "${1:-}" == "--stage" ]]; then
  STAGE_ONLY=1
fi

# Which Cloudflare account and bucket to publish into is deployment
# configuration, not a property of the pedal, so it is read rather than written
# down here: from the environment, or from a .env the repo never tracks. CI
# passes them as secrets alongside CLOUDFLARE_API_TOKEN.
#
# That token is an R2 API token scoped to this one bucket -- R2 can do that,
# where a Pages token is account-wide and can write every project in it. It is
# required: a scoped token is refused by the REST API wrangler uses, so the
# upload goes over S3 and there is no wrangler-login path to fall back to.
if [[ -f "$REPO/.env" ]]; then
  # shellcheck disable=SC1091
  set -a && source "$REPO/.env" && set +a
fi

if [[ ! -f "$REPO/web/dist/module.wasm" ]]; then
  echo "No web/dist/module.wasm; run scripts/build-web-wasm.sh first." >&2
  exit 1
fi

rm -rf "$SITE"
mkdir -p "$SITE"
cp "$REPO/web/index.html" "$SITE/index.html"
cp "$REPO/web/_headers" "$SITE/_headers"
cp "$REPO/web/og.png" "$SITE/og.png"
cp -R "$REPO/web/dist" "$SITE/dist"

echo "Staged $(find "$SITE" -type f | wc -l | tr -d ' ') files in build/site:"
find "$SITE" -type f | sed "s|$SITE|  build/site|"

if ((STAGE_ONLY)); then
  exit 0
fi

# The token becomes an S3 credential here; see the comment in that file for why
# the upload does not go through wrangler.
# shellcheck disable=SC1091
source "$REPO/scripts/r2-credentials.sh" || exit 1

content_type() {
  case "$1" in
    *.html) echo "text/html; charset=utf-8" ;;
    *.js) echo "text/javascript; charset=utf-8" ;;
    # The browser refuses to stream-compile a module served as anything else,
    # and falls back to buffering it, or to failing outright.
    *.wasm) echo "application/wasm" ;;
    *.json) echo "application/json" ;;
    *.png) echo "image/png" ;;
    *) echo "application/octet-stream" ;;
  esac
}

# Everything under dist/ is asked for with a version on the URL that is the hash
# of its own bytes, so a given URL can never mean two different files, which is
# what immutable is for. index.html is what names those versions, so it is the
# one file that has to be revalidated.
cache_control() {
  case "$1" in
    dist/*) echo "public, max-age=31536000, immutable" ;;
    *) echo "public, max-age=0, must-revalidate" ;;
  esac
}

uploaded=0
while IFS= read -r file; do
  key="${file#"$SITE"/}"
  # Pages read this; R2 cannot, and a stray copy of it on the site would only
  # mislead whoever found it.
  [[ "$key" == "_headers" ]] && continue
  echo "  $key"
  aws s3api put-object --endpoint-url "$R2_ENDPOINT" \
    --bucket "$BUCKET" --key "$key" \
    --body "$file" \
    --content-type "$(content_type "$key")" \
    --cache-control "$(cache_control "$key")" > /dev/null
  uploaded=$((uploaded + 1))
done < <(find "$SITE" -type f | sort)

echo "Uploaded $uploaded objects to r2://$BUCKET."
# Objects the site no longer references are left where they are. Every name here
# is fixed -- index.html, og.png, dist/module.{js,wasm} and the two factory
# images -- so a deploy overwrites rather than accumulates.
