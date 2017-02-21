#!/bin/bash

OUT="results.dat"
MANAGERS="Parf Aparf Rrpaa Prcs MinstrelBlues"

for i in $MANAGERS; do
  for j in $(seq 1 10); do
    ./waf --run "power-adaptation-distance --manager=ns3::${i}WifiManager" >> $OUT
  done
done
