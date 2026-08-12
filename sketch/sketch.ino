/* ============================================================
 *   ตู้จ่ายยาอัตโนมัติ + จดจำใบหน้า  —  Arduino UNO Q
 *   MCU: STM32U585 / ArduinoCore-zephyr (arduino:zephyr:unoq)
 * ============================================================ */

/* ไลบรารีภายนอกที่ยังใช้: มากับแพลตฟอร์ม arduino:zephyr อยู่แล้ว
   จึงไม่โดน sketch.yaml แบบ profile บัง — ไม่ต้องลงอะไรเพิ่มเลย
   ส่วน Servo / DHT / HUSKYLENS เขียนเองไว้ในโฟลเดอร์สเก็ตช์ */
#include "Arduino_RouterBridge.h"

#include "SoftServo.h"
#include "DHT11Lite.h"

/* ---------- ตรวจจับใบหน้า ----------
 * ทำที่ฝั่ง Linux ด้วย webcam + brick video_object_detection (model: face-detection)
 * แล้วส่งผลมาที่ MCU ผ่าน Bridge.call("set_face", true/false)
 * MCU ไม่ได้ต่อกล้องเอง จึงไม่ต้องใช้ I2C/HuskyLens อีก */
bool faceFromHost = false;

/* ---------- Servo ----------
 * ตำแหน่งพัก 0° ปัดยา -30° (มุมติดลบ = ปัดสวนทางกับทิศบวก)
 *
 * SoftServo map ช่วง SERVO_ANGLE_MIN..MAX ลงบนพัลส์ 544–2400 µs แบบเชิงเส้น
 * ดังนั้น -30° = 544 µs, 180° = 2400 µs, และ 0° = 809 µs
 * ระยะเดินจาก 0 ไป -30 จึงเป็น 265 µs (~26° จริง)
 *
 * ทำแบบนี้เพราะถ้า map ตรง ๆ ที่ 0° = 544 µs มุม -30° จะได้พัลส์ ~235 µs
 * ซึ่งต่ำกว่าที่เซอร์โวทั่วไปรับได้ (ต่ำสุด ~500 µs) เซอร์โวจะกระตุกหรือชนสุด
 *
 * ถ้าอยากให้ 1 หน่วย = 1 องศาจริง ให้จูน setPulseRange() ในภายหลัง */
SoftServo servos[3];
const uint8_t servoPins[3] = { 9, 10, 11 };

/* ขอบเขตทางกลไก ใช้ร่วมกันทั้ง 3 ตัว */
const int SERVO_ANGLE_MIN = -30;
const int SERVO_ANGLE_MAX = 180;
const int SERVO_SPEED_MAX = 1000;  // องศา/วินาที

/* ค่าตั้งแยกรายตัว — ปรับจากหน้าเว็บได้ (ไม่ค้างหลังรีบูต MCU)
   speed = องศา/วินาที, 0 = กระโดดไปทันที */
struct ServoCfg { int home; int push; uint16_t speed; };
ServoCfg servoCfg[3] = {
  { 0, -30, 120 },   // ช่อง 1 — เช้า    (D9)
  { 0, -30, 120 },   // ช่อง 2 — กลางวัน (D10)
  { 0, -30, 120 },   // ช่อง 3 — เย็น    (D11)
};

/* เวลาค้างที่แต่ละตำแหน่ง (ms) — ค้างที่ตำแหน่งปัด 0.5 วิ ให้ยาหล่นทัน */
const uint16_t PUSH_HOLD_MS = 500;
const uint16_t HOME_HOLD_MS = 500;

/* ---------- DHT11 ---------- */
#define DHT_PIN 8
DHT11Lite dht(DHT_PIN);

/* ---------- DS1302 (bit-bang เอง ไม่พึ่งไลบรารี) ---------- */
const uint8_t RTC_RST = 5;         // CE
const uint8_t RTC_DAT = 6;         // I/O
const uint8_t RTC_CLK = 7;         // SCLK

