#!/bin/bash
# Tuning sweep: runs the harness across (K, STRIDE, SEND_EARLY, delay)
# combos on both profiles and prints one compact table.
#
#   ./sweep.sh "2 3" "1 2" "10" "80 90 100"
#         K     STRIDE  EARLY  DELAYS

KS=${1:-"2"}
STRIDES=${2:-"1"}
EARLIES=${3:-"0"}   # unused: receiver no longer paces output
DELAYS=${4:-"100"}
PROFILES=${5:-"A B"}

printf "%-3s %-6s %-5s %-6s %-3s %-8s %-8s %s\n" \
       K STRIDE EARLY DELAY PRF MISS% OVERHEAD RESULT
echo "---------------------------------------------------------------"

for k in $KS; do
for s in $STRIDES; do
for e in $EARLIES; do
for d in $DELAYS; do
for p in $PROFILES; do
    out=$(FEC_K=$k FEC_STRIDE=$s SEND_EARLY_MS=$e \
          python3 run.py --profile profiles/$p.json --delay_ms $d 2>&1)

    miss=$(echo "$out" | grep "deadline misses" | sed 's/.*(\(.*\)%).*/\1/')
    ovh=$(echo  "$out" | grep "bandwidth overhead" | sed 's/.*: *\([0-9.]*\)x.*/\1/')
    res=$(echo  "$out" | grep "RESULT" | awk '{print $3}')
    [ -z "$res" ] && res="ERR"

    printf "%-3s %-6s %-5s %-6s %-3s %-8s %-8s %s\n" \
           "$k" "$s" "$e" "$d" "$p" "$miss" "$ovh" "$res"
done; done; done; done; done
