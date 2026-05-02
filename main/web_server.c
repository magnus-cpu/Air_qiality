#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "heater.h"
#include "mqtt_manager.h"
#include "sd_card.h"
#include "sensor.h"
#include "telemetry_pipeline.h"
#include "time_manager.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";

#define HEATER_WARMUP_SECONDS 60

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char) strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool parse_form_value(const char *body, const char *key, char *out, size_t out_len)
{
    const char *found = body;
    size_t key_len = strlen(key);
    while ((found = strstr(found, key)) != NULL) {
        if ((found == body || found[-1] == '&') && found[key_len] == '=') {
            break;
        }
        found += key_len;
    }

    if (!found) {
        return false;
    }

    const char *val = found + key_len + 1;
    const char *amp = strchr(val, '&');
    size_t len = amp ? (size_t)(amp - val) : strlen(val);

    char tmp[128];
    if (len >= sizeof(tmp)) {
        return false;
    }

    memcpy(tmp, val, len);
    tmp[len] = '\0';

    url_decode(out, tmp, out_len);
    return true;
}

static const char *auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2 Enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Secured";
    }
}

static size_t json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src && src[si] != '\0' && di + 1 < dst_len; si++) {
        char c = src[si];
        if ((c == '"' || c == '\\') && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((unsigned char)c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Air Quality</title>"
        "<style>"
        ":root{--bg:#0b0f12;--card:#141b22;--ink:#e8f1ff;--muted:#9fb0c7;--accent:#48d18f;--accent2:#2cc3ff;--warn:#f4b860;--ok:#48d18f;}"
        "*{box-sizing:border-box;}body{margin:0;font-family:'Avenir Next','Trebuchet MS',sans-serif;background:radial-gradient(120% 120% at 10% 0%,#1b2430 0%,#0b0f12 60%);color:var(--ink);}"
        ".wrap{max-width:520px;margin:0 auto;padding:20px 18px 92px;}"
        ".hero{padding:18px;border-radius:8px;background:linear-gradient(135deg,#1b2a33 0%,#132028 100%);box-shadow:0 10px 30px rgba(0,0,0,0.35);}"
        ".title{font-size:22px;margin:0 0 6px;} .sub{color:var(--muted);margin:0 0 12px;font-size:13px;}"
        ".card{margin-top:14px;padding:14px;border-radius:8px;background:var(--card);}"
        "label{display:block;font-size:12px;color:var(--muted);margin:10px 0 6px;}"
        "input{width:100%;padding:12px;border-radius:8px;border:1px solid #2a3746;background:#0e151c;color:var(--ink);font-size:15px;}"
        ".btn{width:100%;margin-top:12px;padding:12px;border:0;border-radius:8px;background:linear-gradient(90deg,var(--accent),var(--accent2));color:#041015;font-weight:700;}"
        ".ghost{background:#111821;color:var(--ink);border:1px solid #1f2a36;}"
        ".split{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;}"
        ".net{width:100%;display:grid;grid-template-columns:1fr auto;gap:5px 12px;text-align:left;margin-top:8px;padding:11px;border-radius:8px;border:1px solid #243244;background:#0e151c;color:var(--ink);}"
        ".net.active{border-color:var(--accent);box-shadow:0 0 0 1px rgba(72,209,143,.25);}"
        ".ssid{font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}.meta,.saved{font-size:11px;color:var(--muted);}.saved{color:var(--accent);}"
        ".row{display:flex;align-items:center;gap:8px;margin-top:12px;color:var(--muted);font-size:13px;}.row input{width:auto;}"
        ".empty{color:var(--muted);font-size:13px;margin:10px 0 0;}"
        ".hint{color:var(--muted);font-size:12px;line-height:1.4;margin:8px 0 0;}"
        ".mini{margin-top:10px;padding:10px;border-radius:8px;background:#0e151c;border:1px solid #223042;}"
        ".mini .label{display:block;font-size:11px;color:var(--muted);margin:0 0 4px;}"
        ".mini .value{font-size:13px;font-weight:700;}"
        ".statusline{margin-top:10px;font-size:12px;color:var(--muted);min-height:16px;}"
        ".dock{position:fixed;left:0;right:0;bottom:0;background:rgba(9,12,16,0.92);backdrop-filter:blur(8px);padding:10px 12px;border-top:1px solid #1f2a36;}"
        ".dock .row{max-width:520px;margin:0 auto;display:grid;grid-template-columns:repeat(3,1fr);gap:10px;}"
        ".dock a{display:block;text-align:center;text-decoration:none;color:var(--ink);padding:10px 0;border-radius:8px;background:#111821;border:1px solid #1f2a36;font-size:13px;}"
        ".dock a.active{border-color:var(--accent);color:var(--accent);}"
        "@media (max-width:420px){.split{grid-template-columns:1fr;}}"
        "</style></head><body>"
        "<div class='wrap'>"
        "<div class='hero'>"
        "<h1 class='title'>Air Quality Hub</h1>"
        "<p class='sub'>Connect the device to your Wi‑Fi and monitor sensors in real time.</p>"
        "</div>"
        "<div class='card'>"
        "<form method='POST' action='/save'>"
        "<label>Available Wi‑Fi</label>"
        "<div id='networks'><p class='empty'>Scanning...</p></div>"
        "<button class='btn ghost' type='button' onclick='scanWifi()'>Scan again</button>"
        "<label>Saved networks</label>"
        "<div id='history'><p class='empty'>Loading...</p></div>"
        "<label>Wi‑Fi SSID</label><input id='ssid' name='ssid' placeholder='Your router name' required>"
        "<label>Password</label><input name='pass' type='password' placeholder='Optional'>"
        "<label class='row'><input id='hidden' name='hidden' type='checkbox' value='1'>Hidden network</label>"
        "<button class='btn' type='submit'>Save & Connect</button>"
        "</form>"
        "</div>"
        "<div class='card'>"
        "<form method='POST' action='/mqtt'>"
        "<label>MQTT broker URI</label><input id='mqtt_uri' name='broker_uri' placeholder='mqtt://192.168.1.10:1883'>"
        "<label>Base topic</label><input id='mqtt_topic' name='base_topic' placeholder='air_quality'>"
        "<label class='row'><input id='mqtt_enabled' name='enabled' type='checkbox' value='1'>Enable MQTT</label>"
        "<p class='hint'>Telemetry: base/device/telemetry. Commands: base/device/cmd.</p>"
        "<button class='btn' type='submit'>Save MQTT</button>"
        "</form>"
        "</div>"
        "<div class='card'>"
        "<form onsubmit='saveCalibration(event)'>"
        "<label>Sensor calibration mode</label>"
        "<p class='hint'>Place the sensor in a stable known-gas environment, wait until readings settle, then enter the reference ppm values and save.</p>"
        "<div class='split'>"
        "<div class='mini'><span class='label'>NH3 resistance</span><span class='value' id='live_nh3_res'>-</span></div>"
        "<div class='mini'><span class='label'>RED resistance</span><span class='value' id='live_red_res'>-</span></div>"
        "<div class='mini'><span class='label'>OX resistance</span><span class='value' id='live_ox_res'>-</span></div>"
        "</div>"
        "<label>NH3 reference ppm</label><input id='cal_nh3_ppm' name='nh3_ppm' type='number' min='0' step='0.001' placeholder='e.g. 25' required>"
        "<label>RED reference ppm</label><input id='cal_red_ppm' name='red_ppm' type='number' min='0' step='0.001' placeholder='e.g. 10' required>"
        "<label>OX reference ppm</label><input id='cal_ox_ppm' name='ox_ppm' type='number' min='0' step='0.001' placeholder='e.g. 5' required>"
        "<button class='btn' type='submit'>Save calibration</button>"
        "<div id='cal_status' class='statusline'></div>"
        "<p class='hint' id='cal_saved'>No calibration saved yet.</p>"
        "</form>"
        "</div>"
        "<div class='card'>"
        "<label>SD card maintenance</label>"
        "<p class='hint'>Format the inserted SD card to FAT and erase all stored telemetry files. Use this only when you want a clean card.</p>"
        "<button class='btn ghost' type='button' onclick='formatSdCard()'>Format SD card</button>"
        "<div id='sd_format_status' class='statusline'></div>"
        "</div>"
        "</div>"
        "<nav class='dock'><div class='row'>"
        "<a class='active' href='/' data-label='Setup'>Setup</a>"
        "<a href='/status' data-label='Status'>Status</a>"
        "<a href='/chat' data-label='Chat'>Chat</a>"
        "</div></nav>"
        "<script>"
        "function esc(s){return String(s||'').replace(/[&<>\"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[m]));}"
        "function bind(box){box.querySelectorAll('.net').forEach(b=>b.onclick=()=>pick(b.dataset.ssid,b.dataset.hidden==='1'));}"
        "function pick(ssid,hidden){document.getElementById('ssid').value=ssid;document.getElementById('hidden').checked=!!hidden;document.querySelectorAll('.net').forEach(n=>n.classList.toggle('active',n.dataset.ssid===ssid));}"
        "async function scanWifi(){const box=document.getElementById('networks');box.innerHTML='<p class=\"empty\">Scanning...</p>';try{const r=await fetch('/api/wifi/scan');const j=await r.json();box.innerHTML=j.networks.length?j.networks.map(n=>'<button class=\"net\" type=\"button\" data-hidden=\"0\" data-ssid=\"'+esc(n.ssid)+'\"><span class=\"ssid\">'+esc(n.ssid)+'</span><span class=\"meta\">'+n.rssi+' dBm</span><span class=\"meta\">'+esc(n.auth)+'</span><span class=\"saved\">'+(n.saved?'saved':'')+'</span></button>').join(''):'<p class=\"empty\">No visible networks found.</p>';bind(box);}catch(e){box.innerHTML='<p class=\"empty\">Scan failed.</p>';}}"
        "async function loadHistory(){const box=document.getElementById('history');try{const r=await fetch('/api/wifi/history');const j=await r.json();box.innerHTML=j.networks.length?j.networks.map(n=>'<button class=\"net\" type=\"button\" data-hidden=\"'+(n.hidden?'1':'0')+'\" data-ssid=\"'+esc(n.ssid)+'\"><span class=\"ssid\">'+esc(n.ssid)+'</span><span class=\"meta\">'+(n.hidden?'hidden':'known')+'</span></button>').join(''):'<p class=\"empty\">No saved networks yet.</p>';bind(box);}catch(e){box.innerHTML='<p class=\"empty\">History unavailable.</p>';}}"
        "async function loadMqtt(){try{const r=await fetch('/api/mqtt/config');const j=await r.json();document.getElementById('mqtt_uri').value=j.broker_uri||'';document.getElementById('mqtt_topic').value=j.base_topic||'air_quality';document.getElementById('mqtt_enabled').checked=!!j.enabled;}catch(e){}}"
        "function fmt(v){return typeof v==='number'&&isFinite(v)?v.toFixed(2)+' ohms':'-';}"
        "async function loadCalibration(){const saved=document.getElementById('cal_saved');try{const r=await fetch('/api/sensor/calibration');const j=await r.json();document.getElementById('cal_nh3_ppm').value=j.nh3.valid?j.nh3.reference_ppm:'';document.getElementById('cal_red_ppm').value=j.red.valid?j.red.reference_ppm:'';document.getElementById('cal_ox_ppm').value=j.ox.valid?j.ox.reference_ppm:'';saved.textContent=(j.nh3.valid&&j.red.valid&&j.ox.valid)?('Saved refs: NH3 '+j.nh3.reference_ppm+' ppm at '+j.nh3.reference_resistance_ohms.toFixed(2)+' ohms, RED '+j.red.reference_ppm+' ppm at '+j.red.reference_resistance_ohms.toFixed(2)+' ohms, OX '+j.ox.reference_ppm+' ppm at '+j.ox.reference_resistance_ohms.toFixed(2)+' ohms'):'No full calibration saved yet.';}catch(e){saved.textContent='Calibration state unavailable.';}}"
        "async function loadLiveCalibrationReadings(){try{const r=await fetch('/api/status');const j=await r.json();document.getElementById('live_nh3_res').textContent=fmt(j.nh3_res_ohms);document.getElementById('live_red_res').textContent=fmt(j.red_res_ohms);document.getElementById('live_ox_res').textContent=fmt(j.ox_res_ohms);}catch(e){}}"
        "async function saveCalibration(ev){ev.preventDefault();const status=document.getElementById('cal_status');status.textContent='Saving calibration...';const body='nh3_ppm='+encodeURIComponent(document.getElementById('cal_nh3_ppm').value)+'&red_ppm='+encodeURIComponent(document.getElementById('cal_red_ppm').value)+'&ox_ppm='+encodeURIComponent(document.getElementById('cal_ox_ppm').value);try{const r=await fetch('/api/sensor/calibration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok){throw new Error('save failed');}status.textContent='Calibration saved from current stabilized resistances.';loadCalibration();loadLiveCalibrationReadings();}catch(e){status.textContent='Calibration save failed.';}}"
        "async function formatSdCard(){const status=document.getElementById('sd_format_status');if(!confirm('Format the SD card to FAT and erase all stored telemetry data?')){return;}status.textContent='Formatting SD card...';try{const r=await fetch('/api/sd/format',{method:'POST'});const text=await r.text();if(!r.ok){throw new Error(text||'format failed');}status.textContent='SD card formatted successfully.';}catch(e){status.textContent='SD format failed.';}}"
        "scanWifi();loadHistory();loadMqtt();loadCalibration();loadLiveCalibrationReadings();setInterval(loadLiveCalibrationReadings,3000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }

    char body[513];
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive body");
        }
        received += r;
    }
    body[total_len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char hidden_value[8] = {0};
    if (!parse_form_value(body, "ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
    }
    parse_form_value(body, "pass", pass, sizeof(pass));
    bool hidden = parse_form_value(body, "hidden", hidden_value, sizeof(hidden_value));

    esp_err_t err = wifi_manager_save_and_connect_ex(ssid, pass, hidden);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Wi-Fi creds: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
    }

    const char *resp =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Saved</title>"
        "<style>body{font-family:'Avenir Next','Trebuchet MS',sans-serif;margin:24px;background:#0b0f12;color:#e8f1ff;}</style>"
        "</head><body>"
        "<h3>Saved</h3>"
        "<p>Trying to connect. Open <a href='/status'>Status</a> for live data.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }

    char body[513];
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive body");
        }
        received += r;
    }
    body[total_len] = '\0';

    mqtt_manager_config_t config = {0};
    char enabled_value[8] = {0};
    parse_form_value(body, "broker_uri", config.broker_uri, sizeof(config.broker_uri));
    parse_form_value(body, "base_topic", config.base_topic, sizeof(config.base_topic));
    config.enabled = parse_form_value(body, "enabled", enabled_value, sizeof(enabled_value));
    if (config.base_topic[0] == '\0') {
        strlcpy(config.base_topic, "air_quality", sizeof(config.base_topic));
    }

    esp_err_t err = mqtt_manager_save_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT config: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save MQTT config");
    }

    const char *resp =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>MQTT Saved</title>"
        "<style>body{font-family:'Avenir Next','Trebuchet MS',sans-serif;margin:24px;background:#0b0f12;color:#e8f1ff;}</style>"
        "</head><body>"
        "<h3>MQTT saved</h3>"
        "<p>Restart the device to apply a changed broker connection. Open <a href='/status'>Status</a> to see MQTT state.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Status</title>"
        "<style>"
        ":root{--bg:#0b0f12;--card:#141b22;--ink:#e8f1ff;--muted:#9fb0c7;--accent:#48d18f;--accent2:#2cc3ff;--warn:#f4b860;--ok:#48d18f;}"
        "*{box-sizing:border-box;}body{margin:0;font-family:'Avenir Next','Trebuchet MS',sans-serif;background:radial-gradient(120% 120% at 10% 0%,#1b2430 0%,#0b0f12 60%);color:var(--ink);}"
        ".wrap{max-width:520px;margin:0 auto;padding:20px 18px 92px;}"
        ".card{padding:14px;border-radius:16px;background:var(--card);margin-top:14px;}"
        ".grid{display:grid;grid-template-columns:1fr auto;gap:10px;}"
        ".label{color:var(--muted);font-size:12px;}"
        ".value{font-weight:600;}"
        ".pill{display:inline-block;padding:4px 8px;border-radius:999px;background:#0f1720;border:1px solid #223042;font-size:11px;color:var(--muted);}"
        ".stack{display:flex;flex-direction:column;align-items:flex-end;gap:4px;}"
        ".badge{display:inline-block;padding:4px 10px;border-radius:999px;font-size:11px;border:1px solid #223042;background:#0f1720;color:var(--muted);}"
        ".badge.on{border-color:var(--ok);color:#062315;background:rgba(72,209,143,0.15);}"
        ".badge.off{border-color:#44566a;color:var(--muted);}"
        ".btns{display:flex;gap:10px;margin-top:12px;}"
        ".btn{flex:1;padding:12px;border:0;border-radius:12px;background:#111821;color:var(--ink);border:1px solid #1f2a36;}"
        ".btn.on{background:linear-gradient(90deg,var(--accent),var(--accent2));color:#041015;font-weight:700;}"
        ".dock{position:fixed;left:0;right:0;bottom:0;background:rgba(9,12,16,0.92);backdrop-filter:blur(8px);padding:10px 12px;border-top:1px solid #1f2a36;}"
        ".dock .row{max-width:520px;margin:0 auto;display:grid;grid-template-columns:repeat(3,1fr);gap:10px;}"
        ".dock a{display:block;text-align:center;text-decoration:none;color:var(--ink);padding:10px 0;border-radius:12px;background:#111821;border:1px solid #1f2a36;font-size:13px;}"
        ".dock a.active{border-color:var(--accent);color:var(--accent);}"
        "</style></head><body>"
        "<div class='wrap'>"
        "<h2>Status</h2>"
        "<div class='card'>"
        "<div class='grid'><div class='label'>NH3</div><div class='value stack'><span><span id='nh3'>-</span> <span class='pill'><span id='nh3mv'>-</span> mV</span></span><span class='pill'><span id='nh3ppm'>-</span></span></div></div>"
        "<div class='grid'><div class='label'>RED</div><div class='value stack'><span><span id='red'>-</span> <span class='pill'><span id='redmv'>-</span> mV</span></span><span class='pill'><span id='redppm'>-</span></span></div></div>"
        "<div class='grid'><div class='label'>OX</div><div class='value stack'><span><span id='ox'>-</span> <span class='pill'><span id='oxmv'>-</span> mV</span></span><span class='pill'><span id='oxppm'>-</span></span></div></div>"
        "<div class='grid'><div class='label'>Heater</div><div class='value'><span id='heater' class='badge'>-</span></div></div>"
        "<div class='grid'><div class='label'>Warm-up</div><div class='value' id='warmup'>-</div></div>"
        "<div class='grid'><div class='label'>Wi‑Fi</div><div class='value' id='wifi'>-</div></div>"
        "<div class='grid'><div class='label'>SD card</div><div class='value' id='sd'>-</div></div>"
        "<div class='grid'><div class='label'>MQTT</div><div class='value' id='mqtt'>-</div></div>"
        "</div>"
        "<div class='btns'>"
        "<button class='btn on' onclick='setHeater(1)'>Heater ON</button>"
        "<button class='btn' onclick='setHeater(0)'>Heater OFF</button>"
        "</div>"
        "</div>"
        "<nav class='dock'><div class='row'>"
        "<a href='/' data-label='Setup'>Setup</a>"
        "<a class='active' href='/status' data-label='Status'>Status</a>"
        "<a href='/chat' data-label='Chat'>Chat</a>"
        "</div></nav>"
        "<script>"
        "function ppm(v,ok){return ok&&typeof v==='number'&&isFinite(v)?v.toFixed(2)+' ppm':'ppm not calibrated';}"
        "async function load(){"
        "const r=await fetch('/api/status');"
        "const j=await r.json();"
        "document.getElementById('nh3').textContent=j.nh3_raw;"
        "document.getElementById('red').textContent=j.red_raw;"
        "document.getElementById('ox').textContent=j.ox_raw;"
        "document.getElementById('nh3mv').textContent=(j.nh3_mv<0)?'n/a':j.nh3_mv;"
        "document.getElementById('redmv').textContent=(j.red_mv<0)?'n/a':j.red_mv;"
        "document.getElementById('oxmv').textContent=(j.ox_mv<0)?'n/a':j.ox_mv;"
        "document.getElementById('nh3ppm').textContent=ppm(j.nh3_ppm,j.nh3_ppm_valid);"
        "document.getElementById('redppm').textContent=ppm(j.red_ppm,j.red_ppm_valid);"
        "document.getElementById('oxppm').textContent=ppm(j.ox_ppm,j.ox_ppm_valid);"
        "const h=document.getElementById('heater');"
        "h.textContent=j.heater_on ? 'ON' : 'OFF';"
        "h.className='badge '+(j.heater_on?'on':'off');"
        "if(j.heater_on){"
        "document.getElementById('warmup').textContent=j.warmup ? ('warming ('+j.since_change+'s)') : 'ready';"
        "}else{"
        "document.getElementById('warmup').textContent='off';"
        "}"
        "document.getElementById('wifi').textContent=j.wifi;"
        "document.getElementById('sd').textContent=j.sd_card;"
        "document.getElementById('mqtt').textContent=j.mqtt;"
        "}"
        "async function setHeater(v){await fetch('/api/heater?value='+v);load();}"
        "load();setInterval(load,2000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t chat_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Chat</title>"
        "<style>"
        ":root{--bg:#0b0f12;--card:#141b22;--ink:#e8f1ff;--muted:#9fb0c7;--accent:#48d18f;}"
        "*{box-sizing:border-box;}body{margin:0;font-family:'Avenir Next','Trebuchet MS',sans-serif;background:radial-gradient(120% 120% at 10% 0%,#1b2430 0%,#0b0f12 60%);color:var(--ink);}"
        ".wrap{max-width:520px;margin:0 auto;padding:20px 18px 92px;}"
        ".card{padding:14px;border-radius:16px;background:var(--card);margin-top:14px;}"
        "label{display:block;font-size:12px;color:var(--muted);margin:10px 0 6px;}"
        "input,textarea{width:100%;padding:12px;border-radius:12px;border:1px solid #2a3746;background:#0e151c;color:var(--ink);font-size:15px;}"
        ".btn{width:100%;margin-top:12px;padding:12px;border:0;border-radius:12px;background:#111821;color:var(--ink);border:1px solid #1f2a36;}"
        ".dock{position:fixed;left:0;right:0;bottom:0;background:rgba(9,12,16,0.92);backdrop-filter:blur(8px);padding:10px 12px;border-top:1px solid #1f2a36;}"
        ".dock .row{max-width:520px;margin:0 auto;display:grid;grid-template-columns:repeat(3,1fr);gap:10px;}"
        ".dock a{display:block;text-align:center;text-decoration:none;color:var(--ink);padding:10px 0;border-radius:12px;background:#111821;border:1px solid #1f2a36;font-size:13px;}"
        ".dock a.active{border-color:var(--accent);color:var(--accent);}"
        "</style></head><body>"
        "<div class='wrap'>"
        "<h2>Chat</h2>"
        "<div class='card'>"
        "<form>"
        "<label>Name</label><input placeholder='Your name'>"
        "<label>Message</label><textarea rows='4' placeholder='Type a message'></textarea>"
        "<button class='btn' type='button'>Send</button>"
        "</form>"
        "</div>"
        "</div>"
        "<nav class='dock'><div class='row'>"
        "<a href='/' data-label='Setup'>Setup</a>"
        "<a href='/status' data-label='Status'>Status</a>"
        "<a class='active' href='/chat' data-label='Chat'>Chat</a>"
        "</div></nav>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    telemetry_sample_t sample = {0};
    bool have_sample = telemetry_pipeline_get_latest(&sample);
    sensor_snapshot_t sensor_snapshot = {0};
    if (!have_sample) {
        sensor_read_all(&sensor_snapshot);
    }
    int nh3_raw = have_sample ? sample.nh3_raw : sensor_snapshot.nh3.raw;
    int red_raw = have_sample ? sample.red_raw : sensor_snapshot.red.raw;
    int ox_raw = have_sample ? sample.ox_raw : sensor_snapshot.ox.raw;
    int nh3_mv = have_sample ? sample.nh3_mv : sensor_snapshot.nh3.mv;
    int red_mv = have_sample ? sample.red_mv : sensor_snapshot.red.mv;
    int ox_mv = have_sample ? sample.ox_mv : sensor_snapshot.ox.mv;
    int heater_on = have_sample ? sample.heater_on : heater_get();
    int since_change = have_sample ? sample.since_change : heater_seconds_since_change();
    int warmup = have_sample ? sample.warmup : heater_is_warming(HEATER_WARMUP_SECONDS);

    char ip_str[16] = "0.0.0.0";
    wifi_manager_get_ip(ip_str, sizeof(ip_str));
    char sd_status[96] = {0};
    sd_card_get_status(sd_status, sizeof(sd_status));
    char mqtt_status[128] = {0};
    mqtt_manager_get_status(mqtt_status, sizeof(mqtt_status));
    char mqtt_json[180] = {0};
    json_escape(mqtt_json, sizeof(mqtt_json), mqtt_status);
    char time_status[128] = {0};
    time_manager_get_status(time_status, sizeof(time_status));
    char time_json[160] = {0};
    json_escape(time_json, sizeof(time_json), time_status);

    const char *wifi_state = (have_sample ? sample.wifi_connected : wifi_manager_is_connected()) ? "connected" : "disconnected";

    char resp[1280];
    snprintf(resp, sizeof(resp),
             "{\"nh3_raw\":%d,\"red_raw\":%d,\"ox_raw\":%d,"
             "\"nh3_mv\":%d,\"red_mv\":%d,\"ox_mv\":%d,"
             "\"nh3_res_ohms\":%.3f,\"red_res_ohms\":%.3f,\"ox_res_ohms\":%.3f,"
             "\"nh3_ppm\":%.3f,\"red_ppm\":%.3f,\"ox_ppm\":%.3f,"
             "\"nh3_ppm_valid\":%s,\"red_ppm_valid\":%s,\"ox_ppm_valid\":%s,"
             "\"heater_on\":%d,\"warmup\":%d,\"since_change\":%d,"
             "\"wifi\":\"%s (%s)\",\"sd_card\":\"%s\",\"mqtt\":\"%s\","
             "\"time\":\"%s\",\"time_synced\":%s,\"timestamp_ms\":%" PRIu64 "}",
             nh3_raw, red_raw, ox_raw,
             nh3_mv, red_mv, ox_mv,
             have_sample ? sample.nh3_res_ohms : sensor_snapshot.nh3.resistance_ohms,
             have_sample ? sample.red_res_ohms : sensor_snapshot.red.resistance_ohms,
             have_sample ? sample.ox_res_ohms : sensor_snapshot.ox.resistance_ohms,
             have_sample ? sample.nh3_ppm : sensor_snapshot.nh3.ppm,
             have_sample ? sample.red_ppm : sensor_snapshot.red.ppm,
             have_sample ? sample.ox_ppm : sensor_snapshot.ox.ppm,
             (have_sample ? sample.nh3_ppm_valid : sensor_snapshot.nh3.ppm_valid) ? "true" : "false",
             (have_sample ? sample.red_ppm_valid : sensor_snapshot.red.ppm_valid) ? "true" : "false",
             (have_sample ? sample.ox_ppm_valid : sensor_snapshot.ox.ppm_valid) ? "true" : "false",
             heater_on, warmup, since_change,
             wifi_state, ip_str, sd_status, mqtt_json,
             time_json,
             (have_sample ? sample.time_synced : time_manager_is_synchronized()) ? "true" : "false",
             have_sample ? sample.timestamp_ms : time_manager_get_epoch_ms());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_heater_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[8] = {0};
        if (httpd_query_key_value(query, "value", value, sizeof(value)) == ESP_OK) {
            int v = atoi(value);
            heater_set(v ? true : false);
        }
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_manager_ap_t aps[WIFI_MANAGER_MAX_SCAN_RESULTS] = {0};
    uint16_t count = WIFI_MANAGER_MAX_SCAN_RESULTS;
    esp_err_t err = wifi_manager_scan(aps, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    }

    char *resp = calloc(1, 4096);
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    size_t pos = 0;
    size_t resp_len = 4096;
    pos += snprintf(resp + pos, resp_len - pos, "{\"networks\":[");
    for (uint16_t i = 0; i < count && pos < resp_len; i++) {
        char ssid[80];
        char auth[32];
        json_escape(ssid, sizeof(ssid), aps[i].ssid);
        json_escape(auth, sizeof(auth), auth_mode_name(aps[i].authmode));
        pos += snprintf(resp + pos, resp_len - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\",\"saved\":%s}",
                        i ? "," : "", ssid, aps[i].rssi, auth, aps[i].saved ? "true" : "false");
    }
    snprintf(resp + pos, resp_len - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return send_err;
}

