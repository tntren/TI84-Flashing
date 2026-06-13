#include "tiComms.h"
#include "nasClient.h"

CBL2 cbl;
uint8_t tiHeader[MAXHDRLEN];
uint8_t tiData[MAXDATALEN];

static char pendingVarName = '\0';
static double pendingReal = 0.0;
static bool realReady = false;

static char pendingStrIndex = 0;
static char pendingStr[MAXDATALEN];
static bool strReady = false;

void tiSetup() {
    cbl.setLines(TIP_PIN, RING_PIN);
    cbl.resetLines();
    cbl.setupCallbacks(tiHeader, tiData, MAXDATALEN, onReceived, onRequest);
}

void tiTick() {
    cbl.eventLoopTick();
}

void tiQueueReal(char varName, double value) {
    pendingVarName = varName;
    pendingReal = value;
    realReady = true;
    Serial.print("Queued real: ");
    Serial.print(varName);
    Serial.print(" = ");
    Serial.println(value);
}

void tiQueueString(char strIndex, const char* text) {
    pendingStrIndex = strIndex;
    strncpy(pendingStr, text, MAXDATALEN);
    strReady = true;
    Serial.print("Queued string Str");
    Serial.print((int)strIndex + 1);
    Serial.print(" = ");
    Serial.println(text);
}

int tiSendProgram(uint8_t* fileData, int fileLen) {
    if (fileLen < 74) {
        Serial.println("File too small to be a valid .8xp");
        return -1;
    }

    uint8_t* varData = fileData + 55;
    int varDataLen = fileLen - 55 - 2;

    uint16_t dataLen = varData[2] | (varData[3] << 8);
    uint8_t varType = varData[4];
    char varName[9] = {0};
    memcpy(varName, &varData[5], 8);

    Serial.print("Sending program: ");
    Serial.print(varName);
    Serial.print(" type=0x");
    Serial.print(varType, HEX);
    Serial.print(" len=");
    Serial.println(dataLen);

    uint8_t* progData = varData + 15;
    int progDataLen = dataLen;

    uint8_t msgHeader[4] = {COMP83P, RTS, 13, 0};
    uint8_t rtsData[13];
    rtsData[0] = dataLen & 0xff;
    rtsData[1] = (dataLen >> 8) & 0xff;
    rtsData[2] = varType;
    memset(&rtsData[3], 0, 10);
    memcpy(&rtsData[3], varName, min((int)strlen(varName), 8));

    int dataLength = 0;

    auto rtsVal = cbl.send(msgHeader, rtsData, 13);
    if (rtsVal) {
        Serial.print("RTS failed: ");
        Serial.println(rtsVal);
        return rtsVal;
    }

    cbl.resetLines();

    auto ackVal = cbl.get(msgHeader, NULL, &dataLength, 0);
    if (ackVal || msgHeader[1] != ACK) {
        Serial.print("ACK failed: ");
        Serial.println(ackVal);
        return -1;
    }

    auto ctsVal = cbl.get(msgHeader, NULL, &dataLength, 0);
    if (ctsVal || msgHeader[1] != CTS) {
        Serial.print("CTS failed: ");
        Serial.println(ctsVal);
        return -1;
    }

    msgHeader[1] = ACK;
    msgHeader[2] = 0x00;
    msgHeader[3] = 0x00;
    cbl.send(msgHeader, NULL, 0);

    msgHeader[1] = DATA;
    msgHeader[2] = progDataLen & 0xff;
    msgHeader[3] = (progDataLen >> 8) & 0xff;
    auto dataVal = cbl.send(msgHeader, progData, progDataLen);
    if (dataVal) {
        Serial.print("DATA failed: ");
        Serial.println(dataVal);
        return dataVal;
    }

    cbl.get(msgHeader, NULL, &dataLength, 0);

    msgHeader[1] = EOT;
    msgHeader[2] = 0x00;
    msgHeader[3] = 0x00;
    cbl.send(msgHeader, NULL, 0);

    cbl.get(msgHeader, NULL, &dataLength, 0);

    Serial.println("Transfer done");
    return 0;
}

