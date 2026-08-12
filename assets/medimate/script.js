const video = document.getElementById("video");
const canvas = document.getElementById("canvas");
const ctx = canvas.getContext("2d");
const statusBox = document.getElementById("statusBox");
const analysisBox = document.getElementById("analysisBox");
const loadingOverlay = document.getElementById("loadingOverlay");

const cameraSelect = document.getElementById("cameraSelect");
const switchCamBtn = document.getElementById("switchCamBtn");

/* ================= AUDIO ================= */
const alertSound = new Audio("alert.mp3");
alertSound.volume = 1.0;

// ต้องมี user gesture อย่างน้อยครั้งเดียวเพื่อให้เล่นเสียงได้
document.body.addEventListener("click", () => {
  alertSound.play().then(() => alertSound.pause()).catch(()=>{});
}, { once:true });

/* ================= CANVAS ================= */
function resizeCanvas() {
  const rect = canvas.parentElement.getBoundingClientRect();
  canvas.width = rect.width;
  canvas.height = rect.height;
}
window.addEventListener("resize", resizeCanvas);
resizeCanvas();

/* ================= CAMERA (เลือกกล้องได้) ================= */
let currentStream = null;
let currentDeviceId = null;

async function stopStream() {
  if (currentStream) {
    currentStream.getTracks().forEach(t => t.stop());
    currentStream = null;
  }
}

async function startStream(deviceId = null) {
  await stopStream();

  const constraints = {
    video: deviceId ? { deviceId: { exact: deviceId } } : { facingMode: "user" },
    audio: false
  };

  try {
    const stream = await navigator.mediaDevices.getUserMedia(constraints);
    currentStream = stream;
    video.srcObject = stream;

    statusBox.innerText = "สถานะ: กล้องพร้อม ✔";
    statusBox.style.color = "#0056a6";
    statusBox.style.borderColor = "#9ecbff";
  } catch (err) {
    statusBox.innerText = "ปัญหากล้อง: " + err.message;
    statusBox.style.color = "#b00000";
    statusBox.style.borderColor = "#ff5b5b";
    throw err;
  }
}

async function loadCameraList() {
  // ต้องขอ permission ก่อน ถึงจะเห็นชื่อกล้อง
  if (!currentStream) await startStream(null);

  const devices = await navigator.mediaDevices.enumerateDevices();
  const cams = devices.filter(d => d.kind === "videoinput");

  cameraSelect.innerHTML = "";
  cams.forEach((cam, idx) => {
    const opt = document.createElement("option");
    opt.value = cam.deviceId;
    opt.textContent = cam.label || `กล้อง ${idx + 1}`;
    cameraSelect.appendChild(opt);
  });

  if (!currentDeviceId && cams[0]) {
    currentDeviceId = cams[0].deviceId;
    cameraSelect.value = currentDeviceId;
  }
}

switchCamBtn.addEventListener("click", async () => {
  const selected = cameraSelect.value;
  if (!selected) return;
  currentDeviceId = selected;
  await startStream(currentDeviceId);
});

if (navigator.mediaDevices?.addEventListener) {
  navigator.mediaDevices.addEventListener("devicechange", () => {
    loadCameraList().catch(()=>{});
  });
}

/* ================= POSE ================= */
const pose = new Pose({
  locateFile: f => `https://cdn.jsdelivr.net/npm/@mediapipe/pose/${f}`
});

pose.setOptions({
  modelComplexity: 0,
  smoothLandmarks: false,
  minDetectionConfidence: 0.5,
  minTrackingConfidence: 0.5
});

/* ================= Helpers ================= */
function mid(a, b){ return { x:(a.x+b.x)/2, y:(a.y+b.y)/2 }; }
function angleDeg(a,b){ return Math.atan2(b.y-a.y, b.x-a.x) * 180/Math.PI; }
function dist(a,b){ return Math.hypot(a.x-b.x, a.y-b.y); }
function getLM(lm, idx){ return (lm && lm[idx]) ? lm[idx] : null; }
function vis(l){ return !!l && (l.visibility ?? 1) >= 0.5; }

