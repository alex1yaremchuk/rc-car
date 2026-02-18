#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>
#include <string.h>

#define STBY 7
#define AIN1 5
#define AIN2 6
#define PWMA 4
#define BIN1 8
#define BIN2 9
#define PWMB 10
// ==== Battery sensing ====
// Подключи к ADC только через делитель (не напрямую к 2S батарее).
#define BAT_ADC_PIN 1
const bool BAT_MONITOR_ENABLED = true;

// Делитель: Rtop (к батарее) и Rbot (к земле)
const float R_TOP = 100000.0f;
const float R_BOT = 47000.0f;

// Пороговые напряжения батареи 2S
const float BAT_WARN = 6.60f;    // предупредить
const float BAT_CUTOFF = 6.30f;  // жестко остановить

// ==== Motor control state ====
int speedVal = 180;
int dirA = 0;  // -1 back, 0 stop, +1 fwd
int dirB = 0;

WebServer server(80);

// dead-man switch (если команды перестали приходить — стоп)
unsigned long lastCmdMs = 0;
const unsigned long CMD_TIMEOUT_MS = 2500;

// ==== In-memory log buffer for web console ====
const int LOG_CAPACITY = 140;
const int LOG_LINE_LEN = 120;

struct LogEntry {
  uint32_t id;
  char line[LOG_LINE_LEN];
};

LogEntry logs[LOG_CAPACITY];
int logWritePos = 0;
int logCount = 0;
uint32_t nextLogId = 0;

void addLog(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  Serial.println(line);

  LogEntry &entry = logs[logWritePos];
  entry.id = ++nextLogId;
  strncpy(entry.line, line, LOG_LINE_LEN - 1);
  entry.line[LOG_LINE_LEN - 1] = '\0';

  logWritePos = (logWritePos + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) {
    logCount++;
  }
}

// --- helpers ---
void stopMotors() {
  dirA = 0;
  dirB = 0;
  ledcWrite(PWMA, 0);
  ledcWrite(PWMB, 0);
}

void apply() {
  // A
  if (dirA == 0) {
    ledcWrite(PWMA, 0);
  } else {
    digitalWrite(AIN1, dirA > 0);
    digitalWrite(AIN2, dirA < 0);
    ledcWrite(PWMA, speedVal);
  }

  // B
  if (dirB == 0) {
    ledcWrite(PWMB, 0);
  } else {
    digitalWrite(BIN1, dirB > 0);
    digitalWrite(BIN2, dirB < 0);
    ledcWrite(PWMB, speedVal);
  }
}

bool executeCommand(char c) {
  if (c == 'w') {
    dirA = +1;
    dirB = +1;
    apply();
    return true;
  }
  if (c == 's') {
    dirA = -1;
    dirB = -1;
    apply();
    return true;
  }
  if (c == 'a') {
    dirA = -1;
    dirB = +1;
    apply();
    return true;
  }
  if (c == 'd') {
    dirA = +1;
    dirB = -1;
    apply();
    return true;
  }
  if (c == 'x') {
    dirA = 0;
    dirB = 0;
    apply();
    return true;
  }
  if (c == '+') {
    speedVal = min(255, speedVal + 20);
    apply();
    return true;
  }
  if (c == '-') {
    speedVal = max(0, speedVal - 20);
    apply();
    return true;
  }
  if (c == '!') {
    return true;  // keepalive only
  }
  return false;
}

// ADC -> оценка напряжения батареи (усреднение для более стабильного значения)
float readBatteryVoltage() {
  if (!BAT_MONITOR_ENABLED) return 0.0f;

  const int samples = 4;
  uint32_t acc = 0;
  for (int i = 0; i < samples; ++i) {
    acc += analogRead(BAT_ADC_PIN);
  }
  float raw = static_cast<float>(acc) / samples;
  float v_adc = (raw / 4095.0f) * 3.3f;            // напряжение на пине
  float v_bat = v_adc * ((R_TOP + R_BOT) / R_BOT); // обратно через делитель
  return v_bat;
}

// --- Web UI (удержание кнопок) ---
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<style>
body{font-family:sans-serif;margin:16px}
.btn{width:30vw;height:18vw;font-size:7vw;margin:6px}
.row{display:flex;justify-content:center}
.small{font-size:5vw}
#st{margin:8px 0}
#log{height:34vh;overflow:auto;background:#111;color:#d4ffd4;border-radius:8px;padding:8px;font:12px/1.35 monospace;white-space:pre-wrap}
*{ user-select:none; -webkit-user-select:none; -ms-user-select:none; }
.btn{ touch-action:none; -webkit-touch-callout:none; }
</style>
</head><body>
<h2>RC Car</h2>

