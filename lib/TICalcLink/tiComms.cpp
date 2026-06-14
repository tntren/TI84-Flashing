#include "tiComms.h"
#include "nasClient.h"
#include "tiAppSend.h"
#include <Preferences.h>

CBL2 cbl;
uint8_t tiHeader[MAXHDRLEN];
uint8_t tiData[MAXDATALEN];

// Queue state for outgoing reals
static char pendingVarName = '\0';
static double pendingReal = 0.0;
static bool realReady = false;

// Queue state for outgoing strings (up to 7: Str0-Str6)
#define MAX_QUEUED_STRS 7
static char pendingStrs[MAX_QUEUED_STRS][100];
static int pendingStrIndices[MAX_QUEUED_STRS];
static int queuedStrCount = 0;
static int currentStrIdx = 0;

#define FILE_BUFFER_SIZE 24576  // 24KB — fits a 16KB flash page + .8xk wrapper

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
    if (queuedStrCount >= MAX_QUEUED_STRS) return;
    pendingStrIndices[queuedStrCount] = strIndex;
    strncpy(pendingStrs[queuedStrCount], text, 99);
    pendingStrs[queuedStrCount][99] = '\0';
    queuedStrCount++;
    Serial.print("Queued Str");
    Serial.print((int)strIndex);
    Serial.print(" = ");
    Serial.println(text);
}

void tiClearStringQueue() {
    queuedStrCount = 0;
    currentStrIdx = 0;
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

    Serial.print("Sending var: ");
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
    if (rtsVal) { Serial.print("RTS failed: "); Serial.println(rtsVal); return rtsVal; }

    cbl.resetLines();

    auto ackVal = cbl.get(msgHeader, NULL, &dataLength, 0);
    if (ackVal || msgHeader[1] != ACK) { Serial.println("ACK failed"); return -1; }

    auto ctsVal = cbl.get(msgHeader, NULL, &dataLength, 0);
    if (ctsVal || msgHeader[1] != CTS) { Serial.println("CTS failed"); return -1; }

    msgHeader[1] = ACK; msgHeader[2] = 0x00; msgHeader[3] = 0x00;
    cbl.send(msgHeader, NULL, 0);

    msgHeader[1] = DATA;
    msgHeader[2] = progDataLen & 0xff;
    msgHeader[3] = (progDataLen >> 8) & 0xff;
    auto dataVal = cbl.send(msgHeader, progData, progDataLen);
    if (dataVal) { Serial.print("DATA failed: "); Serial.println(dataVal); return dataVal; }

    cbl.get(msgHeader, NULL, &dataLength, 0);

    msgHeader[1] = EOT; msgHeader[2] = 0x00; msgHeader[3] = 0x00;
    cbl.send(msgHeader, NULL, 0);
    cbl.get(msgHeader, NULL, &dataLength, 0);

    Serial.println("Transfer done");
    return 0;
}

void tiSendExamExit() {
    uint8_t msgHeader[4] = {COMP83P, RTS, 13, 0};
    uint8_t rtsData[13] = {0x09, 0x00, VarTypes82::VarProgram, 'D','U','M','M','Y',0x00,0x00,0x00,0x00,0x00};
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

// Determine transfer method from file extension
static bool isFlashApp(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".8xk") == 0;
}