/* ตั้งเป็น 1 -> อัปโหลด 1 ครั้งเพื่อเขียนเวลาลง RTC
   แล้วเปลี่ยนกลับเป็น 0 -> อัปโหลดซ้ำ (ไม่งั้นเวลาจะถูกรีเซ็ตทุกครั้งที่บูต) */
#define SET_RTC_NOW   0
#define RTC_SET_Y 2025
#define RTC_SET_MO   6
#define RTC_SET_D   28
#define RTC_SET_DOW  6            // 1=จันทร์ ... 7=อาทิตย์
#define RTC_SET_H   12
#define RTC_SET_MI  10
#define RTC_SET_S    0

struct RtcTime { uint8_t sec, min, hour, date, month, dow; uint16_t year; };

/* ---------- ช่วงเวลาจ่ายยา ----------
 * ต้องประกาศ struct ไว้ "ก่อน" นิยามฟังก์ชันตัวแรกของไฟล์
 * เพราะ arduino-cli แทรก auto-prototype ไว้ตรงนั้น ถ้าประกาศทีหลัง
 * prototype ของ dispense(Slot&) จะมองไม่เห็น Slot */
struct Slot {
  uint8_t     startHour;
  uint8_t     endHour;
  uint8_t     servoIndex;
  bool        doneThisWindow;
  int         pillsLeft;
  const char *name;
};

