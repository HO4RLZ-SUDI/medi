/* SoftServo — เซอร์โวแบบ bit-bang สำหรับ Arduino UNO Q
 *
 * ทำไมไม่ใช้ Servo.h: ไลบรารี Servo ต้องลงเพิ่ม แต่ sketch.yaml แบบ profile
 * บัง ~/Arduino/libraries ทั้งหมด (โปรไฟล์เป็น self-contained) ตัวนี้พึ่งแค่
 * Arduino core ล้วน ๆ เลยไม่มีปัญหานั้น
 *
 * ทำไมไม่ใช้ analogWrite: PWM ของ UNO Q ล็อกที่ 500 Hz (คาบ 2 ms)
 * สร้างเฟรมเซอร์โว 50 Hz (คาบ 20 ms) ไม่ได้
 *
 * วิธีทำงาน: ยิงพัลส์ HIGH ยาว 544–2400 µs เอง แล้วเว้น ~20 ms ต่อเฟรม
 * ระหว่างพัลส์ "ไม่ปิด interrupt" — ใช้ busy-wait ด้วย micros() แทน
 * เพราะการล็อก IRQ นาน 2.4 ms เสี่ยงกวน Bridge/USB ของฝั่ง Linux
 * ความคลาดเคลื่อนจาก interrupt latency ไม่กี่ µs = ต่ำกว่า 0.5°
 *
 * เซอร์โวจะถูกยิงพัลส์เฉพาะตอนสั่งให้ขยับ (hold) พอหยุดยิงเซอร์โวก็คลายแรง
 * เหมาะกับงานตู้จ่ายยาที่ขยับเป็นครั้ง ๆ และช่วยลดความร้อน/กระตุก
 */

#pragma once
#include <Arduino.h>

class SoftServo {
public:
  static const uint16_t DEFAULT_MIN_US = 544;   // พัลส์สั้นสุดที่เซอร์โวทั่วไปรับได้
  static const uint16_t DEFAULT_MAX_US = 2400;  // พัลส์ยาวสุด
  static const uint16_t FRAME_MS       = 20;    // 50 Hz

  void attach(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
    _attached = true;
  }

  /* กำหนดช่วงมุมที่ยอมรับ — รองรับมุมติดลบ
     ช่วงมุมนี้จะถูก map ลงบนช่วงพัลส์แบบเชิงเส้น
     เช่น setLimits(-30, 180) -> -30° = 544 µs, 180° = 2400 µs
     ทำให้สั่งมุมติดลบได้โดยไม่ยิงพัลส์สั้นเกินจนเซอร์โวไม่รับ */
  void setLimits(int minAngle, int maxAngle) {
    if (minAngle >= maxAngle) return;
    _minA = minAngle;
    _maxA = maxAngle;
    write(_angle);                  // คำนวณพัลส์ใหม่ตามสเกลใหม่
  }

  /* ปรับเทียบความกว้างพัลส์จริงของเซอร์โวรุ่นที่ใช้
     ถ้าอยากให้ 1 หน่วยมุม = 1 องศาจริง ต้องจูนคู่กับ setLimits */
  void setPulseRange(uint16_t minUs, uint16_t maxUs) {
    if (minUs >= maxUs) return;
    _minUs = minUs;
    _maxUs = maxUs;
    write(_angle);
  }

  int minAngle() const { return _minA; }
  int maxAngle() const { return _maxA; }

  void detach() {
    if (_attached) digitalWrite(_pin, LOW);
    _attached = false;
  }

  bool attached() const { return _attached; }
  int  read() const     { return _angle; }

  void writeMicroseconds(uint16_t us) {
    _us = constrain(us, _minUs, _maxUs);
    _angle = map((int)_us, (int)_minUs, (int)_maxUs, _minA, _maxA);
  }

  void write(int angle) {
    _angle = constrain(angle, _minA, _maxA);
    _us = (uint16_t)map(_angle, _minA, _maxA, (int)_minUs, (int)_maxUs);
  }

  /* ยิงพัลส์ 1 ลูก — ต้องเรียกซ้ำทุก ~20 ms เซอร์โวถึงจะขยับ */
  void pulse() {
    if (!_attached) return;
    digitalWrite(_pin, HIGH);
    uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < _us) { /* busy-wait */ }
    digitalWrite(_pin, LOW);
  }

  /* ยิงพัลส์ต่อเนื่องนาน holdMs แล้วปล่อยให้เซอร์โวคลาย
     ใช้ตอนสั่งขยับ: write(angle) แล้วตามด้วย hold(...) */
  void hold(uint16_t holdMs) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < holdMs) {
      pulse();
      delay(FRAME_MS);
    }
  }

  /* ขยับไปยัง target ด้วยความเร็วที่กำหนด (องศา/วินาที) แล้วค้างต่อ holdMs
     degPerSec = 0 -> กระโดดไปทันที (เร็วสุดเท่าที่เซอร์โวทำได้)
     ค่ายิ่งน้อยยิ่งช้า ช่วยลดการกระชากและเสียงของเซอร์โวราคาถูก */
  void moveTo(int target, uint16_t degPerSec, uint16_t holdMs) {
    target = constrain(target, _minA, _maxA);

    if (degPerSec > 0 && target != _angle) {
      // 1 เฟรม = FRAME_MS -> เดินได้กี่องศาต่อเฟรม
      int perFrame = (int)(((uint32_t)degPerSec * FRAME_MS) / 1000);
      if (perFrame < 1) perFrame = 1;

      int dir = (target > _angle) ? 1 : -1;
      while (_angle != target) {
        int next = _angle + dir * perFrame;
        if ((dir > 0 && next > target) || (dir < 0 && next < target)) next = target;
        write(next);
        pulse();
        delay(FRAME_MS);
      }
    } else {
      write(target);
    }

    hold(holdMs);
  }

private:
  uint8_t  _pin      = 255;
  bool     _attached = false;
  uint16_t _us       = 1500;
  int      _angle    = 90;
  int      _minA     = 0;
  int      _maxA     = 180;
  uint16_t _minUs    = DEFAULT_MIN_US;
  uint16_t _maxUs    = DEFAULT_MAX_US;
};
