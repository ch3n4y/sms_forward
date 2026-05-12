#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

#define RX_PIN 20
#define TX_PIN 21
#define UART_BAUD 115200
#define HTTP_PORT 80
#define WS_PORT 81
#define MAX_RAW_LOG_CHARS 12000
#define HOST_TIME_SYNC_INTERVAL_MS (60UL * 1000UL)
#define STATUS_POLL_INTERVAL_MS (5UL * 1000UL)
#define WIFI_RETRY_COUNT 20
#define WIFI_RETRY_DELAY_MS 500

WebServer server(HTTP_PORT);
WebSocketsServer webSocket = WebSocketsServer(WS_PORT);
Preferences preferences;

struct DeviceState {
  String phoneNumber;
  String identityType;
  String operatorName;
  String signalStrength;
  String temperature;
  String poweronReasonZh;
  String updatedAt;
};

struct BarkConfig {
  bool enabled = false;
  String api;
  String key;
};

DeviceState deviceState;
BarkConfig barkConfig;
String uartLineBuffer;
String rawLogBuffer;
String lastStatusMessage;
String webPassword;
String sessionToken;
unsigned long lastTimeSyncMs = 0;
unsigned long lastStatusPollMs = 0;
bool downstreamOnline = false;
unsigned long lastDownstreamMessageMs = 0;
unsigned long rawLogSeq = 0;
unsigned long deviceStateSeq = 0;
unsigned long statusSeq = 0;

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 32);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

String extractJsonString(const String &json, const String &key) {
  String marker = "\"" + key + "\"";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) return "";
  int colonPos = json.indexOf(':', keyPos + marker.length());
  if (colonPos < 0) return "";
  int startQuote = json.indexOf('"', colonPos + 1);
  if (startQuote < 0) return "";

  String value;
  bool escaping = false;
  for (int i = startQuote + 1; i < (int)json.length(); ++i) {
    char c = json[i];
    if (escaping) {
      switch (c) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        default: value += c; break;
      }
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    if (c == '"') {
      return value;
    }
    value += c;
  }
  return value;
}

bool extractJsonBool(const String &json, const String &key, bool defaultValue) {
  String marker = "\"" + key + "\"";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) return defaultValue;
  int colonPos = json.indexOf(':', keyPos + marker.length());
  if (colonPos < 0) return defaultValue;
  String tail = json.substring(colonPos + 1);
  tail.trim();
  if (tail.startsWith("true")) return true;
  if (tail.startsWith("false")) return false;
  return defaultValue;
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

