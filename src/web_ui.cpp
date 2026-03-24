#include "web_ui.h"
#include "bluetooth.h"
#include <esp_wifi.h>
#include <nvs_flash.h>

// ---- HTML helpers ----
static String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

static String renderPage() {
  String page;
  page.reserve(4096);
  page += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>ESP32 A2DP Audio</title><style>");
  page += F("body{font-family:Arial,sans-serif;max-width:760px;margin:24px auto;padding:0 16px;background:#f7f7f7;color:#222}");
  page += F(".card{background:#fff;border-radius:12px;padding:18px;box-shadow:0 2px 10px rgba(0,0,0,.08);margin-bottom:16px}");
  page += F("input,button{font-size:16px;padding:10px 12px;border-radius:10px;border:1px solid #ccc}");
  page += F("button{cursor:pointer;background:#111;color:#fff;border:none;margin-right:8px;margin-top:8px}");
  page += F("button.danger{background:#c0392b}");
  page += F("input[type=range]{width:100%;padding:4px 0;border:none}");
  page += F(".muted{color:#666}.ok{color:#0a7d32}.warn{color:#9a6700}.mono{font-family:monospace}</style></head><body>");

  page += F("<div class='card'><h2>ESP32 A2DP Audio</h2>");
  page += F("<p class='muted'>Stream analog input (GPIO34) to a Bluetooth headset.</p></div>");

  page += F("<div class='card'><h3>Status</h3>");
  page += F("<p><strong>Target:</strong> ");
  page += htmlEscape(targetDeviceName.length() ? targetDeviceName : String("(not set)"));
  page += F("</p><p><strong>Bluetooth:</strong> <span id='bt'>");
  page += htmlEscape(lastBtState);
  page += F("</span></p><p><strong>Connected:</strong> <span id='conn' class='");
  page += btConnected ? F("ok'>yes") : F("warn'>no");
  page += F("</span></p><p><strong>Streaming:</strong> <span id='streaming'>");
  page += toneEnabled ? F("on") : F("off");
  page += F("</span></p></div>");

  page += F("<div class='card'><h3>1) Connect headset</h3>");
  page += F("<input type='text' id='devname' placeholder='Exact Bluetooth name' style='width:100%;box-sizing:border-box;margin-bottom:8px' value='");
  page += htmlEscape(targetDeviceName);
  page += F("'>");
  page += F("<div><button id='connbtn' onclick=\"");
  page += F("var n=document.getElementById('devname').value.trim();");
  page += F("if(!n)return;");
  page += F("document.getElementById('connbtn').textContent='Connecting...';");
  page += F("document.getElementById('connbtn').disabled=true;");
  page += F("fetch('/connect?name='+encodeURIComponent(n)).then(r=>r.json()).then(function(d){");
  page += F("document.getElementById('connbtn').textContent='Connect';");
  page += F("document.getElementById('connbtn').disabled=false;");
  page += F("document.getElementById('connmsg').textContent=d.msg;");
  page += F("}).catch(function(){");
  page += F("document.getElementById('connbtn').textContent='Connect';");
  page += F("document.getElementById('connbtn').disabled=false;");
  page += F("});\">Connect</button>");
  if (targetDeviceName.length()) {
    page += F("<a href='/forget'><button type='button' class='danger'>Forget device</button></a>");
  }
  page += F("<a href='/fullreset'><button type='button' class='danger'>Full BT reset</button></a>");
  page += F("</div>");
  page += F("<p id='connmsg' class='muted'></p>");
  page += F("<p class='muted'>Put headset in pairing mode. Audio starts automatically when connected.</p></div>");

  page += F("<div class='card'><h3>2) Manual audio control</h3>");
  page += F("<button onclick=\"fetch('/tone/on')\">Start audio</button>");
  page += F("<button onclick=\"fetch('/tone/off')\">Stop audio</button>");
  page += F("<hr style='margin:12px 0'>");
  page += F("<p class='muted' style='margin:0 0 8px'><b>Test audio</b> (embedded PCM chord, no ADC) — use to verify BT quality. WiFi stays up.<br><b>ADC input</b> — switches to live analog input. WiFi stops to reduce interference.</p>");
  page += F("</div>");

  page += F("<div class='card'><h3>3) Input Gain</h3>");
  page += F("<input type='range' id='vol' min='0' max='100' value='");
  page += String(audioVolume);
  page += F("'> <span id='volval'>");
  page += String(audioVolume);
  page += F("%</span>");
  page += F("<script>");
  page += F("var s=document.getElementById('vol');");
  page += F("s.oninput=function(){document.getElementById('volval').textContent=this.value+'%';};");
  page += F("s.onchange=function(){fetch('/volume?level='+this.value);};");
  page += F("</script></div>");

  page += F("<script>");
  page += F("function refresh(){fetch('/status').then(r=>r.json()).then(function(d){");
  page += F("document.getElementById('bt').textContent=d.bt;");
  page += F("document.getElementById('conn').textContent=d.connected?'yes':'no';");
  page += F("document.getElementById('conn').className=d.connected?'ok':'warn';");
  page += F("document.getElementById('streaming').textContent=d.streaming?'on':'off';");
  page += F("}).catch(()=>{});}");
  page += F("setInterval(refresh,3000);");
  page += F("</script>");
  page += F("</body></html>");
  return page;
}