/* ================= Posture (robust) ================= */
function postureClassify(lm){
  const nose = getLM(lm,0);

  const ls = getLM(lm,11), rs = getLM(lm,12);
  const lh = getLM(lm,23), rh = getLM(lm,24);
  const lk = getLM(lm,25), rk = getLM(lm,26);
  const la = getLM(lm,27), ra = getLM(lm,28);

  const hasShoulders = vis(ls) && vis(rs);
  const hasHips      = vis(lh) && vis(rh);

  const shoulderMid = (hasShoulders) ? mid(ls,rs) : null;
  const hipMid      = (hasHips) ? mid(lh,rh) : null;

  const hasKnees = vis(lk) && vis(rk);
  const kneeMid  = (hasKnees) ? mid(lk,rk) : null;

  const hasAnkles = vis(la) && vis(ra);
  const ankleMid  = (hasAnkles) ? mid(la,ra) : null;

  let torsoAngleAbs = null;
  let nearHorizontal = false;

  if (shoulderMid && hipMid) {
    torsoAngleAbs = Math.abs(angleDeg(shoulderMid, hipMid));
    nearHorizontal = (torsoAngleAbs < 35 || torsoAngleAbs > 145);
  } else if (vis(nose) && hipMid) {
    torsoAngleAbs = Math.abs(angleDeg(nose, hipMid));
    nearHorizontal = (torsoAngleAbs < 30 || torsoAngleAbs > 150);
  }

  const hipY   = hipMid?.y ?? null;
  const kneeY  = kneeMid?.y ?? null;
  const ankleY = ankleMid?.y ?? null;

  const hipToKnee  = (hipY!=null && kneeY!=null)  ? Math.abs(kneeY - hipY)  : null;
  const hipToAnkle = (hipY!=null && ankleY!=null) ? Math.abs(ankleY - hipY) : null;

  let shoulderWidth = null;
  if (hasShoulders) shoulderWidth = dist(ls, rs);

  let upperBodyHeight = null;
  if (shoulderMid && hipMid) upperBodyHeight = Math.abs(hipMid.y - shoulderMid.y);

  // นอน
  if (torsoAngleAbs != null && nearHorizontal) {
    let confidence = 0.85;
    if (shoulderWidth != null && upperBodyHeight != null && shoulderWidth > upperBodyHeight * 1.2) {
      confidence = Math.min(0.95, confidence + 0.07);
    }
    return { posture:"LYING", confidence };
  }

  // ยืน/นั่ง (มีข้อเท้า)
  if (hipToAnkle != null) {
    let posture = "SITTING";
    let confidence = 0.7;

    if (hipToAnkle > 0.22) posture="STANDING", confidence=0.80;
    else if (hipToAnkle >= 0.12) posture="SITTING", confidence=0.70;
    else posture="SITTING", confidence=0.55;

    if (torsoAngleAbs != null) {
      const verticalLike = (torsoAngleAbs > 60 && torsoAngleAbs < 120);
      if (verticalLike && posture === "STANDING") confidence = Math.min(0.90, confidence + 0.08);
      if (!verticalLike && posture === "SITTING") confidence = Math.min(0.85, confidence + 0.05);
    }
    return { posture, confidence };
  }

  // ยืน/นั่ง (มีเข่า)
  if (hipToKnee != null) {
    const kneeNearHip = hipToKnee < 0.11;
    let posture = kneeNearHip ? "SITTING" : "STANDING";
    let confidence = kneeNearHip ? 0.72 : 0.60;

    if (torsoAngleAbs != null) {
      const verticalLike = (torsoAngleAbs > 60 && torsoAngleAbs < 120);
      confidence = verticalLike ? Math.min(0.85, confidence + 0.08) : Math.max(0.45, confidence - 0.08);
    }
    return { posture, confidence };
  }

  // เห็นช่วงบน
  if (torsoAngleAbs != null) {
    const verticalLike = (torsoAngleAbs > 65 && torsoAngleAbs < 115);
    return { posture: verticalLike ? "STANDING" : "SITTING", confidence: 0.50 };
  }

  return { posture:"UNKNOWN", confidence:0.35 };
}

/* ================= FALL + NEAR-FALL (เกือบล้ม) ================= */
let lastHipY = null;
let lastT = null;

let fastDropAt = null;
let lyingSince = null;

