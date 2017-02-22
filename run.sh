#!/bin/bash

MANAGERS="Parf Aparf Rrpaa Prcs MinstrelBlues"
REP=3

job() {
  local OUT=$(./waf --run "power-adaptation-distance --manager=ns3::${1}WifiManager")
  echo "$OUT $1"
}
export -f job

for i in $MANAGERS; do
  for j in $(seq 1 $REP); do
    sem -j+0 job $i 2>/dev/null
  done
done
sem --wait 2>/dev/null
