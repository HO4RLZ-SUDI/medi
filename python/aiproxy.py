"""พร็อกซีคุยกับ Hugging Face Inference โดยให้บอร์ดเป็นคนถือ token

ทำไมต้องมี:
    เดิมหน้า chatbot.html ยิงไปหา API ตรง ๆ จากเบราว์เซอร์ แปลว่า token
    ต้องอยู่ในหน้าเว็บ ใครกด View Source ก็ก็อปไปใช้ได้ ย้ายมาไว้ที่บอร์ด
    แทน หน้าเว็บก็แค่ส่งข้อความมาให้บอร์ดยิงต่อ ไม่เคยเห็น token เลย

token อ่านจาก (เอาอันแรกที่เจอ):
    1. environment variable HF_TOKEN
    2. ไฟล์ <DATA_DIR>/hf_token   (DATA_DIR ตัวเดียวกับ medimate.py)

ตั้ง token ได้ 2 ทาง — ทาง SSH:
    echo -n 'hf_xxx' > ~/.medimate/hf_token && chmod 600 ~/.medimate/hf_token
หรือส่ง socket event "chat_set_token" มาจากหน้าเว็บครั้งเดียว

โมดูลนี้ไม่แตะ hwtest.py / medimate.py / sketch ผูกเข้ากับ WebUI ตัวเดียวกัน:

    from aiproxy import register as register_ai
    register_ai(ui)
"""

import json
import os
import threading
import urllib.error
import urllib.request

from medimate import DATA_DIR

TOKEN_FILE = DATA_DIR / "hf_token"

DEFAULT_ENDPOINT = os.environ.get(
    "HF_ENDPOINT", "https://router.huggingface.co/v1/chat/completions")
DEFAULT_MODEL = os.environ.get(
    "HF_MODEL", "meta-llama/Llama-3.3-70B-Instruct")

TIMEOUT_SEC = 60.0
MAX_MESSAGES = 24                # กันไม่ให้ประวัติแชทยาวจนโดนปฏิเสธ
MAX_CHARS = 8000                 # ความยาวรวมของข้อความที่ยอมส่งออกไป

_lock = threading.Lock()


# ---------- token ----------
def _read_token():
    tok = (os.environ.get("HF_TOKEN") or "").strip()
    if tok:
        return tok
    try:
        return TOKEN_FILE.read_text(encoding="utf-8").strip()
    except Exception:
        return ""


def _write_token(tok):
    """เก็บ token ลงไฟล์แบบอ่านได้เฉพาะเจ้าของ"""
    with _lock:
        TOKEN_FILE.write_text(tok.strip(), encoding="utf-8")
        try:
            os.chmod(TOKEN_FILE, 0o600)
        except Exception:
            pass          # บาง filesystem ตั้ง mode ไม่ได้ ไม่ใช่เรื่องคอขาดบาดตาย


def _has_token():
    return bool(_read_token())


# ---------- เรียก HF ----------
def _sanitize(messages):
    """รับเฉพาะรูปแบบที่คาดไว้ ตัดส่วนเกิน แล้วคืน list ที่ปลอดภัยจะส่งออก"""
    out = []
    total = 0
    for m in (messages or [])[-MAX_MESSAGES:]:
        if not isinstance(m, dict):
            continue
        role = m.get("role")
        content = m.get("content")
        if role not in ("system", "user", "assistant") or not isinstance(content, str):
            continue
        content = content.strip()
        if not content:
            continue
        total += len(content)
        if total > MAX_CHARS:
            break
        out.append({"role": role, "content": content})
    return out


def _ask(messages, model=None, max_tokens=600, temperature=0.6):
    """ยิงคำถามไป HF แล้วคืน (ok, ข้อความตอบ / ข้อความ error)"""
    token = _read_token()
    if not token:
        return False, "บอร์ดยังไม่มี token ของ Hugging Face"

    msgs = _sanitize(messages)
    if not msgs:
        return False, "ไม่มีข้อความที่จะส่ง"

    body = json.dumps({
        "model": model or DEFAULT_MODEL,
        "messages": msgs,
        "max_tokens": int(max_tokens),
        "temperature": float(temperature),
    }).encode("utf-8")

    req = urllib.request.Request(
        DEFAULT_ENDPOINT,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + token,
        },
    )

    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SEC) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = exc.read().decode("utf-8", "replace")[:300]
        except Exception:
            pass
        # ไม่ส่ง header กลับไป กัน token หลุดผ่านข้อความ error
        return False, f"HF ตอบกลับ HTTP {exc.code} {detail}".strip()
    except urllib.error.URLError as exc:
        return False, f"ต่อ HF ไม่ได้: {exc.reason}"
    except Exception as exc:
        return False, f"เรียก HF ไม่สำเร็จ: {exc}"

    choices = data.get("choices") or []
    if choices:
        msg = (choices[0] or {}).get("message") or {}
        reply = (msg.get("content") or "").strip()
        if reply:
            return True, reply
    err = (data.get("error") or {})
    if isinstance(err, dict):
        err = err.get("message") or ""
    return False, str(err) or "HF ไม่ได้ส่งคำตอบกลับมา"


# ---------- ผูกเข้ากับ WebUI ----------
def register(ui):
    def _config_payload():
        return {
            "configured": _has_token(),
            "model": DEFAULT_MODEL,
            "endpoint": DEFAULT_ENDPOINT,
        }

    def api_status():
        # ไม่มี token อยู่ใน payload มีแค่ว่า "ตั้งไว้แล้วหรือยัง"
        return _config_payload()

    ui.expose_api("GET", "/api/chat/status", api_status)

    def on_status(client, data):
        ui.send_message("chat_config", _config_payload())

    def on_set_token(client, data):
        tok = str((data or {}).get("token", "")).strip()
        if not tok:
            ui.send_message("chat_config",
                            dict(_config_payload(), ok=False,
                                 message="token ว่าง"))
            return
        try:
            _write_token(tok)
        except Exception as exc:
            ui.send_message("chat_config",
                            dict(_config_payload(), ok=False,
                                 message=f"เก็บ token ไม่ได้: {exc}"))
            return
        ui.send_message("chat_config",
                        dict(_config_payload(), ok=True,
                             message="เก็บ token ไว้ที่บอร์ดแล้ว"))

    def on_ask(client, data):
        data = data or {}
        req_id = data.get("id")
        messages = data.get("messages")
        model = data.get("model")

        # ยิง HTTP ในเธรดแยก ไม่งั้น socket ค้างทั้งเส้นระหว่างรอ HF ตอบ
        def work():
            ok, reply = _ask(messages, model=model)
            ui.send_message("chat_reply", {
                "id": req_id,
                "ok": ok,
                "reply": reply if ok else "",
                "error": "" if ok else reply,
            })

        threading.Thread(target=work, daemon=True).start()

    ui.on_message("chat_status", on_status)
    ui.on_message("chat_set_token", on_set_token)
    ui.on_message("chat_ask", on_ask)

    return ui