// Handle incoming data from calculator
int onReceived(uint8_t type, enum Endpoint model, int datalen) {
    char varName = tiHeader[3];

    // ── Str9 received → WiFi credentials ──────────────────────────
    // Calc sends Str9 = "SSID:PASS" on startup
    if (type == VarTypes82::VarString && varName == 0x09) {
        String creds = TIVar::strVarToString8x(tiData, model);
        Serial.print("Received creds: ");
        Serial.println(creds);

        int sep = creds.indexOf(':');
        if (sep < 0) {
            Serial.println("Bad creds format, expected SSID:PASS");
            tiQueueReal('A', 0);
            return 0;
        }

        String ssid = creds.substring(0, sep);
        String pass = creds.substring(sep + 1);

        // Save to NVS
        Preferences prefs;
        prefs.begin("nasconf", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();

        Serial.print("Connecting to: "); Serial.println(ssid);

        // Disconnect first if already connected
        nasDisconnect();

        // Update config and connect
        bool ok = nasConnectWithCreds(ssid.c_str(), pass.c_str());
        tiQueueReal('A', ok ? 1 : 0);
        return 0;
    }

    // ── Real T received → command ──────────────────────────────────
    if (type == VarTypes82::VarReal && varName == 'T') {
        double val = TIVar::realToFloat8x(tiData, model);
        int cmd = (int)val;
        Serial.print("Command T="); Serial.println(cmd);

        // T=1 → list files
        if (cmd == 1) {
            if (!nasIsConnected()) {
                Serial.println("Not connected");
                tiQueueReal('N', -1);
                return 0;
            }

            // Get file list from NAS
            char listBuf[2048];
            if (!nasListFiles(listBuf, sizeof(listBuf))) {
                Serial.println("List failed");
                tiQueueReal('N', 0);
                return 0;
            }

            // Parse "1:file.8xk\n2:file2.8xp\n..." into individual strings
            // Queue N (count) first, then Str1-Str6 with filenames
            tiClearStringQueue();
            int count = 0;
            char* line = strtok(listBuf, "\n");
            while (line && count < 6) {
                char* colon = strchr(line, ':');
                if (colon) {
                    char fname[96];
                    strncpy(fname, colon + 1, 95);
                    fname[95] = '\0';
                    // Truncate at 14 chars to fit TI screen
                    if (strlen(fname) > 14) fname[14] = '\0';
                    tiQueueString(count + 1, fname); // Str1=file1, Str2=file2 etc
                    count++;
                }
                line = strtok(NULL, "\n");
            }
            tiQueueReal('N', count);
            Serial.print("Queued "); Serial.print(count); Serial.println(" files");
            return 0;
        }

        // T=2 → transfer file (filename arrives next as Str0)
        if (cmd == 2) {
            // filename will come in next string receive, handled there
            Serial.println("Ready for filename");
            return 0;
        }

        // T=7829 → exam exit
        if (cmd == 7829) {
            delay(500);
            tiSendExamExit();
            return 0;
        }
    }

    // ── Str0 received → filename to transfer ─────────────────────
    if (type == VarTypes82::VarString && varName == 0x00) {
        String filename = TIVar::strVarToString8x(tiData, model);
        filename.trim();
        Serial.print("Transfer request: "); Serial.println(filename);

        if (!nasIsConnected()) {
            tiQueueReal('A', -1);
            return 0;
        }

        uint8_t* fileBuffer = (uint8_t*)malloc(FILE_BUFFER_SIZE);
        if (!fileBuffer) {
            Serial.println("malloc failed for file buffer");
            tiQueueReal('A', -3);
            return 0;
        }

        int fileLen = 0;
        if (!nasDownloadFile(filename.c_str(), fileBuffer, &fileLen, FILE_BUFFER_SIZE)) {
            Serial.println("Download failed");
            free(fileBuffer);
            tiQueueReal('A', -2);
            return 0;
        }

        Serial.print("Downloaded "); Serial.print(fileLen); Serial.println(" bytes");
        delay(5500); // calc is going to home screen

        int result;
        if (isFlashApp(filename.c_str())) {
            Serial.println("Sending as flash app (.8xk)");
            result = tiSendApp(fileBuffer, fileLen);
        } else {
            Serial.println("Sending as variable (.8xp/.8xl etc)");
            result = tiSendProgram(fileBuffer, fileLen);
        }

        free(fileBuffer);
        tiQueueReal('A', result == 0 ? 1 : result);
        return 0;
    }

    return 0;
}

// Handle calculator requesting a variable
int onRequest(uint8_t type, enum Endpoint model, int* headerlen, int* datalen, data_callback* cb) {
    char varName = tiHeader[3];
    char strIndex = tiHeader[4];
    Serial.print("Calc requested: type=0x");
    Serial.print(type, HEX);
    Serial.print(" var=");
    Serial.println(varName);

    memset(tiHeader, 0, sizeof(tiHeader));

    // Send queued real
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

    // Send next queued string
    if (type == VarTypes82::VarString && currentStrIdx < queuedStrCount) {
        *datalen = TIVar::stringToStrVar8x(String(pendingStrs[currentStrIdx]), tiData, model);
        TIVar::intToSizeWord(*datalen, tiHeader);
        tiHeader[2] = VarTypes82::VarString;
        tiHeader[3] = 0xAA;
        tiHeader[4] = pendingStrIndices[currentStrIdx];
        *headerlen = 13;
        currentStrIdx++;
        return 0;
    }

    Serial.println("Nothing queued");
    return -1;
}