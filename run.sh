#!/bin/bash

FILE="results.dat"
MANAGERS="Parf Aparf Rrpaa Prcs MinstrelBlues"

for i in $MANAGERS; do
  for j in $(seq 1 10); do
    OUT=$(./waf --run "power-adaptation-distance --manager=ns3::${i}WifiManager")
    echo "$OUT $i" >> $FILE
  done
done