<div class="row"><button class="btn" id="w">W</button></div>
<div class="row">
  <button class="btn" id="a">A</button>
  <button class="btn" id="x">STOP</button>
  <button class="btn" id="d">D</button>
</div>
<div class="row"><button class="btn" id="s">S</button></div>

<div class="row">
  <button class="btn small" onclick="send('+')">+ speed</button>
  <button class="btn small" onclick="send('-')">- speed</button>
</div>

<p id="st">...</p>
<h3>ESP log</h3>
<pre id="log">waiting...</pre>

<script>
let holdTimer = null;
let holding = null;
const HOLD_HEARTBEAT_MS = 400;
let lastLogId = 0;
let logLines = [];
const LOG_MAX_LINES = 120;

async function send(c){
  const u = '/cmd?c=' + encodeURIComponent(c) + '&t=' + Date.now();
  await fetch(u, { cache: 'no-store' }).catch(()=>{});
}

function startHold(c){
  if (holding === c) return;
  if (holding !== null) stopHold(true); // смена направления: явно стоп, затем новая команда
  holding = c;
  send(c); // сразу
  holdTimer = setInterval(()=>send('!'), HOLD_HEARTBEAT_MS); // keepalive без повторной команды движения
}

function stopHold(withStop = true){
  if (holdTimer) clearInterval(holdTimer);
  holdTimer = null;
  holding = null;
  if (withStop) send('x'); // стоп
}

function bindHold(id, c){
  const b = document.getElementById(id);
  b.addEventListener('pointerdown', (e)=>{ e.preventDefault(); b.setPointerCapture(e.pointerId); startHold(c); });
  b.addEventListener('pointerup', (e)=>{ e.preventDefault(); stopHold(); });
  b.addEventListener('pointercancel', (e)=>{ e.preventDefault(); stopHold(); });
}

function appendLogLine(line){
  logLines.push(line);
  if (logLines.length > LOG_MAX_LINES) logLines = logLines.slice(logLines.length - LOG_MAX_LINES);
  const box = document.getElementById('log');
  box.textContent = logLines.join('\n');
  box.scrollTop = box.scrollHeight;
}

async function pullLogs(){
  if (holding) return; // уменьшаем сетевую конкуренцию во время удержания
  const u = '/logs?since=' + lastLogId + '&t=' + Date.now();
  const r = await fetch(u, { cache: 'no-store' }).catch(()=>null);
  if (!r) return;
  const t = await r.text();
  if (!t.trim()) return;

  for (const line of t.split('\n')) {
    if (!line) continue;
    const sep = line.indexOf('|');
    if (sep < 0) continue;
    const id = Number(line.slice(0, sep));
    const msg = line.slice(sep + 1);
    if (!Number.isFinite(id)) continue;
    if (id > lastLogId) lastLogId = id;
    appendLogLine(msg);
  }
}

bindHold('w', 'w');
bindHold('a', 'a');
bindHold('s', 's');
bindHold('d', 'd');
document.getElementById('x').addEventListener('pointerdown', (e)=>{ e.preventDefault(); stopHold(); });

// статус
async function upd(){
  if (holding) return; // не конкурируем с командами движения при удержании
  const t = await fetch('/status?t=' + Date.now(), { cache: 'no-store' }).catch(()=>null);
  if (!t) return;
  document.getElementById('st').innerText = await t.text();
}

setInterval(upd, 1000);
setInterval(pullLogs, 800);
upd();
pullLogs();
</script>
</body></html>
)HTML";

void handleRoot() {
  server.send(200, "text/html", PAGE);
}

void handleCmd() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "no c");
    return;
  }

  char c = server.arg("c")[0];
  unsigned long now = millis();
  unsigned long gap = now - lastCmdMs;
  bool ok = executeCommand(c);

  if (!ok) {
    addLog("%lu WEB unknown cmd=%c", now, c);
    server.send(400, "text/plain", "bad c");
    return;
  }

  lastCmdMs = now;

  static unsigned long lastKeepaliveLogMs = 0;
  if (c == '!') {
    if (now - lastKeepaliveLogMs >= 2000) {
      addLog("%lu WEB keepalive gap=%lu sta=%d", now, gap, WiFi.softAPgetStationNum());
      lastKeepaliveLogMs = now;
    }
  } else {
    addLog("%lu WEB cmd=%c gap=%lu dirA=%d dirB=%d spd=%d sta=%d",
           now, c, gap, dirA, dirB, speedVal, WiFi.softAPgetStationNum());
  }

  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  float vb = readBatteryVoltage();
  String s;
  s.reserve(130);
  s += "dirA=";
  s += dirA;
  s += " dirB=";
  s += dirB;
  s += " speed=";
  s += speedVal;
  s += " age=";
  s += (millis() - lastCmdMs);
  s += "ms sta=";
  s += WiFi.softAPgetStationNum();
  if (BAT_MONITOR_ENABLED) {
    s += " batt=";
    s += String(vb, 2);
    s += "V";
  } else {
    s += " batt=off";
  }
  server.send(200, "text/plain", s);
}

