#!/bin/bash

MANAGERS="Parf Aparf Rrpaa Prcs MinstrelBlues"
REP=10

job() {
  local OUT=$(NS_GLOBAL_VALUE="RngRun=$2" ./waf --run "power-adaptation-distance --manager=ns3::${1}WifiManager")
  while read -r LINE; do
    echo "$LINE $1"
  done <<< "$OUT"
}
export -f job

for i in $MANAGERS; do
  for j in $(seq 1 $REP); do
    sem -j+0 job $i $j 2>/dev/null
  done
done
sem --wait 2>/dev/null