String getCurrentTimeString() {
  time_t now = time(nullptr);
  if (now < 100000) return String(millis() / 1000) + "s";
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void appendRawLog(const String &line) {
  String record = "[" + getCurrentTimeString() + "] " + line + "\n";
  rawLogBuffer += record;
  if (rawLogBuffer.length() > MAX_RAW_LOG_CHARS) {
    rawLogBuffer.remove(0, rawLogBuffer.length() - MAX_RAW_LOG_CHARS);
  }
  rawLogSeq++;
}

void wsBroadcast(const String &type, const String &payloadJson) {
  String msg = "{\"type\":\"" + type + "\"," + payloadJson + "}";
  webSocket.broadcastTXT(msg);
}

void publishRawLog(const String &line) {
  appendRawLog(line);
  wsBroadcast("raw_log", "\"line\":\"" + jsonEscape("[" + getCurrentTimeString() + "] " + line) + "\",\"seq\":" + String(rawLogSeq));
}

String deviceStateJson() {
  String statusText = downstreamOnline ? "在线" : "离线";
  return String("{") +
         "\"phone_number\":\"" + jsonEscape(deviceState.phoneNumber) + "\"," +
         "\"identity_type\":\"" + jsonEscape(deviceState.identityType) + "\"," +
         "\"operator\":\"" + jsonEscape(deviceState.operatorName) + "\"," +
         "\"signal_strength\":\"" + jsonEscape(deviceState.signalStrength) + "\"," +
         "\"temperature\":\"" + jsonEscape(deviceState.temperature) + "\"," +
         "\"poweron_reason_zh\":\"" + jsonEscape(deviceState.poweronReasonZh) + "\"," +
         "\"status\":\"" + jsonEscape(statusText) + "\"," +
         "\"updated_at\":\"" + jsonEscape(deviceState.updatedAt) + "\"" +
         "}";
}

void publishDeviceState() {
  deviceStateSeq++;
  wsBroadcast("device_state", String("\"state\":") + deviceStateJson() + ",\"seq\":" + String(deviceStateSeq));
}

void publishStatusMessage(const String &text) {
  lastStatusMessage = text;
  statusSeq++;
  Serial.println(text);
  wsBroadcast("status", "\"text\":\"" + jsonEscape(text) + "\",\"seq\":" + String(statusSeq));
  publishRawLog("[STATUS] " + text);
}

void persistBarkConfig() {
  preferences.putBool("bark_enabled", barkConfig.enabled);
  preferences.putString("bark_api", barkConfig.api);
  preferences.putString("bark_key", barkConfig.key);
}

void loadBarkConfig() {
  preferences.begin("smsnotify", false);
  barkConfig.enabled = preferences.getBool("bark_enabled", false);
  barkConfig.api = preferences.getString("bark_api", "");
  barkConfig.key = preferences.getString("bark_key", "");
  webPassword = preferences.getString("web_password", "123456");
  if (webPassword.isEmpty()) {
    webPassword = "123456";
    preferences.putString("web_password", webPassword);
  }
}

void persistWebPassword() {
  preferences.putString("web_password", webPassword);
}

String generateSessionToken() {
  char buf[24];
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(buf, sizeof(buf), "%08lx%08lx", (unsigned long)a, (unsigned long)b);
  return String(buf);
}

String getCookieValue(const String &name) {
  if (!server.hasHeader("Cookie")) return "";
  String cookie = server.header("Cookie");
  String key = name + "=";
  int start = cookie.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

bool isAuthenticated() {
  if (sessionToken.isEmpty()) return false;
  return getCookieValue("ESPSESSION") == sessionToken;
}

bool ensureAuthenticated() {
  if (isAuthenticated()) return true;
  sendNoCacheHeaders();
  server.send(401, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"unauthorized\"}");
  return false;
}

String buildBarkUrl(const String &api, const String &key) {
  String normalizedApi = api;
  normalizedApi.trim();
  while (normalizedApi.endsWith("/")) normalizedApi.remove(normalizedApi.length() - 1);
  String normalizedKey = key;
  normalizedKey.trim();
  if (normalizedApi.length() == 0) return "";
  if (normalizedKey.length() == 0) return normalizedApi;
  if (normalizedApi.endsWith("/" + normalizedKey)) return normalizedApi;
  return normalizedApi + "/" + normalizedKey;
}

String buildBarkPayload(const String &message) {
  String title = "Air724UG 通知";
  if (message.indexOf("#SMS") >= 0 || message.indexOf("#sms") >= 0) title = "Air724UG 短信";
  else if (message.indexOf("#CALL") >= 0 || message.indexOf("#call") >= 0) title = "Air724UG 来电";
  String subtitle = message;
  int newlinePos = subtitle.indexOf('\n');
  if (newlinePos >= 0) subtitle = subtitle.substring(0, newlinePos);
  if (subtitle.length() > 80) subtitle = subtitle.substring(0, 80);
  return String("{") +
         "\"title\":\"" + jsonEscape(title) + "\"," +
         "\"subtitle\":\"" + jsonEscape(subtitle) + "\"," +
         "\"body\":\"" + jsonEscape(message) + "\"," +
         "\"group\":\"air724ug\"," +
         "\"isArchive\":\"1\"}";
}

bool shouldForwardBark(const String &message) {
  if (!barkConfig.enabled || barkConfig.api.isEmpty() || barkConfig.key.isEmpty()) return false;
  return message.indexOf("#SMS") >= 0 || message.indexOf("#sms") >= 0 || message.indexOf("#CALL") >= 0 || message.indexOf("#call") >= 0;
}

void sendBark(const String &message, bool forceSend = false) {
  if (!forceSend && !shouldForwardBark(message)) return;
  String url = buildBarkUrl(barkConfig.api, barkConfig.key);
  if (url.isEmpty()) {
    publishRawLog("[BARK] 配置为空，跳过发送");
    return;
  }
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  int code = http.POST(buildBarkPayload(message));
  String response = http.getString();
  if (code > 0 && code < 400) publishRawLog("[BARK] 转发成功 (" + String(code) + ")");
  else publishRawLog("[BARK] 转发失败 (" + String(code) + ") " + response);
  http.end();
}

void writeDownstreamLine(const String &line) {
  Serial1.print(line);
  Serial1.print("\r\n");
  Serial1.flush();
  publishRawLog("[UART_TX] " + line);
}

void sendCustomCommand(const String &line) {
  String payload = line;
  payload.trim();
  if (payload.isEmpty()) {
    publishStatusMessage("忽略空命令");
    return;
  }
  writeDownstreamLine(payload);
}

void sendTimeSync() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (!localtime_r(&now, &timeinfo)) return;
  String payload = String("{") +
                   "\"cmd\":\"sync_time\"," +
                   "\"source\":\"esp32_web\"," +
                   "\"clock\":{" +
                   "\"year\":" + String(timeinfo.tm_year + 1900) + "," +
                   "\"month\":" + String(timeinfo.tm_mon + 1) + "," +
                   "\"day\":" + String(timeinfo.tm_mday) + "," +
                   "\"hour\":" + String(timeinfo.tm_hour) + "," +
                   "\"min\":" + String(timeinfo.tm_min) + "," +
                   "\"sec\":" + String(timeinfo.tm_sec) + "}," +
                   "\"host_timestamp\":\"" + jsonEscape(getCurrentTimeString()) + "\"}";
  writeDownstreamLine(payload);
}

void requestDeviceStatus() { writeDownstreamLine("{\"cmd\":\"get_status\"}"); }
void sendRebootCommand() { writeDownstreamLine("{\"cmd\":\"reboot\"}"); }

void clearDeviceState() {
  deviceState.phoneNumber = "";
  deviceState.identityType = "";
  deviceState.operatorName = "";
  deviceState.signalStrength = "";
  deviceState.temperature = "";
  deviceState.poweronReasonZh = "";
  deviceState.updatedAt = "";
}

void handleIncomingJsonLine(const String &line) {
  String event = extractJsonString(line, "event");
  String message = extractJsonString(line, "message");
  String channel = extractJsonString(line, "channel");
  String timestamp = extractJsonString(line, "timestamp");
  if (event == "get_status") {
    deviceState.phoneNumber = extractJsonString(line, "phone_number");
    deviceState.identityType = extractJsonString(line, "identity_type");
    deviceState.operatorName = extractJsonString(line, "operator");
    deviceState.signalStrength = extractJsonString(line, "signal_strength");
    deviceState.temperature = extractJsonString(line, "temperature");
    deviceState.poweronReasonZh = extractJsonString(line, "poweron_reason_zh");
    deviceState.updatedAt = timestamp.length() ? timestamp : getCurrentTimeString();
    downstreamOnline = true;
    lastDownstreamMessageMs = millis();
    publishDeviceState();
  }
  if (channel == "usb_uart" && message.length() && shouldForwardBark(message)) sendBark(message);
}

void handleIncomingLine(const String &line) {
  if (line.length() == 0) return;
  publishRawLog("[UART_RX] " + line);
  downstreamOnline = true;
  lastDownstreamMessageMs = millis();
  if (line.startsWith("{")) handleIncomingJsonLine(line);
}

void handleUartReceive() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = uartLineBuffer;
      uartLineBuffer = "";
      line.trim();
      handleIncomingLine(line);
    } else {
      uartLineBuffer += c;
      if (uartLineBuffer.length() > 768) {
        handleIncomingLine(uartLineBuffer);
        uartLineBuffer = "";
      }
    }
  }
}

