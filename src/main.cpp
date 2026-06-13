#include <Arduino.h>
#include <LittleFS.h>
#include "tiAppSend.h"
#include "tiComms.h"

void setup() {
    Serial.begin(115200);
    delay(6000);

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        return;
    }

    File f = LittleFS.open("/noteflio.8xk", "r");
    if (!f) {
        Serial.println("File not found");
        return;
    }

    int fileLen = f.size();
    uint8_t* buf = (uint8_t*)malloc(fileLen);
    f.read(buf, fileLen);
    f.close();

    Serial.print("File size: ");
    Serial.println(fileLen);

    tiDebugParseApp(buf, fileLen);

    tiSetup();
    cbl.setVerbosity(true, &Serial);
    delay(2000);

    Serial.println("Attempting app send...");
    int result = tiSendApp(buf, fileLen);
    Serial.print("Result: ");
    Serial.println(result);

    free(buf);
}

void loop() {}