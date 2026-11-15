#!/bin/bash
# One-off: produced the A/B run tables in docs/evidence/climb-moves-2026-11-15.md (paths are the owner's machine)
# abmoves.sh <pid> <tap> <seconds> <modes...>: replay <tap> once per mode; mode = M[:C[:B[:W[:N]]]] sets [Coop] NativeReplicaClimbMoves=M
# (NativeReplicaClimbChoice=C, NativeReplicaBeam=B, NativeReplicaWallEntry=W, NativeReplicaClimbNoSnap=N); prints the log range and summary per run
pid=$1; tap=$2; secs=$3; shift 3
G="D:/SteamLibrary/steamapps/common/Assassin's Creed IV Black Flag"
L="$G/phase1_log_$pid.txt"; S="C:/Users/ADMINI~1/AppData/Local/Temp/claude/99b29c06899e5d90/a9873050-163f-4ce2-8854-2150b537f94e/scratchpad"
cd "/c/Users/Administrator/Documents/Default Project/projects/assassins-creed/bf-coop/ref/AC4Coop-lobby"
i=0
for m in "$@"; do
  i=$((i+1))
  IFS=: read -r mv_ ch_ bm_ we_ ns_ <<< "$m"; ch_=${ch_:-0}; bm_=${bm_:-0}; we_=${we_:-0}; ns_=${ns_:-0}
  sed -i "s/^NativeReplicaClimbMoves=.*/NativeReplicaClimbMoves=$mv_/; s/^NativeReplicaClimbChoice=.*/NativeReplicaClimbChoice=$ch_/; s/^NativeReplicaBeam=.*/NativeReplicaBeam=$bm_/; s/^NativeReplicaWallEntry=.*/NativeReplicaWallEntry=$we_/; s/^NativeReplicaClimbNoSnap=.*/NativeReplicaClimbNoSnap=$ns_/" "$G/AC4Coop.ini"
  py -3 tools/pilot.py --pid $pid goto 80 -76 > /dev/null 2>&1
  sleep 3
  a=$(wc -l < "$L")
  timeout $((secs+40)) py -3 tools/wire_tap.py replay "$tap" join R2M4-8S5T-Q74V --player-only --adopt-world --seconds $secs > "$S/ab_$i.txt" 2>&1
  b=$(wc -l < "$L")
  echo "run $i mode=$m lines $a-$b $(tail -1 "$S/ab_$i.txt" | cut -c1-60)"
  py -3 "$S/cisum.py" "$L" $a $b
  grep -a "climb intent" "$L" | tail -n +1 | sed -n '$p' | grep -o "moves on=.*"
  sed -n "$a,${b}p" "$L" | grep -a "beam steer\|wall entry: starts" | tail -2
  py -3 "$S/beamalone.py" "$L" $a $b
  tasklist | grep -qi ac4bfsp || { echo "GAME GONE"; exit 1; }
done
