import os
import json
import queue
import datetime
from flask import Flask, request, jsonify, render_template, Response
from flask_cors import CORS
from dotenv import load_dotenv
from database import init_db, insert_violation, get_all_offenders
from ocr import extract_plate_from_bytes

load_dotenv()

app         = Flask(__name__)
CORS(app)

BASE_DIR     = os.path.dirname(os.path.abspath(__file__))
SNAPSHOT_DIR = os.path.join(BASE_DIR, 'static', 'snapshots')
os.makedirs(SNAPSHOT_DIR, exist_ok=True)

init_db()

# SSE subscriber queues
subscribers: list[queue.Queue] = []

def push_event(data: dict):
    dead = []
    for q in subscribers:
        try:
            q.put_nowait(data)
        except queue.Full:
            dead.append(q)
    for q in dead:
        subscribers.remove(q)


@app.route('/violation', methods=['POST'])
def receive_violation():
    if 'plate_crop' not in request.files:
        return jsonify({"error": "no plate_crop field"}), 400

    plate_file  = request.files['plate_crop']
    confidence  = float(request.form.get('confidence', 0.0))
    image_bytes = plate_file.read()

    # Save snapshot
    ts       = datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    filename = f"{ts}.jpg"
    snap_abs = os.path.join(SNAPSHOT_DIR, filename)
    with open(snap_abs, 'wb') as f:
        f.write(image_bytes)

    # OCR via OCR.space
    plate = extract_plate_from_bytes(image_bytes)

    # Persist
    snap_rel = f"static/snapshots/{filename}"
    insert_violation(plate, ts, snap_rel, confidence)

    # Live push to all open dashboards
    push_event({
        "plate":      plate,
        "timestamp":  ts,
        "snapshot":   f"/{snap_rel}",
        "confidence": round(confidence * 100, 1)
    })

    return jsonify({"plate": plate, "timestamp": ts}), 200


@app.route('/stream')
def stream():
    def event_gen(q):
        try:
            while True:
                data = q.get(timeout=30)
                yield f"data: {json.dumps(data)}\n\n"
        except Exception:
            yield "data: {}\n\n"

    q = queue.Queue(maxsize=50)
    subscribers.append(q)
    return Response(
        event_gen(q),
        mimetype='text/event-stream',
        headers={'X-Accel-Buffering': 'no', 'Cache-Control': 'no-cache'}
    )


@app.route('/')
def dashboard():
    offenders = get_all_offenders()
    return render_template('dashboard.html', offenders=offenders)


@app.route('/api/offenders')
def api_offenders():
    return jsonify(get_all_offenders())


@app.route('/health')
def health():
    return jsonify({"status": "ok"}), 200


if __name__ == '__main__':
    app.run(debug=True, threaded=True, port=5000)