void syncNtpTime() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");
  publishStatusMessage("已启动 NTP 校时");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  Serial.print("[WiFi] 正在尝试连接记忆的 WiFi 网络");
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < WIFI_RETRY_COUNT) {
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
    retryCount++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] 重连记忆网络成功！");
    return;
  }
  Serial.println("\n[WiFi] 未能连上记忆网络，自动进入配网模式！");
  Serial.println("[System] 请打开 ESPTouch App 进行配网...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.beginSmartConfig();
  while (!WiFi.smartConfigDone()) {
    delay(500);
    Serial.print("#");
  }
  Serial.println("\n[SmartConfig] 成功收到路由器信息！正在连接...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  WiFi.mode(WIFI_STA);
  Serial.println("\n[WiFi] 新网络连接成功！密码已自动保存。");
}

String loginPage() {
  return R"rawliteral(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Air724UG 登录</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#0f1115;color:#e8eaed;font-family:"Microsoft YaHei UI",sans-serif}.card{width:min(92vw,420px);background:#171a21;border:1px solid #2a2f3a;border-radius:16px;padding:24px}.card h2{margin:0 0 8px}.muted{color:#94a3b8;font-size:13px;margin-bottom:16px}input,button{width:100%;padding:12px;border-radius:10px;border:1px solid #394150;background:#0d1117;color:#e8eaed;font-size:15px}button{background:#2563eb;border-color:#2563eb;cursor:pointer;margin-top:12px}.msg{margin-top:12px;padding:10px 12px;border-radius:10px;background:#111827;color:#cbd5e1;min-height:20px}.err{background:#3a1616;color:#fecaca}</style></head><body><div class="card"><h2>Air724UG Web Monitor</h2><div class="muted">请输入访问密码</div><input id="password" type="password" placeholder="访问密码" autofocus><button onclick="login()">登录</button><div id="msg" class="msg">未登录</div></div><script>async function login(){const pwd=document.getElementById('password').value;const params=new URLSearchParams();params.set('password',pwd);const msg=document.getElementById('msg');msg.textContent='登录中...';msg.className='msg';try{const res=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:params.toString(),cache:'no-store'});const data=await res.json();if(!res.ok||!data.ok)throw new Error(data.error||('HTTP '+res.status));msg.textContent='登录成功，正在跳转...';location.href='/';}catch(e){msg.textContent='登录失败：'+e.message;msg.className='msg err';}}</script></body></html>
)rawliteral";
}