// ---- HTTP handlers ----
static void handleRoot()     { server.send(200, "text/html", renderPage()); }

static void handleRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/");
  server.sendHeader("Connection", "close");
  server.send(302, "text/plain", "");
}

static void handleStatus() {
  String json = "{\"bt\":\"";
  json += lastBtState;
  json += "\",\"connected\":";
  json += btConnected ? "true" : "false";
  json += ",\"streaming\":";
  json += toneEnabled ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

static void handleConnect() {
  if (server.hasArg("name")) {
    targetDeviceName = server.arg("name");
    targetDeviceName.trim();
  }
  if (targetDeviceName.isEmpty()) {
    server.send(400, "application/json", "{\"msg\":\"Missing device name\"}");
    return;
  }
  prefs.begin("audio", false);
  prefs.putString("device", targetDeviceName);
  prefs.end();

  String msg;
  if (!sourceStarted) {
    startA2DP();
    msg = "Scanning for " + targetDeviceName + "...";
  } else {
    msg = "Already connecting — check status above.";
  }
  server.send(200, "application/json", "{\"msg\":\"" + msg + "\"}");
}

static void handleForget() {
  prefs.begin("audio", false);
  prefs.remove("device");
  prefs.end();
  server.send(200, "text/html",
    "<html><body><p>Device forgotten. Rebooting...</p>"
    "<script>setTimeout(function(){location='/'}, 4000);</script></body></html>");
  delay(500);
  ESP.restart();
}

static void handleFullReset() {
  server.send(200, "text/html",
    "<html><body><p>Full NVS reset (clears BT bonds + all settings). Rebooting...</p>"
    "<script>setTimeout(function(){location='/'}, 6000);</script></body></html>");
  delay(500);
  nvs_flash_erase();
  ESP.restart();
}

static void handleToneOn()  { toneEnabled = true;  server.send(200, "text/plain", "ok"); }
static void handleToneOff() { toneEnabled = false; server.send(200, "text/plain", "ok"); }

static void handleVolume() {
  if (server.hasArg("level")) {
    int v = server.arg("level").toInt();
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    audioVolume = v;
    prefs.begin("audio", false);
    prefs.putInt("volume", v);
    prefs.end();
  }
  server.send(200, "text/plain", "ok");
}

// ---- Setup ----
void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("[WiFi] AP up: %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/",             HTTP_GET, handleRoot);
  server.on("/connect",      HTTP_GET, handleConnect);
  server.on("/forget",       HTTP_GET, handleForget);
  server.on("/fullreset",    HTTP_GET, handleFullReset);
  server.on("/tone/on",      HTTP_GET, handleToneOn);
  server.on("/tone/off",     HTTP_GET, handleToneOff);
  server.on("/volume",       HTTP_GET, handleVolume);
  server.on("/status",       HTTP_GET, handleStatus);

  server.on("/hotspot-detect.html",       HTTP_GET, handleRedirect);
  server.on("/library/test/success.html", HTTP_GET, handleRedirect);
  server.on("/generate_204",              HTTP_GET, handleRedirect);
  server.on("/gen_204",                   HTTP_GET, handleRedirect);
  server.on("/ncsi.txt",                  HTTP_GET, handleRedirect);
  server.on("/connecttest.txt",           HTTP_GET, handleRedirect);
  server.onNotFound(handleRedirect);

  server.begin();
  Serial.println("http://192.168.4.1  (ESP32-Audio-Setup / esp32audio)");
}
