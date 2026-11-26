#!/bin/sh
# Talk to an ESP-Sim board over Wi-Fi (main/remote.c).
#
#   tools/remote.sh log   [host]   follow the board's log (Ctrl-C to stop)
#   tools/remote.sh flash [host]   send build/esp_sim_dash.bin; the board
#                                  restarts into it, and rolls it back if it
#                                  does not stay up a minute
#
# host: an address or name, or $ESPSIM_HOST.
set -e
cmd=$1
host=${2:-$ESPSIM_HOST}
if [ -z "$cmd" ] || [ -z "$host" ]; then
    echo "usage: $0 log|flash host   (or set ESPSIM_HOST)" >&2
    exit 2
fi
case $cmd in
log)
    since=0
    hdr=$(mktemp)
    trap 'rm -f "$hdr"' EXIT
    while :; do
        if curl -sf -m 10 -D "$hdr" "http://$host/log?since=$since"; then
            next=$(tr -d '\r' <"$hdr" | awk -F': ' 'tolower($1)=="x-log-next"{print $2}')
            if [ -n "$next" ]; then
                [ "$next" -lt "$since" ] && echo "--- the board restarted ---"
                since=$next
            fi
        fi
        sleep 2
    done
    ;;
flash)
    bin=$(dirname "$0")/../build/esp_sim_dash.bin
    [ -f "$bin" ] || { echo "no $bin: build first (idf.py build)" >&2; exit 1; }
    curl -sS --fail-with-body -m 300 -H 'Content-Type: application/octet-stream' \
        --data-binary @"$bin" "http://$host/ota"
    ;;
*)
    echo "usage: $0 log|flash host" >&2
    exit 2
    ;;
esac
