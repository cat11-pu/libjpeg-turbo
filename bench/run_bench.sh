#!/bin/bash
# Runs full-vs-scaled decode memory comparison in clean processes.
set -e
cd "$(dirname "$0")"

gcc -O2 -fPIC -shared mallocpeak.c -o mallocpeak.so -lpthread -ldl
gcc -O2 -I../src -I../build dsbench.c -o dsbench -L../build -ljpeg \
    -Wl,-rpath,"$(cd ../build && pwd)" -lm

for img in "$@"; do
  [ -f "$img" ] || continue
  echo "================ $img ================"
  for mode in full 1/2 1/4 1/8 stream-full stream-1/2 stream-1/4 stream-1/8; do
    out=$(MALLOC_PEAK_REPORT=1 LD_PRELOAD=./mallocpeak.so ./dsbench "$img" 5 "$mode" 2>&1)
    echo "$out" | tr '\n' ' '; echo
  done
done
