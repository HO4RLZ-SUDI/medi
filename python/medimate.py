"""สะพานเชื่อมเว็บแอป Medimate เข้ากับฮาร์ดแวร์ตู้จ่ายยา

โมดูลนี้ "เสริม" ของเดิม ไม่แตะ hwtest.py และไม่แตะ sketch:

  * เฝ้าดูจำนวนยาในแต่ละหลอดผ่าน Bridge แล้วบันทึกประวัติเอง
    -> จับได้ทั้งการปัดยาที่สั่งจากหน้าเว็บ (หน้าไหนก็ได้) และที่ MCU
       ตัดสินใจปัดเองตอนเจอหน้า + ถึงเวลา โดยไม่ต้องแก้โค้ดเดิมสักบรรทัด
  * เก็บชื่อยาประจำหลอด (ซิงก์มาจากหน้า Drug Information ที่อยู่บน Firestore)
    เพื่อให้ประวัติมีชื่อยาจริง ไม่ใช่แค่เลขหลอด
  * เปิด REST ให้หน้า Dispensary History / Dispenser ดึงไปแสดง

ผูกเข้ากับ WebUI ตัวเดียวกับ hwtest:

    from medimate import register as register_medimate
    register_medimate(ui)
"""

import json
import os
import threading
import time
from datetime import datetime
from pathlib import Path

from arduino.app_utils import *          # App, Bridge


# ---------- ที่เก็บข้อมูล ----------
def _data_dir():
    """หาโฟลเดอร์ที่เขียนได้จริงบนบอร์ด (เผื่อ home อ่านอย่างเดียว/เต็ม)"""
    for cand in (os.environ.get("MEDIMATE_DATA"),
                 Path.home() / ".medimate",
                 Path("/tmp/medimate")):
        if not cand:
            continue
        p = Path(cand)
        try:
            p.mkdir(parents=True, exist_ok=True)
            probe = p / ".w"
            probe.write_text("1")
            probe.unlink()
            return p
        except Exception:
            continue
    return Path("/tmp")


DATA_DIR = _data_dir()
HISTORY_FILE = DATA_DIR / "history.json"
TUBES_FILE = DATA_DIR / "tubes.json"

HISTORY_MAX = 500                # เก็บย้อนหลังกี่รายการ
POLL_SEC = 3.0                   # ถี่แค่ไหนถึงจะเช็คจำนวนยา
NOTE_TTL_SEC = 20.0              # "สั่งจากหน้าเว็บ" มีอายุกี่วินาที

TUBE_LABELS = ["ยาหลอดที่ 1", "ยาหลอดที่ 2", "ยาหลอดที่ 3"]
MEAL_LABELS = ["ช่วงเช้า", "ช่วงเที่ยง", "ช่วงเย็น"]

_lock = threading.Lock()
_history = []
_tubes = [{"name": "", "detail": "", "count": 0} for _ in range(3)]
_notes = {}                      # idx -> (timestamp, source)
_started = False


# ---------- อ่าน/เขียนไฟล์ ----------
def _load_json(path, fallback):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception:
        return fallback


def _save_json(path, obj):
    try:
        tmp = Path(str(path) + ".tmp")
        tmp.write_text(json.dumps(obj, ensure_ascii=False), encoding="utf-8")
        tmp.replace(path)
        return True
    except Exception:
        return False


def _load_all():
    global _history, _tubes
    hist = _load_json(HISTORY_FILE, [])
    if isinstance(hist, list):
        _history = hist[-HISTORY_MAX:]
    tubes = _load_json(TUBES_FILE, None)
    if isinstance(tubes, list) and len(tubes) == 3:
        for i, t in enumerate(tubes):
            if isinstance(t, dict):
                _tubes[i].update({k: t.get(k, _tubes[i][k]) for k in _tubes[i]})


def _tube_name(idx):
    name = (_tubes[idx].get("name") or "").strip()
    return name or TUBE_LABELS[idx]


def _add_record(idx, kind, amount, pills_left):
    """บันทึก 1 แถวลงประวัติ แล้วคืน record ที่เพิ่ง add"""
    now = datetime.now()

    source = "อัตโนมัติ (ตู้จ่ายเอง)"
    note = _notes.get(idx)
    if note and (time.time() - note[0]) <= NOTE_TTL_SEC:
        source = note[1]
        _notes.pop(idx, None)

    rec = {
        "ts": now.isoformat(timespec="seconds"),
        "time": now.strftime("%Y-%m-%d %H:%M"),
        "idx": idx,
        "tube": TUBE_LABELS[idx] if 0 <= idx < 3 else f"หลอด {idx}",
        "meal": MEAL_LABELS[idx] if 0 <= idx < 3 else "",
        "drug": _tube_name(idx),
        "kind": kind,                       # dispense | refill
        "amount": amount,
        "pills_left": pills_left,
        "note": source if kind == "dispense" else "เติมยา",
    }

    with _lock:
        _history.append(rec)
        if len(_history) > HISTORY_MAX:
            del _history[:-HISTORY_MAX]
        snapshot = list(_history)
    _save_json(HISTORY_FILE, snapshot)
    return rec