void handleLogs() {
  uint32_t since = 0;
  if (server.hasArg("since")) {
    since = static_cast<uint32_t>(server.arg("since").toInt());
  }

  String out;
  out.reserve(2400);

  const int maxLinesPerResponse = 60;
  int sent = 0;
  int start = (logWritePos - logCount + LOG_CAPACITY) % LOG_CAPACITY;

  for (int i = 0; i < logCount; ++i) {
    int idx = (start + i) % LOG_CAPACITY;
    const LogEntry &entry = logs[idx];
    if (entry.id <= since) continue;
    out += String(entry.id);
    out += "|";
    out += entry.line;
    out += "\n";
    sent++;
    if (sent >= maxLinesPerResponse) break;
  }

  server.send(200, "text/plain", out);
}

void setup() {
  Serial.begin(115200);

  pinMode(STBY, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  digitalWrite(STBY, HIGH);

  ledcAttach(PWMA, 1000, 8);
  ledcAttach(PWMB, 1000, 8);

  analogReadResolution(12);

  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP("RC-CAR", "12345678");
  IPAddress ip = WiFi.softAPIP();
  String ipStr = ip.toString();

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/status", handleStatus);
  server.on("/logs", handleLogs);
  server.begin();

  stopMotors();
  apply();

  lastCmdMs = millis();

  addLog("%lu BOOT AP=%s IP=%s timeout=%lums", millis(), apOk ? "ok" : "fail", ipStr.c_str(), CMD_TIMEOUT_MS);
  addLog("%lu BAT monitor=%s pin=%d warn=%.2f cutoff=%.2f",
         millis(), BAT_MONITOR_ENABLED ? "on" : "off", BAT_ADC_PIN, BAT_WARN, BAT_CUTOFF);
}

unsigned long lastBatMs = 0;
unsigned long lastBatWarnLogMs = 0;
unsigned long lastBatDiagLogMs = 0;
unsigned long lastStaCheckMs = 0;
bool cutoff = false;
int cutoffLowStreak = 0;
int lastStationCount = -1;

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // консоль тоже оставим
  if (Serial.available()) {
    char c = Serial.read();
    unsigned long gap = now - lastCmdMs;
    bool ok = executeCommand(c);
    if (ok) {
      lastCmdMs = now;
      if (c != '!') {
        addLog("%lu SERIAL cmd=%c gap=%lu dirA=%d dirB=%d spd=%d",
               now, c, gap, dirA, dirB, speedVal);
      }
    } else {
      addLog("%lu SERIAL unknown cmd=%c", now, c);
    }
  }

  // диагностика AP-клиентов
  if (now - lastStaCheckMs >= 1000) {
    lastStaCheckMs = now;
    int sta = WiFi.softAPgetStationNum();
    if (sta != lastStationCount) {
      lastStationCount = sta;
      addLog("%lu AP stations=%d", now, sta);
    }
  }

  // контроль батареи (раз в 300 мс)
  if (BAT_MONITOR_ENABLED && (now - lastBatMs > 300)) {
    lastBatMs = now;
    float vb = readBatteryVoltage();

    // Диагностика плавающего/неподключенного входа или неверного делителя.
    if ((vb < 1.0f || vb > 12.8f) && (now - lastBatDiagLogMs > 3000)) {
      lastBatDiagLogMs = now;
      addLog("%lu BAT suspicious %.2fV (check BAT_ADC_PIN wiring/divider)", now, vb);
    }

    if (!cutoff) {
      if (vb <= BAT_CUTOFF) {
        cutoffLowStreak++;
      } else {
        cutoffLowStreak = 0;
      }
    }

    // if (!cutoff && cutoffLowStreak >= 3) {
    //   cutoff = true;
    //   stopMotors();
    //   addLog("%lu BAT CUTOFF %.2fV (streak=%d)", now, vb, cutoffLowStreak);
    // } else if (!cutoff && vb <= BAT_WARN && (now - lastBatWarnLogMs > 3000)) {
    //   lastBatWarnLogMs = now;
    //   addLog("%lu BAT LOW %.2fV", now, vb);
    // }

    if (cutoff) {
      dirA = 0;
      dirB = 0;
      ledcWrite(PWMA, 0);
      ledcWrite(PWMB, 0);
    }
  }

  // dead-man switch
  if (!cutoff && (now - lastCmdMs > CMD_TIMEOUT_MS)) {
    if (dirA != 0 || dirB != 0) {
      addLog("%lu DEADMAN stop age=%lums", now, now - lastCmdMs);
      dirA = 0;
      dirB = 0;
      apply();
    }
  }
}
