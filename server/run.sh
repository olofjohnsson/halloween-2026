#!/bin/sh
# Starts the counter server, stopping any previous instance first.
# Set API_KEY (and optionally PORT) before running.
cd "$(dirname "$0")" || exit 1
if [ -f server.pid ]; then
  kill "$(cat server.pid)" 2>/dev/null
  sleep 1
fi
[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install -r requirements.txt; }
export API_KEY="${API_KEY:-change-me}"
echo $$ > server.pid
exec .venv/bin/python app.py
