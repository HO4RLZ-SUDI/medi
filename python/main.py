"""Entry point ของแอป — App Lab เรียกไฟล์นี้เสมอ (ชื่อต้องเป็น python/main.py)

หน้าเว็บที่เปิดได้:
  http://<ip-ของบอร์ด>:8001/            หน้าทดสอบ/ตั้งค่าฮาร์ดแวร์
  http://<ip-ของบอร์ด>:8001/medimate/   เว็บแอป Medimate AI
"""

from arduino.app_utils import *              # App, Bridge
from arduino.app_bricks.web_ui import WebUI

from hwtest import register                  # ปัดยา + เทสเซอร์โว + กล้อง
from medimate import register as register_medimate   # ประวัติจ่ายยา + ชื่อยาในหลอด
from aiproxy import register as register_ai          # แชทบอท — บอร์ดถือ token ให้

WEB_PORT = 8001

# WebUI.start() จะโยน RuntimeError ถ้าไม่มี assets/index.html
ui = WebUI(port=WEB_PORT)
register(ui)
register_medimate(ui)
register_ai(ui)

App.run()                                    # บล็อกจนกว่าแอปจะถูกสั่งหยุด