def register(ui):
    """ผูก REST + socket handler ของ Medimate เข้ากับ WebUI ที่ main.py สร้างไว้"""

    global _started

    _load_all()

    def _call(method, *args):
        try:
            return True, Bridge.call(method, *args)
        except Exception as exc:
            return False, str(exc)

    def _read_pills():
        out = []
        for i in range(3):
            ok, val = _call("get_pills", i)
            out.append(val if ok and isinstance(val, int) and val >= 0 else None)
        return out

    # ---------- ตัวเฝ้าดูจำนวนยา ----------
    def _watcher():
        """จำนวนยาลด = ปัดยา, จำนวนยาเพิ่ม = เติมยา
           วิธีนี้จับได้ทุกทาง ไม่ว่าจะสั่งจากหน้าไหนหรือ MCU สั่งเอง"""
        prev = None
        while True:
            time.sleep(POLL_SEC)
            cur = _read_pills()

            if prev is not None:
                changed = False
                for i, (before, after) in enumerate(zip(prev, cur)):
                    if before is None or after is None or before == after:
                        continue
                    if after < before:
                        _add_record(i, "dispense", before - after, after)
                    else:
                        _add_record(i, "refill", after - before, after)
                    changed = True
                if changed:
                    ui.send_message("history_update", {"history": _recent(20)})

            prev = cur

    # ---------- ประวัติ ----------
    def _recent(limit=100):
        with _lock:
            return list(reversed(_history[-limit:]))     # ใหม่สุดขึ้นก่อน

    def api_history():
        return {"history": _recent(HISTORY_MAX), "count": len(_history)}

    ui.expose_api("GET", "/api/history", api_history)

    def on_clear_history(client, data):
        with _lock:
            _history.clear()
        _save_json(HISTORY_FILE, [])
        ui.send_message("history_update", {"history": []})
        ui.send_message("medimate_status", {"ok": True, "message": "ล้างประวัติแล้ว"})

    ui.on_message("mm_clear_history", on_clear_history)

    # ---------- ชื่อยาประจำหลอด ----------
    def api_tubes():
        return {"tubes": _tubes, "labels": TUBE_LABELS, "meals": MEAL_LABELS}

    ui.expose_api("GET", "/api/tubes", api_tubes)

    def on_set_tubes(client, data):
        """หน้าเว็บดึงรายการยาจาก Firestore แล้วส่งมาเก็บไว้ที่บอร์ด
           เพื่อให้ประวัติมีชื่อยาแม้ตอนที่เน็ตล่ม"""
        tubes = (data or {}).get("tubes")
        if not isinstance(tubes, list):
            ui.send_message("medimate_status", {"ok": False, "message": "ข้อมูลหลอดยาไม่ถูกต้อง"})
            return
        for i, t in enumerate(tubes[:3]):
            if not isinstance(t, dict):
                continue
            _tubes[i]["name"] = str(t.get("name", ""))[:60]
            _tubes[i]["detail"] = str(t.get("detail", ""))[:200]
            try:
                _tubes[i]["count"] = int(t.get("count", 0))
            except (TypeError, ValueError):
                _tubes[i]["count"] = 0
        _save_json(TUBES_FILE, _tubes)
        ui.send_message("tubes_update", {"tubes": _tubes})

    ui.on_message("mm_set_tubes", on_set_tubes)

    # ---------- ที่มาของการปัดยา ----------
    def on_note(client, data):
        """หน้าเว็บบอกล่วงหน้าว่ากำลังจะสั่งปัดเอง ตัวเฝ้าดูจะได้ติดป้ายถูก
           (ส่ง mm_note ก่อน แล้วค่อยส่ง dispense ตามปกติ)"""
        data = data or {}
        try:
            idx = int(data.get("idx", 0))
        except (TypeError, ValueError):
            return
        source = str(data.get("source") or "สั่งจากหน้าเว็บ")[:60]
        _notes[idx] = (time.time(), source)

    ui.on_message("mm_note", on_note)

    # ---------- สรุปสถานะรวบยอด (เรียกทีเดียวได้ครบ) ----------
    def api_status():
        out = {"tubes": _tubes, "labels": TUBE_LABELS, "meals": MEAL_LABELS}

        pills = []
        for i in range(3):
            ok, val = _call("get_pills", i)
            pills.append(val if ok and isinstance(val, int) and val >= 0 else None)
        out["pills"] = pills

        for key, method in (("temperature", "get_temperature"),
                            ("humidity", "get_humidity"),
                            ("face", "get_face")):
            ok, val = _call(method)
            if not ok:
                out[key] = None
            else:
                out[key] = None if isinstance(val, (int, float)) and val == -999 else val

        out["history"] = _recent(10)
        out["online"] = any(p is not None for p in pills)
        return out

    ui.expose_api("GET", "/api/medimate/status", api_status)

    # เริ่มตัวเฝ้าดูท้ายสุด เพื่อให้ helper ทุกตัวถูกนิยามครบก่อน
    if not _started:
        _started = True
        threading.Thread(target=_watcher, daemon=True).start()

    return ui