let fallLatched = false;       // ล้มค้างจนลุก
let recoveredSince = null;

let nearFallSince = null;      // เกือบล้มค้างเพื่อยืนยัน

let fallBeeped = false;        // เล่นเสียงครั้งเดียวตอนเริ่มล้ม

const FALL = {
  SPEED_THRESH: 0.75,        // ตกเร็ว
  DROP_WINDOW_MS: 1200,      // ตกเร็วแล้วต้องไปนอนภายในเวลานี้
  LYING_HOLD_MS: 700,        // นอนค้างเพื่อยืนยันล้ม
  RECOVER_HOLD_MS: 900,      // ลุกค้างเพื่อปลดล้ม

  NEAR_FALL_HOLD_MS: 450,    // ค้างเพื่อยืนยันเกือบล้ม
  WRIST_LOW_MARGIN: 0.03     // มืออยู่ “ต่ำ/ใกล้สะโพก” ถือว่าดันทัน
};

function detectFallEvent(lm, postureResult){
  const lh = getLM(lm,23), rh = getLM(lm,24);
  const lw = getLM(lm,15), rw = getLM(lm,16); // ข้อมือ
  const now = performance.now();

  if(!lh || !rh){
    return { state: fallLatched ? "FALL_CONFIRMED" : "UNKNOWN", isFall: fallLatched };
  }

  const hipY = (lh.y + rh.y) / 2;

  // speed
  let fastDrop = false;
  if(lastHipY != null && lastT != null){
    const dt = (now - lastT) / 1000;
    const dy = hipY - lastHipY;
    const speed = dy / Math.max(dt, 0.001);
    fastDrop = speed > FALL.SPEED_THRESH;
  }
  lastHipY = hipY;
  lastT = now;

  if(fastDrop) fastDropAt = now;

  // lying hold
  const lyingConfident = (postureResult.posture === "LYING" && postureResult.confidence >= 0.6);
  if(lyingConfident){
    if(!lyingSince) lyingSince = now;
  } else {
    lyingSince = null;
  }

  // 1) ถ้าล้มค้างแล้ว: รอจนกว่าจะลุกค้าง
  if(fallLatched){
    const upConfident =
      (postureResult.posture === "STANDING" && postureResult.confidence >= 0.6) ||
      (postureResult.posture === "SITTING"  && postureResult.confidence >= 0.6);

    if(upConfident){
      if(!recoveredSince) recoveredSince = now;
    } else {
      recoveredSince = null;
    }

    if(recoveredSince && (now - recoveredSince >= FALL.RECOVER_HOLD_MS)){
      fallLatched = false;
      recoveredSince = null;
      fastDropAt = null;
      fallBeeped = false;
      nearFallSince = null;
      return { state:"OK", isFall:false };
    }

    return { state:"FALL_CONFIRMED", isFall:true };
  }

  // 2) ยืนยัน “ล้ม”: ตกเร็ว -> ไปนอน ภายใน window และนอนค้าง
  const withinWindow = fastDropAt && (now - fastDropAt <= FALL.DROP_WINDOW_MS);
  const lyingHold = lyingSince && (now - lyingSince >= FALL.LYING_HOLD_MS);

  if(withinWindow && lyingHold){
    fallLatched = true;
    recoveredSince = null;
    nearFallSince = null;
    return { state:"FALL_CONFIRMED", isFall:true };
  }

  // 3) เกือบล้ม (พยุงทัน): นั่งทัน / มือดันทัน (ตกเร็ว แต่ไม่ถึงนอน)
  if(withinWindow && !lyingConfident){
    const sittingConfident = (postureResult.posture === "SITTING" && postureResult.confidence >= 0.6);

    // มือดันทัน: ข้อมืออยู่ต่ำมาก (y มาก) และ “ใกล้/ต่ำกว่า” ระดับสะโพก
    const hasWrists = !!lw && !!rw;
    const wristY = hasWrists ? ((lw.y + rw.y) / 2) : null;
    const braceHands = hasWrists && (wristY >= hipY - FALL.WRIST_LOW_MARGIN);

    if(sittingConfident || braceHands){
      if(!nearFallSince) nearFallSince = now;
      const holdOK = (now - nearFallSince) >= FALL.NEAR_FALL_HOLD_MS;

      if(holdOK){
        if (sittingConfident) return { state:"NEAR_FALL_SIT", isFall:false };
        if (braceHands) return { state:"NEAR_FALL_BRACE", isFall:false };
      }
      return { state:"FALL_LIKELY", isFall:false }; // ระหว่างรอยืนยัน
    } else {
      nearFallSince = null;
    }
  } else {
    nearFallSince = null;
  }

  // ระหว่างทาง
  if(withinWindow && lyingConfident) return { state:"FALL_LIKELY", isFall:false };
  if(fastDrop) return { state:"DROP_DETECTED", isFall:false };

  return { state:"OK", isFall:false };
}