void tiSendExamExit() {
    uint8_t msgHeader[4] = {COMP83P, RTS, 13, 0};
    uint8_t rtsData[13] = {0x09, 0x00, VarTypes82::VarProgram, 'D', 'U', 'M', 'M', 'Y', 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t progData[9] = {0x09, 0x00, 0x3F, 0x00, 0xDE, 0x00, 0x00, 0x00, 0x00};
    int dataLen = 0;

    Serial.println("Sending exam exit...");

    if (cbl.send(msgHeader, rtsData, 13)) { Serial.println("RTS failed"); return; }
    cbl.resetLines();
    if (cbl.get(msgHeader, NULL, &dataLen, 0) || msgHeader[1] != ACK) { Serial.println("ACK failed"); return; }
    if (cbl.get(msgHeader, NULL, &dataLen, 0) || msgHeader[1] != CTS) { Serial.println("CTS failed"); return; }

    msgHeader[1] = ACK; msgHeader[2] = 0; msgHeader[3] = 0;
    cbl.send(msgHeader, NULL, 0);

    msgHeader[1] = DATA;
    msgHeader[2] = sizeof(progData) & 0xff;
    msgHeader[3] = (sizeof(progData) >> 8) & 0xff;
    if (cbl.send(msgHeader, progData, sizeof(progData))) { Serial.println("DATA failed"); return; }

    cbl.get(msgHeader, NULL, &dataLen, 0);
    msgHeader[1] = EOT; msgHeader[2] = 0; msgHeader[3] = 0;
    cbl.send(msgHeader, NULL, 0);
    Serial.println("Exam exit done");
}

int onReceived(uint8_t type, enum Endpoint model, int datalen) {
    char varName = tiHeader[3];
    Serial.print("Received var: ");
    Serial.print(varName);

    if (type == VarTypes82::VarReal) {
        double val = TIVar::realToFloat8x(tiData, model);
        Serial.print(" = ");
        Serial.println(val);

        if (varName == 'T' && (int)val == 7829) {
            Serial.println("Trigger received, sending SGF.8xp...");
            delay(500);
            if (nasIsConnected()) {
                uint8_t fileBuffer[MAXDATALEN];
                int fileLen = 0;
                if (nasDownloadFile("SGF.8xp", fileBuffer, &fileLen, MAXDATALEN)) {
                    tiSendProgram(fileBuffer, fileLen);
                } else {
                    Serial.println("Download failed");
                }
            } else {
                Serial.println("Not connected to NAS");
            }
        }

        if (varName == 'Q' && (int)val == 7829) {
            Serial.println("Exam exit trigger received...");
            delay(500);
            tiSendExamExit();
        }

    } else if (type == VarTypes82::VarString) {
        Serial.print(" = ");
        Serial.println(TIVar::strVarToString8x(tiData, model));
    } else {
        Serial.println();
    }
    return 0;
}

int onRequest(uint8_t type, enum Endpoint model, int* headerlen, int* datalen, data_callback* cb) {
    char varName = tiHeader[3];
    char strIndex = tiHeader[4];
    Serial.print("Calculator requested: ");
    Serial.println(varName);

    memset(tiHeader, 0, sizeof(tiHeader));

    if (type == VarTypes82::VarReal && realReady && varName == pendingVarName) {
        *datalen = TIVar::floatToReal8x(pendingReal, tiData, model);
        TIVar::intToSizeWord(*datalen, tiHeader);
        tiHeader[2] = VarTypes82::VarReal;
        tiHeader[3] = varName;
        tiHeader[4] = '\0';
        *headerlen = 11;
        realReady = false;
        pendingVarName = '\0';
        return 0;
    }

    if (type == VarTypes82::VarString && strReady) {
        *datalen = TIVar::stringToStrVar8x(String(pendingStr), tiData, model);
        TIVar::intToSizeWord(*datalen, tiHeader);
        tiHeader[2] = VarTypes82::VarString;
        tiHeader[3] = 0xAA;
        tiHeader[4] = strIndex;
        *headerlen = 13;
        strReady = false;
        return 0;
    }

    Serial.println("Nothing queued for that variable");
    return -1;
}