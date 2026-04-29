#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "heater.h"
#include "sensor.h"
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
    const char *found = strstr(body, key);
    if (!found) {
        return false;
    }

    const char *eq = strchr(found, '=');
    if (!eq) {
        return false;
    }

    const char *val = eq + 1;
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
        ".hero{padding:18px;border-radius:18px;background:linear-gradient(135deg,#1b2a33 0%,#132028 100%);box-shadow:0 10px 30px rgba(0,0,0,0.35);}"
        ".title{font-size:22px;margin:0 0 6px;} .sub{color:var(--muted);margin:0 0 12px;font-size:13px;}"
        ".card{margin-top:14px;padding:14px;border-radius:16px;background:var(--card);}"
        "label{display:block;font-size:12px;color:var(--muted);margin:10px 0 6px;}"
        "input{width:100%;padding:12px;border-radius:12px;border:1px solid #2a3746;background:#0e151c;color:var(--ink);font-size:15px;}"
        ".btn{width:100%;margin-top:12px;padding:12px;border:0;border-radius:12px;background:linear-gradient(90deg,var(--accent),var(--accent2));color:#041015;font-weight:700;}"
        ".dock{position:fixed;left:0;right:0;bottom:0;background:rgba(9,12,16,0.92);backdrop-filter:blur(8px);padding:10px 12px;border-top:1px solid #1f2a36;}"
        ".dock .row{max-width:520px;margin:0 auto;display:grid;grid-template-columns:repeat(3,1fr);gap:10px;}"
        ".dock a{display:block;text-align:center;text-decoration:none;color:var(--ink);padding:10px 0;border-radius:12px;background:#111821;border:1px solid #1f2a36;font-size:13px;}"
        ".dock a.active{border-color:var(--accent);color:var(--accent);}"
        "</style></head><body>"
        "<div class='wrap'>"
        "<div class='hero'>"
        "<h1 class='title'>Air Quality Hub</h1>"
        "<p class='sub'>Connect the device to your Wi‑Fi and monitor sensors in real time.</p>"
        "</div>"
        "<div class='card'>"
        "<form method='POST' action='/save'>"
        "<label>Wi‑Fi SSID</label><input name='ssid' placeholder='Your router name' required>"
        "<label>Password</label><input name='pass' type='password' placeholder='Optional'>"
        "<button class='btn' type='submit'>Save & Connect</button>"
        "</form>"
        "</div>"
        "</div>"
        "<nav class='dock'><div class='row'>"
        "<a class='active' href='/' data-label='Setup'>Setup</a>"
        "<a href='/status' data-label='Status'>Status</a>"
        "<a href='/chat' data-label='Chat'>Chat</a>"
        "</div></nav>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
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

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!parse_form_value(body, "ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
    }
    parse_form_value(body, "pass", pass, sizeof(pass));

    esp_err_t err = wifi_manager_save_and_connect(ssid, pass);
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
        "<div class='grid'><div class='label'>NH3</div><div class='value'><span id='nh3'>-</span> <span class='pill'><span id='nh3mv'>-</span> mV</span></div></div>"
        "<div class='grid'><div class='label'>RED</div><div class='value'><span id='red'>-</span> <span class='pill'><span id='redmv'>-</span> mV</span></div></div>"
        "<div class='grid'><div class='label'>OX</div><div class='value'><span id='ox'>-</span> <span class='pill'><span id='oxmv'>-</span> mV</span></div></div>"
        "<div class='grid'><div class='label'>Heater</div><div class='value'><span id='heater' class='badge'>-</span></div></div>"
        "<div class='grid'><div class='label'>Warm-up</div><div class='value' id='warmup'>-</div></div>"
        "<div class='grid'><div class='label'>Wi‑Fi</div><div class='value' id='wifi'>-</div></div>"
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
        "async function load(){"
        "const r=await fetch('/api/status');"
        "const j=await r.json();"
        "document.getElementById('nh3').textContent=j.nh3_raw;"
        "document.getElementById('red').textContent=j.red_raw;"
        "document.getElementById('ox').textContent=j.ox_raw;"
        "document.getElementById('nh3mv').textContent=(j.nh3_mv<0)?'n/a':j.nh3_mv;"
        "document.getElementById('redmv').textContent=(j.red_mv<0)?'n/a':j.red_mv;"
        "document.getElementById('oxmv').textContent=(j.ox_mv<0)?'n/a':j.ox_mv;"
        "const h=document.getElementById('heater');"
        "h.textContent=j.heater_on ? 'ON' : 'OFF';"
        "h.className='badge '+(j.heater_on?'on':'off');"
        "if(j.heater_on){"
        "document.getElementById('warmup').textContent=j.warmup ? ('warming ('+j.since_change+'s)') : 'ready';"
        "}else{"
        "document.getElementById('warmup').textContent='off';"
        "}"
        "document.getElementById('wifi').textContent=j.wifi;"
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
    int nh3_raw = sensor_read_nh3();
    int red_raw = sensor_read_red();
    int ox_raw = sensor_read_ox();
    int nh3_mv = sensor_read_nh3_mv();
    int red_mv = sensor_read_red_mv();
    int ox_mv = sensor_read_ox_mv();
    int heater_on = heater_get();
    int since_change = heater_seconds_since_change();
    int warmup = heater_is_warming(HEATER_WARMUP_SECONDS);

    char ip_str[16] = "0.0.0.0";
    wifi_manager_get_ip(ip_str, sizeof(ip_str));

    const char *wifi_state = wifi_manager_is_connected() ? "connected" : "disconnected";

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"nh3_raw\":%d,\"red_raw\":%d,\"ox_raw\":%d,"
             "\"nh3_mv\":%d,\"red_mv\":%d,\"ox_mv\":%d,"
             "\"heater_on\":%d,\"warmup\":%d,\"since_change\":%d,"
             "\"wifi\":\"%s (%s)\"}",
             nh3_raw, red_raw, ox_raw,
             nh3_mv, red_mv, ox_mv,
             heater_on, warmup, since_change,
             wifi_state, ip_str);

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

void web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 9;

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

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
        httpd_register_uri_handler(server, &status);
        httpd_register_uri_handler(server, &api_status);
        httpd_register_uri_handler(server, &chat);
        httpd_register_uri_handler(server, &api_heater);
    }
}
