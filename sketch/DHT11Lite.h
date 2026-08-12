/* DHT11Lite — ไดรเวอร์ DHT11 แบบ single-wire เขียนเอง (ไม่พึ่ง Adafruit DHT)
 *
 * โปรโตคอล DHT11:
 *   1) MCU ดึงสายลง LOW >= 18 ms แล้วปล่อย
 *   2) DHT ตอบ: LOW 80 µs -> HIGH 80 µs
 *   3) ส่ง 40 บิต แต่ละบิต = LOW 50 µs แล้ว HIGH
 *        HIGH ~26-28 µs = บิต 0
 *        HIGH ~70 µs    = บิต 1
 *   4) 5 ไบต์: RH_int, RH_dec, T_int, T_dec, checksum
 *
 * วัดความกว้างพัลส์ด้วย micros() แทนการนับรอบลูป เพราะ digitalRead ของ
 * ArduinoCore-zephyr ช้ากว่า AVR มาก (ผ่าน Zephyr GPIO API) การนับรอบจะเพี้ยน
 * เกณฑ์ตัดสิน 0/1 ตั้งไว้ 45 µs ซึ่งอยู่กึ่งกลางระหว่าง 28 กับ 70 พอดี
 *
 * ถ้าอ่านไม่ผ่านบ่อย ๆ ลองขยับ BIT_THRESHOLD_US หรือเปลี่ยนไปใช้เซนเซอร์
 * แบบ I2C (DHT20/AHT20) ซึ่งไม่ต้องพึ่ง timing ของ GPIO เลย
 */

#pragma once
#include <Arduino.h>

class DHT11Lite {
public:
  explicit DHT11Lite(uint8_t pin) : _pin(pin) {}

  void begin() {
    pinMode(_pin, INPUT_PULLUP);
    _lastRead = 0;
    _hasValue = false;
  }

  /* คืน true ถ้าอ่านสำเร็จ ค่าถูกเขียนลง temperature/humidity
     ถ้าเรียกถี่กว่า 2 วินาที จะคืนค่าที่แคชไว้ (DHT11 อ่านได้ทุก 1-2 วิ) */
  bool read(float &temperature, float &humidity) {
    uint32_t now = millis();
    if (_hasValue && (uint32_t)(now - _lastRead) < MIN_INTERVAL_MS) {
      temperature = _t;
      humidity    = _h;
      return true;
    }
    _lastRead = now;

    uint8_t data[5] = { 0, 0, 0, 0, 0 };

    /* --- สัญญาณเริ่ม --- */
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
    delay(20);                        // >= 18 ms
    digitalWrite(_pin, HIGH);
    delayMicroseconds(30);
    pinMode(_pin, INPUT_PULLUP);

    /* --- DHT ตอบรับ --- */
    if (!waitFor(LOW,  200)) return fail();   // เริ่ม LOW 80 µs
    if (!waitFor(HIGH, 200)) return fail();   // เริ่ม HIGH 80 µs

    /* --- 40 บิต --- */
    for (uint8_t i = 0; i < 40; i++) {
      if (!waitFor(LOW,  200)) return fail();   // LOW 50 µs นำหน้าบิต
      if (!waitFor(HIGH, 200)) return fail();   // ขอบขาขึ้น
      uint32_t t0 = micros();
      if (!waitFor(LOW,  200)) return fail();   // ขอบขาลง
      uint32_t width = micros() - t0;

      data[i / 8] <<= 1;                        // DHT ส่ง MSB ก่อน
      if (width > BIT_THRESHOLD_US) data[i / 8] |= 1;
    }

    /* --- checksum --- */
    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if (sum != data[4]) return fail();

    _h = data[0] + data[1] * 0.1f;
    _t = (data[2] & 0x7F) + data[3] * 0.1f;
    if (data[2] & 0x80) _t = -_t;               // บิตเครื่องหมายของอุณหภูมิ

    _hasValue   = true;
    temperature = _t;
    humidity    = _h;
    return true;
  }

  bool  hasValue() const   { return _hasValue; }
  float temperature() const { return _t; }
  float humidity() const    { return _h; }

private:
  static const uint32_t BIT_THRESHOLD_US  = 45;    // > นี้ = บิต 1
  static const uint32_t MIN_INTERVAL_MS   = 2000;

  bool waitFor(uint8_t level, uint32_t timeoutUs) {
    uint32_t t0 = micros();
    while (digitalRead(_pin) != level) {
      if ((uint32_t)(micros() - t0) > timeoutUs) return false;
    }
    return true;
  }

  bool fail() {
    pinMode(_pin, INPUT_PULLUP);
    return false;
  }

  uint8_t  _pin;
  uint32_t _lastRead = 0;
  bool     _hasValue = false;
  float    _t = 0.0f;
  float    _h = 0.0f;
};
