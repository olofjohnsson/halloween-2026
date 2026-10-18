#!/bin/sh
# Starts the counter server. Set API_KEY (and optionally PORT) before running.
cd "$(dirname "$0")" || exit 1
[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install -r requirements.txt; }
export API_KEY="${API_KEY:-change-me}"
exec .venv/bin/python app.py