String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Air724UG Web Monitor</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;font-family:"Microsoft YaHei UI",sans-serif;background:#0f1115;color:#e8eaed}.wrap{max-width:1180px;margin:0 auto;padding:16px}.top{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap}.badge{padding:6px 10px;border-radius:999px;background:#1f2937;color:#cbd5e1;font-size:12px}.ok{background:#12361f;color:#86efac}.bad{background:#3a1616;color:#fca5a5}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;margin-top:16px}.card{background:#171a21;border:1px solid #2a2f3a;border-radius:12px;padding:14px}.card h3{margin:0 0 10px;font-size:15px}.kv{display:grid;grid-template-columns:88px 1fr;gap:8px;font-size:14px;line-height:1.7}input,button,textarea{border-radius:8px;border:1px solid #394150;background:#0d1117;color:#e8eaed;padding:10px;font-size:14px}input,textarea{width:100%}button{cursor:pointer;background:#2563eb;border-color:#2563eb}button.secondary{background:#374151;border-color:#374151}button.warn{background:#b91c1c;border-color:#b91c1c}button:disabled{opacity:.55;cursor:not-allowed}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}.status-line{margin-top:12px;padding:10px 12px;border-radius:10px;background:#111827;color:#cbd5e1;font-size:13px}.status-line.error{background:#3a1616;color:#fecaca}.status-line.ok{background:#12361f;color:#bbf7d0}#raw{height:360px;overflow:auto;white-space:pre-wrap;word-break:break-all;background:#05070b;border:1px solid #2a2f3a;border-radius:12px;padding:12px;font-family:Consolas,monospace}.muted{color:#94a3b8;font-size:12px}textarea{min-height:96px;resize:vertical}</style></head><body><div class="wrap"><div class="top"><div><h2 style="margin:0">Air724UG Web Monitor</h2><div class="muted">ESP32-C3 UART 网关 · GPIO20 RX / GPIO21 TX</div></div><div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center"><span class="badge" id="wifiState">WiFi -</span><span class="badge bad" id="wsState">WebSocket 未连接</span><span class="badge" id="deviceStateBadge">下位机离线</span><button class="secondary" style="padding:8px 12px" onclick="logout()">退出登录</button></div></div><div id="statusLine" class="status-line">页面已加载，等待设备状态...</div><div class="grid"><div class="card"><h3>设备状态</h3><div class="kv"><div>号码/ICCID</div><div id="phone">-</div><div>身份类型</div><div id="identity">-</div><div>运营商</div><div id="operator">-</div><div>信号</div><div id="signal">-</div><div>温度</div><div id="temp">-</div><div>启动原因</div><div id="reason">-</div><div>更新时间</div><div id="updatedAt">-</div></div><div class="actions"><button id="btnRefresh" onclick="refreshStatus()">刷新状态</button><button id="btnSyncTime" class="secondary" onclick="syncTimeNow()">同步时间</button><button id="btnReboot" class="warn" onclick="rebootDownstream()">重启下位机</button></div></div><div class="card"><h3>Bark 配置</h3><label class="muted"><input type="checkbox" id="barkEnabled" style="width:auto;margin-right:8px">启用短信/来电转发</label><div style="height:10px"></div><input id="barkApi" placeholder="BARK_API，例如 https://bark.256404.xyz"><div style="height:10px"></div><input id="barkKey" placeholder="BARK_KEY"><div class="actions"><button id="btnSaveBark" onclick="saveBark()">保存 Bark</button><button id="btnTestBark" class="secondary" onclick="testBark()">测试 Bark</button></div></div><div class="card"><h3>网页密码</h3><div class="muted">修改后立即生效，当前会话保持登录</div><div style="height:10px"></div><input id="newPassword" type="password" placeholder="新密码"><div style="height:10px"></div><input id="confirmPassword" type="password" placeholder="确认新密码"><div class="actions"><button id="btnSavePassword" onclick="savePassword()">保存密码</button></div></div><div class="card"><h3>原始命令</h3><div class="muted">用于联调下位机，支持直接发送 JSON 行</div><div style="height:10px"></div><textarea id="commandText" spellcheck="false">{&quot;cmd&quot;:&quot;get_status&quot;}</textarea><div class="actions"><button id="btnSendCommand" onclick="sendCommand()">发送命令</button><button class="secondary" onclick="presetCommand('status')">填入 get_status</button><button class="secondary" onclick="presetCommand('time')">填入 sync_time</button></div></div></div><div class="card" style="margin-top:16px"><div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap"><h3 style="margin:0">原始日志</h3><div class="actions" style="margin-top:0"><button class="secondary" onclick="clearRawLog()">清空日志</button></div></div><div class="muted" style="margin:8px 0 12px">显示 UART 收发、状态消息和 Bark 结果</div><div id="raw"></div></div></div><script>let ws;let rawSeq=0;let deviceSeq=0;let statusSeq=0;let actionBusy=0;function setText(id,val){document.getElementById(id).textContent=val||'-';}function setStatus(text,kind=''){const el=document.getElementById('statusLine');el.textContent=text||'';el.className='status-line'+(kind?(' '+kind):'');}function setBusy(flag){actionBusy+=flag?1:-1;if(actionBusy<0)actionBusy=0;document.querySelectorAll('button').forEach(btn=>btn.disabled=actionBusy>0&&btn.textContent!=='退出登录');}function setRawText(text){const box=document.getElementById('raw');box.textContent=text||'';box.scrollTop=box.scrollHeight;}function appendRaw(line,seq){if(seq&&seq<=rawSeq)return;if(seq)rawSeq=seq;const box=document.getElementById('raw');box.textContent+=(box.textContent?'\n':'')+line;box.scrollTop=box.scrollHeight;}function setDeviceState(state,seq){if(seq&&seq<deviceSeq)return;if(seq)deviceSeq=seq;setText('phone',state.phone_number);setText('identity',state.identity_type);setText('operator',state.operator);setText('signal',state.signal_strength);setText('temp',state.temperature);setText('reason',state.poweron_reason_zh);setText('updatedAt',state.updated_at);const badge=document.getElementById('deviceStateBadge');badge.textContent='下位机'+(state.status||'-');badge.className='badge '+((state.status||'').includes('在线')?'ok':'bad');}async function api(path,opts={}){opts.cache='no-store';const res=await fetch(path,opts);let data={ok:false};try{data=await res.json();}catch(e){}if(res.status===401){location.href='/';throw new Error('未登录或会话已失效');}if(!res.ok||data.ok===false&&data.error){throw new Error(data.error||('HTTP '+res.status));}return data;}async function runAction(text,fn){setBusy(true);setStatus(text);try{return await fn();}catch(e){setStatus('操作失败：'+(e&&e.message?e.message:e),'error');throw e;}finally{setBusy(false);}}function applySnapshot(data){if(data.raw_seq&&data.raw_seq>=rawSeq){rawSeq=data.raw_seq;setRawText(data.raw_log||'');}if(data.status_seq&&data.status_seq>=statusSeq){statusSeq=data.status_seq;setStatus(data.last_status||'已同步快照',data.last_status?'ok':'');}if(data.device_state)setDeviceState(data.device_state,data.device_seq||0);document.getElementById('wifiState').textContent='WiFi '+(data.wifi_ip||'-');document.getElementById('barkEnabled').checked=!!(data.bark&&data.bark.enabled);document.getElementById('barkApi').value=(data.bark&&data.bark.api)||'';document.getElementById('barkKey').value=(data.bark&&data.bark.key)||'';}async function loadSnapshot(){const data=await api('/api/snapshot');applySnapshot(data);}async function refreshStatus(){await runAction('正在发送状态请求...',()=>api('/api/action/status',{method:'POST'}));setStatus('已发送状态请求，等待下位机回复','ok');}async function syncTimeNow(){await runAction('正在同步时间...',()=>api('/api/action/sync-time',{method:'POST'}));setStatus('已发送时间同步命令','ok');}async function rebootDownstream(){if(!confirm('确认重启下位机？'))return;await runAction('正在发送重启命令...',()=>api('/api/action/reboot',{method:'POST'}));setStatus('已发送重启命令','ok');}async function clearRawLog(){await runAction('正在清空日志...',()=>api('/api/raw/clear',{method:'POST'}));rawSeq=0;setRawText('');setStatus('日志已清空','ok');}async function saveBark(){const params=new URLSearchParams();params.set('enabled',document.getElementById('barkEnabled').checked?'true':'false');params.set('api',document.getElementById('barkApi').value.trim());params.set('key',document.getElementById('barkKey').value.trim());await runAction('正在保存 Bark 配置...',()=>api('/api/bark/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:params.toString()}));await loadSnapshot();setStatus('Bark 配置已保存','ok');}async function testBark(){await runAction('正在发送 Bark 测试...',()=>api('/api/bark/test',{method:'POST'}));await loadSnapshot();setStatus('Bark 测试已触发，请查看日志','ok');}async function savePassword(){const p1=document.getElementById('newPassword').value.trim();const p2=document.getElementById('confirmPassword').value.trim();if(!p1){setStatus('新密码不能为空','error');return;}if(p1!==p2){setStatus('两次输入的新密码不一致','error');return;}const params=new URLSearchParams();params.set('password',p1);await runAction('正在保存网页密码...',()=>api('/api/auth/password',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:params.toString()}));document.getElementById('newPassword').value='';document.getElementById('confirmPassword').value='';setStatus('网页密码已更新','ok');}function presetCommand(type){if(type==='status'){document.getElementById('commandText').value='{\"cmd\":\"get_status\"}';return;}const d=new Date();document.getElementById('commandText').value=JSON.stringify({cmd:'sync_time',source:'esp32_web',clock:{year:d.getFullYear(),month:d.getMonth()+1,day:d.getDate(),hour:d.getHours(),min:d.getMinutes(),sec:d.getSeconds()},host_timestamp:d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')+' '+String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')+':'+String(d.getSeconds()).padStart(2,'0')});}async function sendCommand(){const line=document.getElementById('commandText').value.trim();if(!line){setStatus('命令不能为空','error');return;}const params=new URLSearchParams();params.set('line',line);await runAction('正在发送原始命令...',()=>api('/api/action/send-command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:params.toString()}));setStatus('原始命令已发送','ok');}async function logout(){try{await fetch('/api/auth/logout',{method:'POST',cache:'no-store'});}catch(e){}location.href='/';}function initWs(){ws=new WebSocket(`ws://${location.hostname}:81/`);ws.onopen=()=>{const el=document.getElementById('wsState');el.textContent='WebSocket 已连接';el.className='badge ok';};ws.onclose=()=>{const el=document.getElementById('wsState');el.textContent='WebSocket 断开，重连中';el.className='badge bad';setTimeout(initWs,2000);};ws.onmessage=(ev)=>{try{const msg=JSON.parse(ev.data);if(msg.type==='raw_log')appendRaw(msg.line,msg.seq||0);else if(msg.type==='device_state')setDeviceState(msg.state||{},msg.seq||0);else if(msg.type==='status'){if((msg.seq||0)>=statusSeq){statusSeq=msg.seq||statusSeq;setStatus(msg.text||'','ok');}}}catch(e){appendRaw(ev.data,0);}};}loadSnapshot().catch(e=>setStatus('加载初始数据失败：'+e.message,'error'));initWs();</script></body></html>
)rawliteral";
}

