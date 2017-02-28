#!/bin/bash

mkdir -p results
MANAGERS="Parf Rrpaa Prcs MinstrelBlues"
REP=10

job() {
  local OUT=$(NS_LOG=PowerAdaptationDistance=level_info \
	      NS_GLOBAL_VALUE="RngRun=$2" \
	      ./waf --run "power-adaptation-distance --manager=ns3::${1}WifiManager" 2> \
	      >(grep DATA | sed 's/,//g' | awk '{print $1, $6, $8}' > results/${1}-${2}.txt) \
  )
  while read -r LINE; do
    echo "$LINE $1 $2"
  done <<< "$OUT"
}
export -f job

for i in $MANAGERS; do
  for j in $(seq 1 $REP); do
    sem -j+0 job $i $j 2>/dev/null
    #sem -j+0 job ${i}Mod $j 2>/dev/null
  done
done
sem --wait 2>/dev/null
