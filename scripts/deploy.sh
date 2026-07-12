#!/usr/bin/env bash
# Copy the built module onto the Move. Requires ssh access
# (ableton@move.local, same as schwung's installer). Run scripts/build.sh
# first. After deploying, rescan modules from the schwung UI (or restart
# Move) so the host picks it up. If Mark is already running as an overtake
# tool, FULL-exit it (Shift+Vol+Jog from inside) and relaunch — suspended
# sessions resume the old code.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules

[ -f build/modules/overtake/mark/dsp.so ] || { echo "run scripts/build.sh first"; exit 1; }

ssh "$HOST" "mkdir -p $DEST/overtake"
scp -r build/modules/overtake/mark "$HOST:$DEST/overtake/"
echo "Deployed to $HOST:$DEST — rescan modules or restart Move."
