#include "commands.h"
#include "config.h"
#include "tiComms.h"
#include "nasClient.h"

static uint8_t fileBuffer[MAXDATALEN];

void handleSerialCommand(String input) {
    input.trim();

    if (input.equalsIgnoreCase("HELP")) {
        Serial.println("Commands:");
        Serial.println("  A=9          queue real variable");
        Serial.println("  STR1=HELLO   queue string into Str1-Str9");
        Serial.println("  CONNECT      connect to WiFi and NAS");
        Serial.println("  DISCONNECT   logout and disconnect");
        Serial.println("  LIST         list files on NAS");
        Serial.println("  SEND:file.8xk  download and queue file from NAS");
        Serial.println("  EXAM         exit exam mode");
        return;
    }

    if (input.equalsIgnoreCase("EXAM")) {
        delay(1000);
        tiSendExamExit();
        return;
    }

    if (input.equalsIgnoreCase("CONNECT")) {
        nasConnect();
        return;
    }

    if (input.equalsIgnoreCase("DISCONNECT")) {
        nasDisconnect();
        return;
    }

    if (input.equalsIgnoreCase("LIST")) {
        if (!nasIsConnected()) {
            Serial.println("Not connected, run CONNECT first");
            return;
        }
        char listBuf[2048];
        if (nasListFiles(listBuf, sizeof(listBuf))) {
            Serial.println("Files on NAS:");
            Serial.println(listBuf);
        } else {
            Serial.println("Failed to list files");
        }
        return;
    }

    if (input.startsWith("SEND:")) {
        if (!nasIsConnected()) {
            Serial.println("Not connected, run CONNECT first");
            return;
        }
        String filename = input.substring(5);
        filename.trim();
        if (!filename.endsWith(".8xp")) {
            Serial.println("Only .8xp program files are supported, use TI-Connect for .8xk apps");
            return;
        }
        int fileLen = 0;
        Serial.print("Downloading: ");
        Serial.println(filename);
        if (nasDownloadFile(filename.c_str(), fileBuffer, &fileLen, MAXDATALEN)) {
            Serial.println("Sending to calculator...");
            delay(1000);
            int result = tiSendProgram(fileBuffer, fileLen);
            if (result == 0) {
                Serial.println("Done, check your calculator");
            } else {
                Serial.print("Transfer failed: ");
                Serial.println(result);
            }
        } else {
            Serial.println("Download failed");
        }
        return;
    }

    if (input.startsWith("STR")) {
        int eqIdx = input.indexOf('=');
        if (eqIdx < 4) { Serial.println("Bad format. Use: STR1=HELLO"); return; }
        char strIndex = input[3] - '1';
        String text = input.substring(eqIdx + 1);
        text.trim();
        tiQueueString(strIndex, text.c_str());
        Serial.print("Run Get(Str");
        Serial.print((int)strIndex + 1);
        Serial.println(") on calculator");
        return;
    }

    int eqIdx = input.indexOf('=');
    if (eqIdx == 1 && input[0] >= 'A' && input[0] <= 'Z') {
        char varName = input[0];
        double value = input.substring(2).toDouble();
        tiQueueReal(varName, value);
        Serial.print("Run Get(");
        Serial.print(varName);
        Serial.println(") on calculator");
        return;
    }

    Serial.println("Unknown command. Type HELP for commands");
}