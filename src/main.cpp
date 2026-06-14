#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "tiComms.h"
#include "nasClient.h"
#include "commands.h"

static String serialBuf = "";

void setup() {
    Serial.begin(BAUD_RATE);
    delay(500);
    Serial.println("TI84-Flashing starting...");

    // Try to load WiFi creds from NVS (saved by calc via Str9)
    Preferences prefs;
    prefs.begin("nasconf", true); // read-only
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() > 0) {
        Serial.print("NVS creds found, connecting to: ");
        Serial.println(ssid);
        nasConnectWithCreds(ssid.c_str(), pass.c_str());
    } else {
        // Fall back to hardcoded creds in config.h
        Serial.println("No NVS creds, using config.h defaults");
        nasConnect();
    }

    tiSetup();
    Serial.println("Ready. Send Str9=SSID:PASS from calc to connect.");
    Serial.println("Type HELP for serial commands.");
}

void loop() {
    // Handle serial commands
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialBuf.length() > 0) {
                handleSerialCommand(serialBuf);
                serialBuf = "";
            }
        } else {
            serialBuf += c;
        }
    }

    // Handle TI link protocol events
    tiTick();
}