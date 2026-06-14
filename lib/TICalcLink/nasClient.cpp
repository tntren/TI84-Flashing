#include "nasClient.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static String sid = "";
static String synoToken = "";
static bool connected = false;
static String currentSSID = "";
static String currentPass = "";

static String urlEncode(const String& s) {
    String encoded = "";
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded += hex;
        }
    }
    return encoded;
}

static String buildUrl(const char* api, const char* method, int version, const char* extra = "") {
    String url = "https://";
    url += NAS_HOST;
    url += ":";
    url += NAS_PORT;
    url += "/webapi/entry.cgi?api=";
    url += api;
    url += "&version=";
    url += version;
    url += "&method=";
    url += method;
    if (sid.length() > 0) {
        url += "&_sid=";
        url += sid;
        url += "&SynoToken=";
        url += synoToken;
    }
    if (strlen(extra) > 0) {
        url += "&";
        url += extra;
    }
    return url;
}

static bool httpGet(const String& url, String& responseOut) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        Serial.print("HTTP error: "); Serial.println(code);
        http.end();
        return false;
    }
    responseOut = http.getString();
    http.end();
    return true;
}

static bool nasLogin() {
    String loginExtra = "account=" + urlEncode(String(NAS_USER))
                      + "&passwd=" + urlEncode(String(NAS_PASS))
                      + "&enable_syno_token=yes";
    String url = buildUrl("SYNO.API.Auth", "login", 6, loginExtra.c_str());

    String response;
    if (!httpGet(url, response)) return false;

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, response) || !doc["success"].as<bool>()) {
        Serial.print("NAS login failed: "); Serial.println(response);
        return false;
    }

    sid = doc["data"]["sid"].as<String>();
    synoToken = doc["data"]["synotoken"].as<String>();
    if (synoToken.length() == 0)
        synoToken = doc["data"]["SynoToken"].as<String>();

    connected = true;
    Serial.println("NAS logged in");
    return true;
}

// Connect with creds from config.h (fallback)
bool nasConnect() {
    return nasConnectWithCreds(WIFI_SSID, WIFI_PASS);
}

// Connect with creds from calculator Str9
bool nasConnectWithCreds(const char* ssid, const char* pass) {
    currentSSID = String(ssid);
    currentPass = String(pass);

    Serial.print("WiFi connecting to: "); Serial.println(ssid);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid, pass);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi failed");
        return false;
    }

    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    return nasLogin();
}

void nasDisconnect() {
    if (!connected) return;
    String url = buildUrl("SYNO.API.Auth", "logout", 6);
    String response;
    httpGet(url, response);
    sid = "";
    synoToken = "";
    connected = false;
    WiFi.disconnect(true);
    Serial.println("NAS disconnected");
}

bool nasIsConnected() {
    return connected && WiFi.status() == WL_CONNECTED;
}

String nasGetSSID() {
    return currentSSID;
}

bool nasListFiles(char* outBuffer, int maxLen) {
    String folderEncoded = urlEncode(String(NAS_FOLDER));
    String extra = "folder_path=" + folderEncoded;
    String url = buildUrl("SYNO.FileStation.List", "list", 2, extra.c_str());
    String response;
    if (!httpGet(url, response)) return false;

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, response);
    if (err || !doc["success"].as<bool>()) {
        Serial.println(response);
        return false;
    }

    String result = "";
    int i = 0;
    for (JsonObject file : doc["data"]["files"].as<JsonArray>()) {
        String name = file["name"].as<String>();
        // Only show TI file types
        if (name.endsWith(".8xk") || name.endsWith(".8xp") ||
            name.endsWith(".8xl") || name.endsWith(".8xv") ||
            name.endsWith(".8xn") || name.endsWith(".8xi")) {
            result += String(i + 1) + ":" + name + "\n";
            i++;
        }
    }

    strncpy(outBuffer, result.c_str(), maxLen);
    return i > 0;
}

bool nasDownloadFile(const char* filename, uint8_t* outBuffer, int* outLen, int maxLen) {
    String path = String(NAS_FOLDER) + "/" + String(filename);
    String extra = "path=" + urlEncode(path) + "&mode=download";
    String url = buildUrl("SYNO.FileStation.Download", "download", 2, extra.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        Serial.print("Download HTTP error: "); Serial.println(code);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    int len = 0;
    unsigned long timeout = millis() + 10000;
    while ((stream->available() || millis() < timeout) && len < maxLen) {
        if (stream->available()) {
            outBuffer[len++] = stream->read();
            timeout = millis() + 2000;
        }
    }
    *outLen = len;
    http.end();
    Serial.print("Downloaded "); Serial.print(len); Serial.println(" bytes");
    return len > 0;
}