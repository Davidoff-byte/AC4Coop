#!/bin/bash
# One-off: produced the A/B run table in docs/evidence/climb-moves-2026-11-15.md (paths are the owner's machine)
# abmoves.sh <pid> <tap> <seconds> <modes...>: replay <tap> once per mode with [Coop] NativeReplicaClimbMoves=<mode>, print log ranges
pid=$1; tap=$2; secs=$3; shift 3
G="D:/SteamLibrary/steamapps/common/Assassin's Creed IV Black Flag"
L="$G/phase1_log_$pid.txt"; S="C:/Users/ADMINI~1/AppData/Local/Temp/claude/99b29c06899e5d90/a9873050-163f-4ce2-8854-2150b537f94e/scratchpad"
cd "/c/Users/Administrator/Documents/Default Project/projects/assassins-creed/bf-coop/ref/AC4Coop-lobby"
i=0
for m in "$@"; do
  i=$((i+1))
  sed -i "s/^NativeReplicaClimbMoves=.*/NativeReplicaClimbMoves=$m/" "$G/AC4Coop.ini"
  py -3 tools/pilot.py --pid $pid goto 80 -76 > /dev/null 2>&1
  sleep 3
  a=$(wc -l < "$L")
  timeout $((secs+40)) py -3 tools/wire_tap.py replay "$tap" join R2M4-8S5T-Q74V --player-only --adopt-world --seconds $secs > "$S/ab_$i.txt" 2>&1
  b=$(wc -l < "$L")
  echo "run $i mode=$m lines $a-$b $(tail -1 "$S/ab_$i.txt" | cut -c1-60)"
  py -3 "$S/cisum.py" "$L" $a $b
  grep -a "climb intent" "$L" | tail -n +1 | sed -n '$p' | grep -o "moves on=.*"
  tasklist | grep -qi ac4bfsp || { echo "GAME GONE"; exit 1; }
done
