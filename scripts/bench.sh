#!/bin/sh
# Usage: scripts/bench.sh [server_workers] [seconds]
# Needs wrk. Serves a ~200 B page and a 16 KiB file from a temp directory.
set -e
workers=${1:-4}
secs=${2:-10}
port=18080
bin=$(dirname "$0")/../http-server
root=$(mktemp -d)
trap 'kill $pid 2>/dev/null; rm -rf "$root"' EXIT

head -c 200 /dev/zero | tr '\0' 'x' | sed 's/^/<html><body>/; s/$/<\/body><\/html>/' > "$root/small.html"
head -c 16384 /dev/urandom > "$root/16k.bin"

"$bin" -p $port -w "$workers" "$root" > /dev/null &
pid=$!
sleep 0.5

run() {
    echo "== $2 connections=$1 server_workers=$workers"
    wrk -t$(($1 < 4 ? $1 : 4)) -c"$1" -d"${secs}s" --latency "http://127.0.0.1:$port/$2" |
        grep -E 'Requests/sec|Transfer/sec|50%|99%|Non-2xx|Socket errors'
}

wrk -t4 -c64 -d3s "http://127.0.0.1:$port/small.html" > /dev/null  # warmup
for c in 1 16 64 256; do run $c small.html; done
run 64 16k.bin
