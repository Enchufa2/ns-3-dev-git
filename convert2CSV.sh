#! /bin/bash
tail -n +2 "$1" > "$1.tmp"
mv "$1.tmp" "$1"

