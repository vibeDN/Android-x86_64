#!/bin/bash
# Pause ninja when RAM is nearly gone, resume when it frees up. Exits when the build ends.
echo 500 > /proc/self/oom_score_adj
paused=0
# wait up to 30 min for ninja to appear (Soong analysis comes first)
for i in $(seq 1 600); do pgrep -x ninja >/dev/null && break; sleep 3; done
while N=$(pgrep -x ninja | head -1); [ -n "$N" ]; do
  a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  if [ $paused = 0 ] && [ "$a" -lt 800 ]; then kill -STOP $N; paused=1; echo "$(date +%T) pause avail=${a}MB"; fi
  if [ $paused = 1 ] && [ "$a" -gt 3000 ]; then kill -CONT $N; paused=0; echo "$(date +%T) resume avail=${a}MB"; fi
  sleep 3
done
[ $paused = 1 ] && kill -CONT "$N" 2>/dev/null
echo "$(date +%T) build ended, guard exit"
