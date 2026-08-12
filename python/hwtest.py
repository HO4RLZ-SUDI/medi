"""หน้าเว็บควบคุม/ทดสอบฮาร์ดแวร์ — เซอร์โว, สั่งปัดยา, ตรวจใบหน้าจาก webcam

ผูกเข้ากับ WebUI ที่ main.py สร้างไว้:

    from hwtest import register
    register(ui)

ตรวจใบหน้าใช้ brick arduino:video_object_detection (model: face-detection)
ที่ประกาศไว้ใน app.yaml — brick ยึด USB webcam ไว้เอง และเสิร์ฟภาพพรีวิว
ที่พอร์ต 4912 path /embed (หน้าเว็บฝังด้วย iframe)

เจอหน้า -> แจ้ง MCU ผ่าน Bridge.call("set_face", True) -> MCU เช็คช่วงเวลา
จาก DS1302 แล้วตัดสินใจจ่ายยาเอง
"""

import threading
import time

from arduino.app_utils import *          # App, Bridge

try:
    from arduino.app_bricks.video_objectdetection import VideoObjectDetection
except ImportError:                      # ไม่มี brick -> ส่วนอื่นยังใช้ได้
    VideoObjectDetection = None


CAM_PREVIEW_PORT = 4912                  # brick เสิร์ฟภาพที่พอร์ตนี้
CAM_PREVIEW_PATH = "/embed"
FACE_TIMEOUT_SEC = 2.0                   # ไม่เจอหน้าติดกันเกินนี้ = ถือว่าไม่มีคน

SLOTS = [
    {"idx": 0, "name": "เช้า",     "pin": "D9",  "window": "06:00–08:00"},
    {"idx": 1, "name": "กลางวัน", "pin": "D10", "window": "11:00–13:00"},
    {"idx": 2, "name": "เย็น",     "pin": "D11", "window": "17:00–19:00"},
]

_detector = None
_detector_error = None
_face_present = False
_face_last_seen = 0.0
_face_lock = threading.Lock()