void handleRoot() {
  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", isAuthenticated() ? htmlPage() : loginPage());
}
void handleAuthLogin() {
  String password = server.hasArg("password") ? server.arg("password") : server.arg("plain");
  password.trim();
  if (password != webPassword) {
    sendNoCacheHeaders();
    server.send(401, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"password incorrect\"}");
    return;
  }
  sessionToken = generateSessionToken();
  sendNoCacheHeaders();
  server.sendHeader("Set-Cookie", "ESPSESSION=" + sessionToken + "; Path=/; HttpOnly; SameSite=Lax");
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}
void handleAuthLogout() {
  sessionToken = "";
  sendNoCacheHeaders();
  server.sendHeader("Set-Cookie", "ESPSESSION=deleted; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}
void handleAuthPasswordSave() {
  if (!ensureAuthenticated()) return;
  String password = server.hasArg("password") ? server.arg("password") : server.arg("plain");
  password.trim();
  if (password.length() < 4) {
    server.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"password too short\"}");
    return;
  }
  webPassword = password;
  persistWebPassword();
  publishStatusMessage("网页访问密码已更新");
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}
void handleSnapshot() {
  if (!ensureAuthenticated()) return;
  String wifiIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "未连接";
  String json = String("{") + "\"ok\":true," + "\"wifi_ip\":\"" + jsonEscape(wifiIp) + "\"," + "\"raw_log\":\"" + jsonEscape(rawLogBuffer) + "\"," + "\"raw_seq\":" + String(rawLogSeq) + "," + "\"device_state\":" + deviceStateJson() + "," + "\"device_seq\":" + String(deviceStateSeq) + "," + "\"last_status\":\"" + jsonEscape(lastStatusMessage) + "\"," + "\"status_seq\":" + String(statusSeq) + "," + "\"bark\":{" + "\"enabled\":" + String(barkConfig.enabled ? "true" : "false") + "," + "\"api\":\"" + jsonEscape(barkConfig.api) + "\"," + "\"key\":\"" + jsonEscape(barkConfig.key) + "\"}}";
  sendNoCacheHeaders();
  server.send(200, "application/json; charset=utf-8", json);
}
void handleActionStatus() { if (!ensureAuthenticated()) return; requestDeviceStatus(); server.send(200, "application/json", "{\"ok\":true}"); }
void handleActionSyncTime() { if (!ensureAuthenticated()) return; sendTimeSync(); server.send(200, "application/json", "{\"ok\":true}"); }
void handleActionReboot() { if (!ensureAuthenticated()) return; sendRebootCommand(); server.send(200, "application/json", "{\"ok\":true}"); }
void handleActionSendCommand() {
  if (!ensureAuthenticated()) return;
  String line = server.hasArg("line") ? server.arg("line") : server.arg("plain");
  line.trim();
  if (line.isEmpty()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty command\"}");
    return;
  }
  sendCustomCommand(line);
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleRawClear() { if (!ensureAuthenticated()) return; rawLogBuffer = ""; rawLogSeq = 0; server.send(200, "application/json", "{\"ok\":true}"); }
void handleBarkSave() {
  if (!ensureAuthenticated()) return;
  if (server.hasArg("enabled")) {
    barkConfig.enabled = server.arg("enabled") == "true";
    barkConfig.api = server.arg("api");
    barkConfig.key = server.arg("key");
  } else {
    String body = server.arg("plain");
    barkConfig.enabled = extractJsonBool(body, "enabled", false);
    barkConfig.api = extractJsonString(body, "api");
    barkConfig.key = extractJsonString(body, "key");
  }
  barkConfig.api.trim();
  barkConfig.key.trim();
  persistBarkConfig();
  publishStatusMessage("已保存 Bark 配置");
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleBarkTest() { if (!ensureAuthenticated()) return; sendBark("Bark 测试消息\n\n#TEST_BARK", true); server.send(200, "application/json", "{\"ok\":true}"); }
void handleNotFound() { server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}"); }

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[WebSocket] 客户端 #%u 已连接: %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      webSocket.sendTXT(num, String("{\"type\":\"status\",\"text\":\"") + jsonEscape("WebSocket 通道建立成功") + "\",\"seq\":" + String(statusSeq) + "}");
      webSocket.sendTXT(num, String("{\"type\":\"device_state\",\"state\":") + deviceStateJson() + ",\"seq\":" + String(deviceStateSeq) + "}");
      break;
    }
    case WStype_DISCONNECTED: Serial.printf("[WebSocket] 客户端 #%u 已断开\n", num); break;
    default: break;
  }
}

