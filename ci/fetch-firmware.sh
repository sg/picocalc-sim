#!/bin/sh
# Downloads the Pico 2, Pico 2 W and Pimoroni Plus 2 W UF2s from each firmware project's latest
# release.
#   ci/fetch-firmware.sh API_URL OWNER DEST_DIR
set -eu
api=$1
owner=$2
dest=$3
mkdir -p "$dest"

for repo in pcsh picocalc_lua picocalc-z80 picocalc-ftpserver; do
    curl -sSf "$api/repos/$owner/$repo/releases/latest" |
        python3 -c '
import json, re, sys
for a in json.load(sys.stdin)["assets"]:
    if re.search(r"-(pico2|pico2w|pim2w)-[^-]+\.uf2$", a["name"]):
        print(a["browser_download_url"])' |
        while read -r url; do
            curl -sSfL -o "$dest/${url##*/}" "$url"
            echo "fetched ${url##*/}"
        done
done
