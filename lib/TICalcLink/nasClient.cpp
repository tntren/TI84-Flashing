#include "nasClient.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static String sid = "";
static String synoToken = "";
static bool connected = false;

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
    Serial.print("HTTP ");
    Serial.println(code);
    if (code <= 0) {
        Serial.print("HTTP error: ");
        Serial.println(http.errorToString(code));
        http.end();
        return false;
    }
    if (code != 200) {
        Serial.print("Unexpected HTTP code: ");
        Serial.println(code);
        http.end();
        return false;
    }
    responseOut = http.getString();
    http.end();
    return true;
}

bool nasConnect() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(" failed");
        return false;
    }
    Serial.println(" connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // Build login URL with properly encoded credentials
    String loginExtra = "account=" + urlEncode(String(NAS_USER))
                      + "&passwd=" + urlEncode(String(NAS_PASS))
                      + "&enable_syno_token=yes";
    String url = buildUrl("SYNO.API.Auth", "login", 6, loginExtra.c_str());

    Serial.println("Logging into NAS...");
    String response;
    if (!httpGet(url, response)) {
        Serial.println("NAS login request failed");
        return false;
    }

    Serial.println(response);  // print raw response so you can see what came back

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        Serial.print("JSON parse error: ");
        Serial.println(err.c_str());
        return false;
    }

    if (!doc["success"].as<bool>()) {
        int errCode = doc["error"]["code"].as<int>();
        Serial.print("NAS login rejected, error code: ");
        Serial.println(errCode);
        // Common codes: 400=invalid password, 401=account disabled, 403=permission denied
        return false;
    }

    sid = doc["data"]["sid"].as<String>();
    // Synology returns it as "synotoken" (lowercase)
    synoToken = doc["data"]["synotoken"].as<String>();
    if (synoToken.length() == 0) {
        // Some DSM versions capitalize it differently
        synoToken = doc["data"]["SynoToken"].as<String>();
    }

    connected = true;
    Serial.println("NAS logged in OK");
    Serial.print("SID: ");
    Serial.println(sid);
    return true;
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
    Serial.println("NAS logged out");
}

bool nasIsConnected() {
    return connected;
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
        result += String(i + 1) + ":" + file["name"].as<String>() + "\n";
        i++;
    }

    strncpy(outBuffer, result.c_str(), maxLen);
    return true;
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
    Serial.print("Download HTTP ");
    Serial.println(code);
    if (code != 200) {
        Serial.println(http.getString());
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    Serial.print("File size: ");
    Serial.println(contentLen);

    WiFiClient* stream = http.getStreamPtr();
    int len = 0;
    unsigned long timeout = millis() + 10000;
    while ((stream->available() || millis() < timeout) && len < maxLen) {
        if (stream->available()) {
            outBuffer[len++] = stream->read();
            timeout = millis() + 2000; // reset timeout on data
        }
    }
    *outLen = len;
    http.end();
    Serial.print("Downloaded bytes: ");
    Serial.println(len);
    return len > 0;
}