Slot slots[3] = {
  {  6,  8, 0, false, 5, "เช้า"     },
  { 11, 13, 1, false, 5, "กลางวัน" },
  { 17, 19, 2, false, 5, "เย็น"    },
};

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static void rtcBegin() {
  pinMode(RTC_RST, OUTPUT);
  pinMode(RTC_CLK, OUTPUT);
  digitalWrite(RTC_RST, LOW);
  digitalWrite(RTC_CLK, LOW);
}
static void rtcOpen() {
  digitalWrite(RTC_CLK, LOW);
  digitalWrite(RTC_RST, HIGH);
  delayMicroseconds(4);
}
static void rtcClose() {
  digitalWrite(RTC_RST, LOW);
  delayMicroseconds(4);
}
static void rtcShiftOut(uint8_t v) {          // LSB first
  pinMode(RTC_DAT, OUTPUT);
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(RTC_DAT, (v >> i) & 0x01);
    delayMicroseconds(2);
    digitalWrite(RTC_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(RTC_CLK, LOW);
    delayMicroseconds(2);
  }
}
static uint8_t rtcShiftIn() {                 // อ่านก่อน แล้วค่อยตีคล็อก
  uint8_t v = 0;
  pinMode(RTC_DAT, INPUT);
  delayMicroseconds(2);
  for (uint8_t i = 0; i < 8; i++) {
    if (digitalRead(RTC_DAT)) v |= (1 << i);
    digitalWrite(RTC_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(RTC_CLK, LOW);
    delayMicroseconds(2);
  }
  return v;
}
static uint8_t rtcReadReg(uint8_t addr) {
  rtcOpen();
  rtcShiftOut(addr | 0x01);
  uint8_t v = rtcShiftIn();
  rtcClose();
  return v;
}
static void rtcWriteReg(uint8_t addr, uint8_t val) {
  rtcOpen();
  rtcShiftOut(addr & 0xFE);
  rtcShiftOut(val);
  rtcClose();
}
static RtcTime rtcGetTime() {
  RtcTime t;
  t.sec   = bcd2dec(rtcReadReg(0x80) & 0x7F);
  t.min   = bcd2dec(rtcReadReg(0x82) & 0x7F);
  t.hour  = bcd2dec(rtcReadReg(0x84) & 0x3F);   // โหมด 24 ชม.
  t.date  = bcd2dec(rtcReadReg(0x86) & 0x3F);
  t.month = bcd2dec(rtcReadReg(0x88) & 0x1F);
  t.dow   = bcd2dec(rtcReadReg(0x8A) & 0x07);
  t.year  = 2000 + bcd2dec(rtcReadReg(0x8C));
  return t;
}
static void rtcSetTime(uint16_t y, uint8_t mo, uint8_t d, uint8_t dow,
                       uint8_t h, uint8_t mi, uint8_t s) {
  rtcWriteReg(0x8E, 0x00);                      // ปลด write-protect
  rtcWriteReg(0x80, dec2bcd(s) & 0x7F);         // CH = 0 -> เดินนาฬิกา
  rtcWriteReg(0x82, dec2bcd(mi));
  rtcWriteReg(0x84, dec2bcd(h) & 0x3F);
  rtcWriteReg(0x86, dec2bcd(d));
  rtcWriteReg(0x88, dec2bcd(mo));
  rtcWriteReg(0x8A, dec2bcd(dow));
  rtcWriteReg(0x8C, dec2bcd(y % 100));
  rtcWriteReg(0x8E, 0x80);                      // ล็อกกลับ
}
static void rtcEnsureRunning() {                // ถ้า CH ค้างอยู่ให้ปลดออก
  uint8_t s = rtcReadReg(0x80);
  if (s & 0x80) {
    rtcWriteReg(0x8E, 0x00);
    rtcWriteReg(0x80, s & 0x7F);
    rtcWriteReg(0x8E, 0x80);
  }
}

/* ---------- สถานะ ---------- */
bool     facePrev    = false;
float    humidity    = NAN;
float    temperature = NAN;
uint32_t lastDhtRead = 0;
uint32_t lastStatus  = 0;

void dispense(Slot &s) {
  /* SoftServo ต้องยิงพัลส์ระหว่างขยับ -> moveTo() คุมความเร็ว + ค้างให้ในตัว */
  uint8_t i = s.servoIndex;
  servos[i].moveTo(servoCfg[i].push, servoCfg[i].speed, PUSH_HOLD_MS);
  servos[i].moveTo(servoCfg[i].home, servoCfg[i].speed, HOME_HOLD_MS);
  s.doneThisWindow = true;
  if (s.pillsLeft > 0) s.pillsLeft--;

  Monitor.print(">> จ่ายยามื้อ");
  Monitor.print(s.name);
  Monitor.print(" แล้ว | เหลือ ");
  Monitor.print(s.pillsLeft);
  Monitor.println(" เม็ด");
}

/* ============================================================
 *   ฟังก์ชันที่เปิดให้ฝั่ง Python เรียก (หน้าเว็บทดสอบ)
 *   ใช้ provide_safe ทั้งหมด -> ถูกเรียกจากเธรดเดียวกับ loop()
 *   ไม่ชนกับการสั่งเซอร์โวในลูปหลัก
 * ============================================================ */

bool servo_write(int idx, int angle) {
  if (idx < 0 || idx >= 3) return false;
  servos[idx].moveTo(angle, servoCfg[idx].speed, 200);
  Monitor.print("[test] servo "); Monitor.print(idx);
  Monitor.print(" -> "); Monitor.println(servos[idx].read());
  return true;
}

bool servo_pulse(int idx) {          // ปัด 1 ครั้งแล้วกลับตำแหน่งพัก
  if (idx < 0 || idx >= 3) return false;
  servos[idx].moveTo(servoCfg[idx].push, servoCfg[idx].speed, PUSH_HOLD_MS);
  servos[idx].moveTo(servoCfg[idx].home, servoCfg[idx].speed, HOME_HOLD_MS);
  Monitor.print("[test] pulse servo "); Monitor.println(idx);
  return true;
}

bool servo_home_all() {
  for (uint8_t i = 0; i < 3; i++) {
    servos[i].moveTo(servoCfg[i].home, servoCfg[i].speed, 300);
  }
  Monitor.println("[test] servo ทั้งหมดกลับตำแหน่งพัก");
  return true;
}

/* ---------- ตั้งค่าเซอร์โวรายตัวจากหน้าเว็บ ---------- */
bool set_servo_config(int idx, int home, int push, int speed) {
  if (idx < 0 || idx >= 3) return false;
  servoCfg[idx].home  = constrain(home,  SERVO_ANGLE_MIN, SERVO_ANGLE_MAX);
  servoCfg[idx].push  = constrain(push,  SERVO_ANGLE_MIN, SERVO_ANGLE_MAX);
  servoCfg[idx].speed = (uint16_t)constrain(speed, 0, SERVO_SPEED_MAX);

  Monitor.print("[cfg] servo "); Monitor.print(idx);
  Monitor.print(" พัก="); Monitor.print(servoCfg[idx].home);
  Monitor.print(" ปัด="); Monitor.print(servoCfg[idx].push);
  Monitor.print(" ความเร็ว="); Monitor.println(servoCfg[idx].speed);
  return true;
}

int get_servo_home(int idx)  { return (idx < 0 || idx >= 3) ? -999 : servoCfg[idx].home;  }
int get_servo_push(int idx)  { return (idx < 0 || idx >= 3) ? -999 : servoCfg[idx].push;  }
int get_servo_speed(int idx) { return (idx < 0 || idx >= 3) ? -999 : (int)servoCfg[idx].speed; }

/* ขอบเขตที่หน้าเว็บใช้กำหนดช่วงของ slider */
int get_angle_min() { return SERVO_ANGLE_MIN; }
int get_angle_max() { return SERVO_ANGLE_MAX; }
int get_speed_max() { return SERVO_SPEED_MAX; }

/* สั่งปัดยาจากหน้าเว็บ — คืนจำนวนเม็ดที่เหลือ, -1 = index ผิด, -2 = ยาหมด
   ตั้งใจให้ mark doneThisWindow ด้วย เพื่อกันจ่ายซ้ำในมื้อเดียวกัน
   (ถ้ากดเองแล้วระบบอัตโนมัติยังจ่ายอีก = ได้ยาเกินขนาด) */
int dispense_slot(int idx) {
  if (idx < 0 || idx >= 3) return -1;
  if (slots[idx].pillsLeft <= 0) {
    Monitor.print("[web] ยาหมดในช่อง "); Monitor.println(idx);
    return -2;
  }
  Monitor.print("[web] สั่งปัดยาด้วยมือ ช่อง "); Monitor.println(idx);
  dispense(slots[idx]);
  return slots[idx].pillsLeft;
}

/* เติมยากลับเข้าช่อง (หลังเติมจริง) */
int refill_slot(int idx, int pills) {
  if (idx < 0 || idx >= 3) return -1;
  if (pills < 0) pills = 0;
  slots[idx].pillsLeft = pills;
  slots[idx].doneThisWindow = false;
  Monitor.print("[web] เติมยาช่อง "); Monitor.print(idx);
  Monitor.print(" เป็น "); Monitor.println(pills);
  return slots[idx].pillsLeft;
}

int get_pills(int idx) {
  if (idx < 0 || idx >= 3) return -1;
  return slots[idx].pillsLeft;
}

float get_temperature() { return isnan(temperature) ? -999.0f : temperature; }
float get_humidity()    { return isnan(humidity)    ? -999.0f : humidity;    }
bool  get_face()        { return faceFromHost; }

/* ฝั่ง Linux แจ้งผลตรวจใบหน้าเข้ามา — ขอบขาขึ้นจะไปทริกเกอร์การจ่ายยาใน loop() */
bool set_face(bool present) {
  faceFromHost = present;
  return true;
}

void setup() {
  Monitor.begin(115200);

  /* Bridge ต้องเริ่มก่อน แล้วจึงลงทะเบียนฟังก์ชันให้ Python เรียก */
  Bridge.begin();
  Bridge.provide_safe("servo_write",     servo_write);
  Bridge.provide_safe("servo_pulse",     servo_pulse);
  Bridge.provide_safe("servo_home_all",  servo_home_all);
  Bridge.provide_safe("dispense_slot",   dispense_slot);
  Bridge.provide_safe("refill_slot",     refill_slot);
  Bridge.provide_safe("get_pills",       get_pills);
  Bridge.provide_safe("set_servo_config", set_servo_config);
  Bridge.provide_safe("get_servo_home",   get_servo_home);
  Bridge.provide_safe("get_servo_push",   get_servo_push);
  Bridge.provide_safe("get_servo_speed",  get_servo_speed);
  Bridge.provide_safe("get_angle_min",    get_angle_min);
  Bridge.provide_safe("get_angle_max",    get_angle_max);
  Bridge.provide_safe("get_speed_max",    get_speed_max);
  Bridge.provide_safe("get_temperature", get_temperature);
  Bridge.provide_safe("get_humidity",    get_humidity);
  Bridge.provide_safe("get_face",        get_face);
  Bridge.provide_safe("set_face",        set_face);

  /* Servo — ตั้งช่วงมุมก่อน แล้วค่อยสั่งไปตำแหน่งพัก
     ตอนบูตไม่รู้ว่าแขนอยู่ตรงไหน จึงสั่งตรง ๆ ไม่ใช้ ramp ความเร็ว */
  for (uint8_t i = 0; i < 3; i++) {
    servos[i].attach(servoPins[i]);
    servos[i].setLimits(SERVO_ANGLE_MIN, SERVO_ANGLE_MAX);
    servos[i].write(servoCfg[i].home);
    servos[i].hold(300);
  }

  dht.begin();
  Monitor.println("DHT11 Sensor Ready");

  rtcBegin();
#if SET_RTC_NOW
  rtcSetTime(RTC_SET_Y, RTC_SET_MO, RTC_SET_D, RTC_SET_DOW,
             RTC_SET_H, RTC_SET_MI, RTC_SET_S);
  Monitor.println("ตั้งเวลา RTC เรียบร้อย -> เปลี่ยน SET_RTC_NOW กลับเป็น 0 แล้วอัปโหลดใหม่");
#else
  rtcEnsureRunning();
#endif
}

void loop() {
  RtcTime t = rtcGetTime();

  /* ---- อ่าน DHT11 ทุก 2 วินาที (ตัวไดรเวอร์กันการอ่านถี่ให้อยู่แล้ว) ---- */
  if (millis() - lastDhtRead >= 2000) {
    lastDhtRead = millis();
    float c, h;
    if (dht.read(c, h)) { temperature = c; humidity = h; }
  }

  /* ---- ตรวจจับใบหน้า (ฝั่ง Linux ส่งมาให้ผ่าน set_face) ---- */
  bool faceNow = faceFromHost;

  /* ---- หาช่วงเวลาปัจจุบัน + รีเซ็ตช่วงที่ผ่านไปแล้ว ---- */
  int activeSlot = -1;
  for (uint8_t i = 0; i < 3; i++) {
    if (t.hour >= slots[i].startHour && t.hour < slots[i].endHour) activeSlot = i;
    else slots[i].doneThisWindow = false;
  }

  /* ---- ขอบขาขึ้นของการเจอหน้า -> จ่ายยา ---- */
  if (faceNow && !facePrev && activeSlot >= 0) {
    Slot &s = slots[activeSlot];
    if (!s.doneThisWindow && s.pillsLeft > 0) dispense(s);
    else if (s.pillsLeft <= 0) Monitor.println("!! ยาหมดในช่องนี้");
  }
  facePrev = faceNow;

  /* ---- รายงานสถานะทุก 1 วินาที ---- */
  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();

    Monitor.print("อุณหภูมิ: ");
    if (isnan(temperature)) Monitor.print("--"); else Monitor.print(temperature, 1);
    Monitor.print(" C  |  ความชื้น: ");
    if (isnan(humidity)) Monitor.print("--"); else Monitor.print(humidity, 1);
    Monitor.println(" %");

    char buf[32];
    snprintf(buf, sizeof(buf), "%02u/%02u/%u | %02u:%02u:%02u",
             t.date, t.month, t.year, t.hour, t.min, t.sec);
    Monitor.println(buf);

    Monitor.print("ใบหน้า: ");
    Monitor.print(faceNow ? "พบ" : "ไม่พบ");
    Monitor.print("  | ช่วง: ");
    Monitor.println(activeSlot >= 0 ? slots[activeSlot].name : "-");

    Monitor.println(" - - - - - - - - - - - - - - - - - - -");
  }

  delay(50);
}
