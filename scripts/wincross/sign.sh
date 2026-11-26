#!/bin/bash
# <Prestige> Authenticode-sign Windows binaries with Azure Trusted Signing (jsign, runs on Linux).
#   sign.sh <file>...          sign these files (replacing any signature they have)
#   sign.sh --configured       exit 0 when signing is set up, 1 when not
#   sign.sh --unsigned <dir>   list the .exe / .dll under <dir> that carry no signature yet
# Without the PRESTIGE_SIGN_* settings (GitHub secrets, see .github/workflows/prestige-windows.yml)
# it signs nothing and succeeds, so unsigned builds keep working.
#   PRESTIGE_SIGN_TENANT_ID / _CLIENT_ID / _CLIENT_SECRET   the app registration that may sign
#   PRESTIGE_SIGN_ENDPOINT   the account's region endpoint, e.g. eus.codesigning.azure.net
#   PRESTIGE_SIGN_ACCOUNT    the Trusted Signing account name
#   PRESTIGE_SIGN_PROFILE    its certificate profile name
set -euo pipefail

configured() {
    local v n
    for v in TENANT_ID CLIENT_ID CLIENT_SECRET ENDPOINT ACCOUNT PROFILE; do
        n="PRESTIGE_SIGN_$v"
        [ -n "${!n:-}" ] || return 1
    done
}

if [ "${1:-}" = "--configured" ]; then
    configured && exit 0 || exit 1
fi

if [ "${1:-}" = "--unsigned" ]; then
    python3 -I - "$2" <<'EOF'
import os, struct, sys
for root, _, files in os.walk(sys.argv[1]):
    for f in sorted(files):
        if not f.lower().endswith(('.exe', '.dll')):
            continue
        p = os.path.join(root, f)
        try:
            with open(p, 'rb') as fh:
                d = fh.read(4096)
            pe = struct.unpack_from('<I', d, 0x3c)[0]
            magic = struct.unpack_from('<H', d, pe + 24)[0]
            off = pe + 24 + (96 if magic == 0x10b else 112) + 4 * 8   # data directory 4: security
            size = struct.unpack_from('<II', d, off)[1]
        except Exception:
            continue   # not a PE we understand: leave it alone
        if size == 0:
            print(p)
EOF
    exit 0
fi

[ $# -gt 0 ] || exit 0
if ! configured; then
    echo "sign: PRESTIGE_SIGN_* not set: leaving $# file(s) unsigned"
    exit 0
fi

JSIGN_VER=7.1
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/prestige-sign"
mkdir -p "$CACHE"
JAR="$CACHE/jsign-$JSIGN_VER.jar"
if [ ! -s "$JAR" ]; then
    curl -fsSL --retry 3 -o "$JAR.part" "https://github.com/ebourg/jsign/releases/download/$JSIGN_VER/jsign-$JSIGN_VER.jar"
    mv "$JAR.part" "$JAR"
fi

# An access token for the signing service (client credentials), reused for 45 minutes.
TOKEN_FILE="$CACHE/token"
if [ ! -s "$TOKEN_FILE" ] || [ -n "$(find "$TOKEN_FILE" -mmin +45 2>/dev/null)" ]; then
    ( umask 077
      curl -fsS --retry 3 -X POST "https://login.microsoftonline.com/$PRESTIGE_SIGN_TENANT_ID/oauth2/v2.0/token" \
          --data-urlencode "client_id=$PRESTIGE_SIGN_CLIENT_ID" \
          --data-urlencode "client_secret=$PRESTIGE_SIGN_CLIENT_SECRET" \
          --data-urlencode "grant_type=client_credentials" \
          --data-urlencode "scope=https://codesigning.azure.net/.default" \
        | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["access_token"])' > "$TOKEN_FILE" )
fi
JSIGN_TOKEN=$(cat "$TOKEN_FILE")
[ -z "${GITHUB_ACTIONS:-}" ] || echo "::add-mask::$JSIGN_TOKEN"
export JSIGN_TOKEN

endpoint=${PRESTIGE_SIGN_ENDPOINT#https://}
endpoint=${endpoint%%/*}
echo "sign: signing $# file(s) with $PRESTIGE_SIGN_ACCOUNT/$PRESTIGE_SIGN_PROFILE"
java -jar "$JAR" --storetype TRUSTEDSIGNING --keystore "$endpoint" --storepass env:JSIGN_TOKEN \
    --alias "$PRESTIGE_SIGN_ACCOUNT/$PRESTIGE_SIGN_PROFILE" --alg SHA-256 \
    --tsaurl http://timestamp.acs.microsoft.com/ --tsmode RFC3161 --replace "$@"