def register(ui, confidence=0.5):
    """ผูก API + socket handler ทั้งหมดเข้ากับอินสแตนซ์ WebUI"""

    global _detector, _detector_error

    # ---------- ตัวช่วยเรียก MCU ----------
    def _call(method, *args):
        """คืน (ok, value_or_error)"""
        try:
            return True, Bridge.call(method, *args)
        except Exception as exc:
            return False, str(exc)

    # ---------- ตรวจใบหน้าจาก webcam ----------
    def _set_face(present):
        """อัปเดตสถานะใบหน้า แล้วดันไปที่ MCU + หน้าเว็บเมื่อมีการเปลี่ยนแปลง"""
        global _face_present
        with _face_lock:
            if _face_present == present:
                return
            _face_present = present
        _call("set_face", present)
        ui.send_message("face_update", {"face": present})

    def _on_face_detected():
        global _face_last_seen
        _face_last_seen = time.time()
        _set_face(True)

    def _face_watchdog():
        """brick ยิง callback เฉพาะตอนเจอ ไม่มี event ตอนคนเดินออก
           จึงต้องมีตัวจับเวลาปิดสถานะเอง"""
        while True:
            time.sleep(0.5)
            if _face_present and (time.time() - _face_last_seen) > FACE_TIMEOUT_SEC:
                _set_face(False)

    if VideoObjectDetection is None:
        _detector_error = "ไม่มี brick video_objectdetection (เช็ค app.yaml)"
    else:
        try:
            _detector = VideoObjectDetection(confidence=confidence, debounce_sec=0.0)
            _detector.on_detect("face", _on_face_detected)
            threading.Thread(target=_face_watchdog, daemon=True).start()
        except Exception as exc:
            _detector_error = f"เริ่มตัวตรวจใบหน้าไม่สำเร็จ: {exc}"

    def api_cam_status():
        return {
            "ok": _detector is not None,
            "port": CAM_PREVIEW_PORT,
            "path": CAM_PREVIEW_PATH,
            "face": _face_present,
            "error": _detector_error,
        }

    ui.expose_api("GET", "/api/cam/status", api_cam_status)

    def on_set_confidence(client, data):
        th = float((data or {}).get("confidence", 0.5))
        if _detector is None:
            ui.send_message("cam_status", {"ok": False, "error": "ตัวตรวจใบหน้าไม่ได้ทำงาน"})
            return
        try:
            _detector.override_threshold(th)
            ui.send_message("cam_status", {"ok": True, "confidence": th})
        except Exception as exc:
            ui.send_message("cam_status", {"ok": False, "error": str(exc)})

    ui.on_message("set_confidence", on_set_confidence)

    def _reply(event, ok, msg, extra=None):
        payload = {"ok": bool(ok), "message": msg}
        if extra:
            payload.update(extra)
        ui.send_message(event, payload)

    # ---------- สั่งปัดยา ----------
    def on_dispense(client, data):
        idx = int((data or {}).get("idx", 0))
        name = SLOTS[idx]["name"] if 0 <= idx < len(SLOTS) else str(idx)

        ok, val = _call("dispense_slot", idx)
        if not ok:
            _reply("dispense_status", False, f"เรียก MCU ไม่สำเร็จ: {val}")
            return

        if val == -1:
            _reply("dispense_status", False, f"ช่องไม่ถูกต้อง ({idx})")
        elif val == -2:
            _reply("dispense_status", False, f"ยาหมดในช่อง{name} — เติมก่อน")
        else:
            _reply("dispense_status", True,
                   f"ปัดยามื้อ{name}แล้ว เหลือ {val} เม็ด",
                   {"idx": idx, "pills": val})
        _push_pills()

    def on_refill(client, data):
        data = data or {}
        idx = int(data.get("idx", 0))
        pills = int(data.get("pills", 5))
        ok, val = _call("refill_slot", idx, pills)
        if not ok or val < 0:
            _reply("dispense_status", False, f"เติมยาไม่สำเร็จ: {val}")
        else:
            name = SLOTS[idx]["name"] if 0 <= idx < len(SLOTS) else str(idx)
            _reply("dispense_status", True, f"เติมยาช่อง{name} เป็น {val} เม็ด")
        _push_pills()

    ui.on_message("dispense", on_dispense)
    ui.on_message("refill", on_refill)

    # ---------- เซอร์โว (ทดสอบกลไก ไม่แตะจำนวนยา) ----------
    def on_servo_write(client, data):
        data = data or {}
        idx = int(data.get("idx", 0))
        angle = int(data.get("angle", 90))
        ok, val = _call("servo_write", idx, angle)
        _reply("servo_status", ok, f"servo {idx} -> {angle}°" if ok
                                   else f"เรียก MCU ไม่สำเร็จ: {val}")

    def on_servo_pulse(client, data):
        idx = int((data or {}).get("idx", 0))
        ok, val = _call("servo_pulse", idx)
        _reply("servo_status", ok, f"ปัด servo {idx} 1 ครั้ง (ไม่นับยา)" if ok
                                   else f"เรียก MCU ไม่สำเร็จ: {val}")

    def on_servo_sweep(client, data):
        """กวาดพัก -> ปัด -> พัก 3 รอบ ดูว่าเดินเต็มช่วงและความเร็วเหมาะไหม"""
        idx = int((data or {}).get("idx", 0))
        cfg = _servo_cfg()[idx] if 0 <= idx < len(SLOTS) else {"home": 0, "push": -30}

        def run():
            for _ in range(3):
                for target in (cfg["push"], cfg["home"]):
                    ok, val = _call("servo_write", idx, target)
                    if not ok:
                        _reply("servo_status", False, f"sweep ล้มเหลว: {val}")
                        return
                    time.sleep(0.1)
            _reply("servo_status", True, f"sweep servo {idx} เสร็จ (3 รอบ)")

        threading.Thread(target=run, daemon=True).start()
        _reply("servo_status", True, f"เริ่ม sweep servo {idx}...")

    def on_servo_home(client, data):
        ok, val = _call("servo_home_all")
        _reply("servo_status", ok, "เซอร์โวทั้งหมดกลับตำแหน่งพัก (180°)" if ok
                                   else f"เรียก MCU ไม่สำเร็จ: {val}")

    ui.on_message("servo_write", on_servo_write)
    ui.on_message("servo_pulse", on_servo_pulse)
    ui.on_message("servo_sweep", on_servo_sweep)
    ui.on_message("servo_home", on_servo_home)

    # ---------- สถานะ ----------
    def _pills():
        out = []
        for s in SLOTS:
            ok, val = _call("get_pills", s["idx"])
            out.append(val if ok and isinstance(val, int) and val >= 0 else None)
        return out

    def _push_pills():
        ui.send_message("pills_update", {"pills": _pills()})

    def _limits():
        """ขอบเขตที่ MCU ยอมรับ ดึงมาเพื่อไม่ต้อง hardcode ซ้ำสองที่"""
        out = {}
        for key, method, fallback in (("angle_min", "get_angle_min", -30),
                                      ("angle_max", "get_angle_max", 180),
                                      ("speed_max", "get_speed_max", 1000)):
            ok, val = _call(method)
            out[key] = val if ok and isinstance(val, int) else fallback
        return out

    def _servo_cfg():
        """ค่าตั้งของเซอร์โวทั้ง 3 ตัว"""
        out = []
        for s in SLOTS:
            cfg = {}
            for key, method, fallback in (("home", "get_servo_home", 0),
                                          ("push", "get_servo_push", -30),
                                          ("speed", "get_servo_speed", 120)):
                ok, val = _call(method, s["idx"])
                cfg[key] = val if ok and isinstance(val, int) and val != -999 else fallback
            out.append(cfg)
        return out

    def on_set_servo_config(client, data):
        data = data or {}
        idx = int(data.get("idx", 0))
        home = int(data.get("home", 0))
        push = int(data.get("push", -30))
        speed = int(data.get("speed", 120))

        ok, val = _call("set_servo_config", idx, home, push, speed)
        if not ok or val is False:
            _reply("servo_status", False, f"บันทึกค่าไม่สำเร็จ: {val}")
        else:
            name = SLOTS[idx]["name"] if 0 <= idx < len(SLOTS) else str(idx)
            _reply("servo_status", True,
                   f"บันทึกช่อง{name}: พัก {home}° · ปัด {push}° · {speed}°/วิ")
        ui.send_message("servo_config_update", {"servos": _servo_cfg()})

    ui.on_message("set_servo_config", on_set_servo_config)

    def api_slots():
        return {
            "slots": SLOTS,
            "pills": _pills(),
            "limits": _limits(),
            "servos": _servo_cfg(),
        }

    def api_sensors():
        out = {}
        errors = []
        for key, method in (("temperature", "get_temperature"),
                            ("humidity", "get_humidity"),
                            ("face", "get_face")):
            ok, val = _call(method)
            if not ok:
                out[key] = None
                errors.append(f"{method}: {val}")
            else:
                # sketch คืน -999 แทน NaN เมื่ออ่าน DHT ไม่ได้
                out[key] = None if isinstance(val, (int, float)) and val == -999 else val
        out["pills"] = _pills()
        if errors:
            out["errors"] = errors
        return out

    ui.expose_api("GET", "/api/slots", api_slots)
    ui.expose_api("GET", "/api/sensors", api_sensors)

    return ui
