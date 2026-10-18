import json
import os
import threading

from flask import Flask, jsonify, request, send_from_directory

API_KEY = os.environ.get("API_KEY", "")
DATA_FILE = os.environ.get(
    "DATA_FILE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "counts.json")
)

app = Flask(__name__, static_folder="static")
lock = threading.Lock()


def load():
    try:
        with open(DATA_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"count": 0, "last": None, "seen": []}


def save(state):
    os.makedirs(os.path.dirname(DATA_FILE), exist_ok=True)
    tmp = DATA_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, DATA_FILE)


@app.post("/api/dispense")
def dispense():
    if not API_KEY or request.headers.get("X-API-Key") != API_KEY:
        return jsonify(error="unauthorized"), 401
    body = request.get_json(silent=True) or {}
    try:
        key = f"{int(body['boot'])}:{int(body['seq'])}"
    except (KeyError, TypeError, ValueError):
        return jsonify(error="bad request"), 400
    with lock:
        state = load()
        if key not in state["seen"]:
            state["count"] += 1
            state["seen"] = (state["seen"] + [key])[-200:]
            state["last"] = __import__("datetime").datetime.now().isoformat(timespec="seconds")
            save(state)
        return jsonify(count=state["count"])


@app.get("/api/count")
def count():
    with lock:
        state = load()
    return jsonify(count=state["count"], last=state["last"])


@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


if __name__ == "__main__":
    from waitress import serve

    serve(app, host="0.0.0.0", port=int(os.environ.get("PORT", "8080")), threads=4)
