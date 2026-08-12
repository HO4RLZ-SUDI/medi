/* medimate-hw.js — ตัวกลางระหว่างหน้าเว็บ Medimate กับตู้จ่ายยา UNO Q
 *
 * ห่อ API ที่ python/hwtest.py และ python/medimate.py เปิดไว้ ให้หน้าเว็บเรียกง่าย ๆ
 * โดยไม่ต้องรู้จัก socket.io เอง และถ้าเปิดหน้าเว็บนอกบอร์ด (เช่นเปิดไฟล์ตรง ๆ)
 * ทุกฟังก์ชันจะไม่พัง แค่ทำงานในโหมดออฟไลน์เฉย ๆ
 *
 * ใช้งาน:
 *   <script src="../libs/socket.io.min.js"></script>
 *   <script src="../libs/arduino.js"></script>
 *   <script src="medimate-hw.js"></script>
 *   MedimateHW.onPills(p => ...);
 *   MedimateHW.dispense(0);
 */
(function (global) {
  "use strict";

  var TUBE_LABELS = ["ยาหลอดที่ 1", "ยาหลอดที่ 2", "ยาหลอดที่ 3"];
  var MEAL_LABELS = ["ช่วงเช้า", "ช่วงเที่ยง", "ช่วงเย็น"];
  var WINDOWS = ["06:00–08:00", "11:00–13:00", "17:00–19:00"];

  var ui = null;
  var connected = false;
  var listeners = {};      // event -> [fn]

  function emitLocal(event, data) {
    (listeners[event] || []).forEach(function (fn) {
      try { fn(data); } catch (e) { console.error(e); }
    });
  }

  function on(event, fn) {
    (listeners[event] = listeners[event] || []).push(fn);
    return HW;
  }

  /* ---------- socket ---------- */
  function connect() {
    if (ui || typeof global.WebUI === "undefined" || typeof global.io === "undefined") return;
    try {
      ui = new global.WebUI();
    } catch (e) {
      ui = null;
      return;
    }

    ui.on_connect(function () { connected = true; emitLocal("connect"); });
    ui.on_disconnect(function () { connected = false; emitLocal("disconnect"); });

    // เหตุการณ์จาก hwtest.py
    ui.on_message("pills_update", function (d) { emitLocal("pills", (d && d.pills) || []); });
    ui.on_message("dispense_status", function (d) { emitLocal("dispense_status", d || {}); });
    ui.on_message("servo_status", function (d) { emitLocal("servo_status", d || {}); });
    ui.on_message("face_update", function (d) { emitLocal("face", !!(d && d.face)); });
    ui.on_message("cam_status", function (d) { emitLocal("cam_status", d || {}); });

    // เหตุการณ์จาก medimate.py
    ui.on_message("history_update", function (d) { emitLocal("history", (d && d.history) || []); });
    ui.on_message("tubes_update", function (d) { emitLocal("tubes", (d && d.tubes) || []); });
    ui.on_message("medimate_status", function (d) { emitLocal("status", d || {}); });
  }

  function send(event, data) {
    if (!ui) return false;
    ui.send_message(event, data || {});
    return true;
  }

  /* ---------- REST ---------- */
  function getJSON(path) {
    return fetch(path, { cache: "no-store" }).then(function (r) {
      if (!r.ok) throw new Error(path + " -> HTTP " + r.status);
      return r.json();
    });
  }

  var HW = {
    TUBE_LABELS: TUBE_LABELS,
    MEAL_LABELS: MEAL_LABELS,
    WINDOWS: WINDOWS,

    /** เชื่อมต่อ socket (เรียกซ้ำได้ ไม่มีผล) */
    init: function () { connect(); return HW; },

    /** ต่อกับบอร์ดอยู่หรือไม่ */
    isConnected: function () { return connected; },

    /** มีฝั่งบอร์ดให้คุยด้วยไหม (มี socket.io + arduino.js โหลดมาแล้ว) */
    isAvailable: function () { return ui !== null; },

    /* ----- อ่านสถานะ ----- */
    status:   function () { return getJSON("/api/medimate/status"); },
    slots:    function () { return getJSON("/api/slots"); },
    sensors:  function () { return getJSON("/api/sensors"); },
    history:  function () { return getJSON("/api/history"); },
    tubes:    function () { return getJSON("/api/tubes"); },
    camera:   function () { return getJSON("/api/cam/status"); },

    /* ----- สั่งงาน ----- */

    /** สั่งปัดยาหลอด idx (0-2) — ติดป้ายที่มาให้ประวัติด้วย */
    dispense: function (idx, source) {
      send("mm_note", { idx: idx, source: source || "สั่งจากหน้าเว็บ Medimate" });
      return send("dispense", { idx: idx });
    },

    /** เติมยาหลอด idx เป็นจำนวน pills เม็ด */
    refill: function (idx, pills) {
      return send("refill", { idx: idx, pills: pills });
    },

    /** ส่งรายการยาจาก Firestore มาเก็บที่บอร์ด เพื่อให้ประวัติมีชื่อยา
     *  tubes = [{name, detail, count}, x3] */
    syncTubes: function (tubes) {
      return send("mm_set_tubes", { tubes: tubes });
    },

    clearHistory: function () { return send("mm_clear_history", {}); },

    /* ----- ทดสอบกลไก ----- */
    servoWrite: function (idx, angle) { return send("servo_write", { idx: idx, angle: angle }); },
    servoPulse: function (idx) { return send("servo_pulse", { idx: idx }); },
    servoSweep: function (idx) { return send("servo_sweep", { idx: idx }); },
    servoHome:  function () { return send("servo_home", {}); },
    setServoConfig: function (idx, home, push, speed) {
      return send("set_servo_config", { idx: idx, home: home, push: push, speed: speed });
    },
    setConfidence: function (v) { return send("set_confidence", { confidence: v }); },

    /* ----- ตัวรับเหตุการณ์ ----- */
    on: on,
    onPills:    function (fn) { return on("pills", fn); },
    onHistory:  function (fn) { return on("history", fn); },
    onTubes:    function (fn) { return on("tubes", fn); },
    onFace:     function (fn) { return on("face", fn); },
    onDispense: function (fn) { return on("dispense_status", fn); },
    onServo:    function (fn) { return on("servo_status", fn); },
    onConnect:  function (fn) { return on("connect", fn); },
    onDisconnect: function (fn) { return on("disconnect", fn); },

    /** ลิงก์ภาพพรีวิวจากกล้อง (brick เสิร์ฟที่พอร์ตแยก) */
    cameraEmbedUrl: function (info) {
      var port = (info && info.port) || 4912;
      var path = (info && info.path) || "/embed";
      return location.protocol + "//" + location.hostname + ":" + port + path;
    }
  };

  connect();
  global.MedimateHW = HW;
})(window);
