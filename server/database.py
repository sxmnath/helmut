import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'offenders.db')

def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute('''
        CREATE TABLE IF NOT EXISTS offenders (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            plate      TEXT,
            timestamp  TEXT,
            snapshot   TEXT,
            confidence REAL
        )
    ''')
    conn.commit()
    conn.close()

def insert_violation(plate, timestamp, snapshot_path, confidence):
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT INTO offenders (plate, timestamp, snapshot, confidence) VALUES (?,?,?,?)",
        (plate, timestamp, snapshot_path, confidence)
    )
    conn.commit()
    conn.close()

def get_all_offenders():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT * FROM offenders ORDER BY id DESC LIMIT 200"
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]