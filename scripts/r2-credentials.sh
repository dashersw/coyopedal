#!/usr/bin/env bash
# Turns the one R2 API token into the S3 credentials R2 will actually accept.
# Sourced, not run.
#
# Wrangler's `r2 object` commands go through the Cloudflare REST API, and that
# path accepts only an account-wide R2 Admin token: a token scoped to a single
# bucket gets 403 from it, however correct the scope. The S3 API is the half of
# R2 that honours the scope, so the deploy speaks S3 -- and the credential stays
# one that cannot reach past this bucket, which was the whole point of leaving
# Pages behind.
#
# There is still only one secret to store. R2 derives the S3 pair from the token
# itself: the access key id is the token's own id, and the secret access key is
# the SHA-256 of the token value.

if [[ -z "${CLOUDFLARE_ACCOUNT_ID:-}" ]]; then
  echo "CLOUDFLARE_ACCOUNT_ID is not set; put it in the environment or in .env." >&2
  return 1
fi

if [[ -z "${CLOUDFLARE_API_TOKEN:-}" ]]; then
  cat >&2 << 'MSG'
CLOUDFLARE_API_TOKEN is not set.

Make one at R2 -> API -> Manage API tokens: an Account API token, Object Read &
Write, applied to the coyopedal-site bucket only. Then put it in .env, which is
untracked:

  CLOUDFLARE_API_TOKEN=...
  CLOUDFLARE_ACCOUNT_ID=...
MSG
  return 1
fi

for tool in aws jq curl openssl; do
  if ! command -v "$tool" > /dev/null 2>&1; then
    echo "$tool is needed to publish and is not installed." >&2
    return 1
  fi
done

_r2_token_id="$(curl -sS -H "Authorization: Bearer $CLOUDFLARE_API_TOKEN" \
  "https://api.cloudflare.com/client/v4/accounts/$CLOUDFLARE_ACCOUNT_ID/tokens/verify" \
  | jq -r 'if .success then .result.id else empty end')"

if [[ -z "$_r2_token_id" ]]; then
  echo "Cloudflare would not verify CLOUDFLARE_API_TOKEN for this account." >&2
  echo "Either the token is wrong or it belongs to a different account." >&2
  return 1
fi

export AWS_ACCESS_KEY_ID="$_r2_token_id"
export AWS_SECRET_ACCESS_KEY="$(printf %s "$CLOUDFLARE_API_TOKEN" \
  | openssl dgst -sha256 | awk '{print $NF}')"
# R2 is not region-partitioned, but SigV4 insists on signing for one.
export AWS_DEFAULT_REGION=auto
# Recent AWS CLIs add checksum trailers R2 rejects.
export AWS_REQUEST_CHECKSUM_CALCULATION=when_required
export AWS_RESPONSE_CHECKSUM_VALIDATION=when_required
unset _r2_token_id

R2_ENDPOINT="https://$CLOUDFLARE_ACCOUNT_ID.r2.cloudflarestorage.com"
export R2_ENDPOINT
