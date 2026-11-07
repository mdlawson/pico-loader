#!/bin/bash
. "$(dirname "$0")/env.sh"
# scrsweep.sh LIST OUTBASE PICO groups... : one strict run per scrambled register group
LIST=$1; OUT=$2; P=$3; shift 3
for g in "$@"; do RTS_SCRAMBLE=$g RTS_AFTER="30:A:4,70:A:4" PICO=$P ./rtsall.sh "$LIST" "$OUT/$g" > /dev/null 2>&1; done
printf "%-8s" game; for g in "$@"; do printf "%8s" "$g"; done; echo
for d in "$OUT/$1"/*/; do n=$(basename "$d"); printf "%-8s" "$n"; for g in "$@"; do v=$(grep -h '^screens' "$OUT/$g/$n/report.txt" 2>/dev/null | sed 's/screens differing: \([0-9]*\).*/\1/'); printf "%8s" "${v:-?}"; done; echo; done