static esp_err_t api_wifi_history_get_handler(httpd_req_t *req)
{
    wifi_manager_saved_network_t networks[WIFI_MANAGER_MAX_NETWORKS] = {0};
    size_t count = wifi_manager_get_saved_networks(networks, WIFI_MANAGER_MAX_NETWORKS);

    char resp[1024];
    size_t pos = 0;
    pos += snprintf(resp + pos, sizeof(resp) - pos, "{\"networks\":[");
    for (size_t i = 0; i < count && pos < sizeof(resp); i++) {
        char ssid[80];
        json_escape(ssid, sizeof(ssid), networks[i].ssid);
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "%s{\"ssid\":\"%s\",\"hidden\":%s}",
                        i ? "," : "", ssid, networks[i].hidden ? "true" : "false");
    }
    snprintf(resp + pos, sizeof(resp) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_mqtt_config_get_handler(httpd_req_t *req)
{
    mqtt_manager_config_t config = {0};
    mqtt_manager_load_config(&config);
    if (config.base_topic[0] == '\0') {
        strlcpy(config.base_topic, "air_quality", sizeof(config.base_topic));
    }

    char uri[180] = {0};
    char topic[96] = {0};
    json_escape(uri, sizeof(uri), config.broker_uri);
    json_escape(topic, sizeof(topic), config.base_topic);

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"broker_uri\":\"%s\",\"base_topic\":\"%s\",\"enabled\":%s}",
             uri, topic, config.enabled ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_sensor_calibration_get_handler(httpd_req_t *req)
{
    sensor_calibration_t calibration = {0};
    sensor_get_calibration(&calibration);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"nh3\":{\"valid\":%s,\"reference_resistance_ohms\":%.3f,\"reference_ppm\":%.3f},"
             "\"red\":{\"valid\":%s,\"reference_resistance_ohms\":%.3f,\"reference_ppm\":%.3f},"
             "\"ox\":{\"valid\":%s,\"reference_resistance_ohms\":%.3f,\"reference_ppm\":%.3f}}",
             calibration.nh3.valid ? "true" : "false",
             calibration.nh3.reference_resistance_ohms, calibration.nh3.reference_ppm,
             calibration.red.valid ? "true" : "false",
             calibration.red.reference_resistance_ohms, calibration.red.reference_ppm,
             calibration.ox.valid ? "true" : "false",
             calibration.ox.reference_resistance_ohms, calibration.ox.reference_ppm);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_sensor_calibration_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }

    char body[257];
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive body");
        }
        received += r;
    }
    body[total_len] = '\0';

    char nh3_ppm_str[24] = {0};
    char red_ppm_str[24] = {0};
    char ox_ppm_str[24] = {0};
    if (!parse_form_value(body, "nh3_ppm", nh3_ppm_str, sizeof(nh3_ppm_str)) ||
        !parse_form_value(body, "red_ppm", red_ppm_str, sizeof(red_ppm_str)) ||
        !parse_form_value(body, "ox_ppm", ox_ppm_str, sizeof(ox_ppm_str))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ppm values");
    }

    float nh3_ppm = strtof(nh3_ppm_str, NULL);
    float red_ppm = strtof(red_ppm_str, NULL);
    float ox_ppm = strtof(ox_ppm_str, NULL);
    if (!sensor_capture_calibration(nh3_ppm, red_ppm, ox_ppm)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Calibration failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_sd_format_post_handler(httpd_req_t *req)
{
    esp_err_t err = sd_card_format();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD format failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

void web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
        };
        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
        };
        httpd_uri_t mqtt = {
            .uri = "/mqtt",
            .method = HTTP_POST,
            .handler = mqtt_post_handler,
        };
        httpd_uri_t status = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_get_handler,
        };
        httpd_uri_t api_status = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = api_status_get_handler,
        };
        httpd_uri_t chat = {
            .uri = "/chat",
            .method = HTTP_GET,
            .handler = chat_get_handler,
        };
        httpd_uri_t api_heater = {
            .uri = "/api/heater",
            .method = HTTP_GET,
            .handler = api_heater_get_handler,
        };
        httpd_uri_t api_wifi_scan = {
            .uri = "/api/wifi/scan",
            .method = HTTP_GET,
            .handler = api_wifi_scan_get_handler,
        };
        httpd_uri_t api_wifi_history = {
            .uri = "/api/wifi/history",
            .method = HTTP_GET,
            .handler = api_wifi_history_get_handler,
        };
        httpd_uri_t api_mqtt_config = {
            .uri = "/api/mqtt/config",
            .method = HTTP_GET,
            .handler = api_mqtt_config_get_handler,
        };
        httpd_uri_t api_sensor_calibration_get = {
            .uri = "/api/sensor/calibration",
            .method = HTTP_GET,
            .handler = api_sensor_calibration_get_handler,
        };
        httpd_uri_t api_sensor_calibration_post = {
            .uri = "/api/sensor/calibration",
            .method = HTTP_POST,
            .handler = api_sensor_calibration_post_handler,
        };
        httpd_uri_t api_sd_format_post = {
            .uri = "/api/sd/format",
            .method = HTTP_POST,
            .handler = api_sd_format_post_handler,
        };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
        httpd_register_uri_handler(server, &mqtt);
        httpd_register_uri_handler(server, &status);
        httpd_register_uri_handler(server, &api_status);
        httpd_register_uri_handler(server, &chat);
        httpd_register_uri_handler(server, &api_heater);
        httpd_register_uri_handler(server, &api_wifi_scan);
        httpd_register_uri_handler(server, &api_wifi_history);
        httpd_register_uri_handler(server, &api_mqtt_config);
        httpd_register_uri_handler(server, &api_sensor_calibration_get);
        httpd_register_uri_handler(server, &api_sensor_calibration_post);
        httpd_register_uri_handler(server, &api_sd_format_post);
    }
}