void setupRoutes() {
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/auth/login", HTTP_POST, handleAuthLogin);
  server.on("/api/auth/logout", HTTP_POST, handleAuthLogout);
  server.on("/api/auth/password", HTTP_POST, handleAuthPasswordSave);
  server.on("/api/snapshot", HTTP_GET, handleSnapshot);
  server.on("/api/action/status", HTTP_POST, handleActionStatus);
  server.on("/api/action/sync-time", HTTP_POST, handleActionSyncTime);
  server.on("/api/action/reboot", HTTP_POST, handleActionReboot);
  server.on("/api/action/send-command", HTTP_POST, handleActionSendCommand);
  server.on("/api/raw/clear", HTTP_POST, handleRawClear);
  server.on("/api/bark/save", HTTP_POST, handleBarkSave);
  server.on("/api/bark/test", HTTP_POST, handleBarkTest);
  server.onNotFound(handleNotFound);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(1500);
  clearDeviceState();
  loadBarkConfig();
  Serial.println("\n\n====== ESP32-C3 Air724UG Web Monitor 启动 ======");
  connectWiFi();
  Serial.print("[WiFi] 当前局域网 IP: ");
  Serial.println(WiFi.localIP());
  syncNtpTime();
  setupRoutes();
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  publishStatusMessage("服务已就绪，请在浏览器访问当前 IP 地址");
  sendTimeSync();
  requestDeviceStatus();
  lastTimeSyncMs = millis();
  lastStatusPollMs = millis();
}

void loop() {
  server.handleClient();
  webSocket.loop();
  handleUartReceive();
  unsigned long now = millis();
  if (now - lastTimeSyncMs >= HOST_TIME_SYNC_INTERVAL_MS) { sendTimeSync(); lastTimeSyncMs = now; }
  if (now - lastStatusPollMs >= STATUS_POLL_INTERVAL_MS) { requestDeviceStatus(); lastStatusPollMs = now; }
  if (downstreamOnline && now - lastDownstreamMessageMs > 30000UL) { downstreamOnline = false; publishDeviceState(); }
}