/* ================= UI ภาษาไทย ================= */
const thPost = { STANDING:"ยืน", SITTING:"นั่ง", LYING:"นอน", UNKNOWN:"ไม่ชัด" };
const thFallState = {
  FALL_CONFIRMED: "ล้ม",
  FALL_LIKELY: "สงสัยว่าล้ม",
  DROP_DETECTED: "ตกลงเร็ว",
  NEAR_FALL_SIT: "เกือบล้ม (นั่งทัน)",
  NEAR_FALL_BRACE: "เกือบล้ม (เอามือดันทัน)",
  OK: "ปกติ",
  UNKNOWN: "ไม่ชัด"
};

function setNormal(){
  statusBox.innerText = "สถานะ: ปกติ ✔";
  statusBox.style.color = "#0056a6";
  statusBox.style.borderColor = "#9ecbff";
}
function setWarn(text){
  statusBox.innerText = text;
  statusBox.style.color = "#7a4b00";
  statusBox.style.borderColor = "#ffd08a";
}
function setDanger(text){
  statusBox.innerText = text;
  statusBox.style.color = "#b00000";
  statusBox.style.borderColor = "#ff5b5b";
}

/* ================= DRAW + LOGIC ================= */
pose.onResults(results => {
  loadingOverlay.style.display = "none";
  ctx.clearRect(0,0,canvas.width,canvas.height);

  if (!results.poseLandmarks) {
    analysisBox.innerText = "ท่า: - | สถานะ: -";
    setWarn("สถานะ: ไม่พบคน");
    fallBeeped = false;
    return;
  }

  drawConnectors(ctx, results.poseLandmarks, POSE_CONNECTIONS, { color:"#00aaff", lineWidth:3 });
  drawLandmarks(ctx, results.poseLandmarks, { color:"#ff66bb", radius:3 });

  const p = postureClassify(results.poseLandmarks);
  const f = detectFallEvent(results.poseLandmarks, p);

  analysisBox.innerText =
    `ท่า: ${thPost[p.posture] ?? p.posture} (${Math.round(p.confidence*100)}%) | สถานะ: ${thFallState[f.state] ?? f.state}`;

  // ล้ม (เล่นเสียง)
  if (f.state === "FALL_CONFIRMED") {
    setDanger("สถานะ: ⚠ ตรวจพบการล้ม");

    if (!fallBeeped) {
      fallBeeped = true;
      alertSound.currentTime = 0;
      alertSound.play().catch(()=>{});
    }
    return;
  }

  // ไม่ล้มยืนยันแล้ว
  fallBeeped = false;

  // เกือบล้ม
  if (f.state === "NEAR_FALL_SIT" || f.state === "NEAR_FALL_BRACE") {
    setWarn("สถานะ: ⚠ เกือบล้ม (พยุงทัน)");
    return;
  }

  if (f.state === "FALL_LIKELY") setWarn("สถานะ: ⚠ สงสัยว่าล้ม…");
  else if (f.state === "DROP_DETECTED") setWarn("สถานะ: ⚠ ตรวจพบการตกลงเร็ว…");
  else setNormal();
});

/* ================= LOOP ================= */
async function run() {
  if (video.readyState >= 2) {
    await pose.send({ image: video });
  }
  requestAnimationFrame(run);
}

/* ================= BOOT ================= */
(async function boot(){
  try{
    await loadCameraList();
    video.onloadeddata = () => run();
  }catch(e){
    console.error(e);
  }
})();
