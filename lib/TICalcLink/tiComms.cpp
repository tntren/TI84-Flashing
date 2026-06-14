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

int tiSendProgram(uint8_t* fileData, int fileSize) {
    // Verified .8xp layout (from real TI-84 Plus file):
    //   fileData[0..54]   = file header
    //   varEntry = fileData+55:
    //     [0..1]  = 0x0D 0x00  (entry flag)
    //     [2..3]  = varDataLen (little-endian) — covers type+name+ver+arch+2+tokens
    //     [4]     = type (0x05 = program)
    //     [5..12] = name (8 bytes, null-padded)
    //     [13]    = version
    //     [14]    = archived flag
    //     [15..16]= progDataLen (token body length, little-endian)
    //     [17+]   = token bytes

    uint8_t* varEntry = fileData + 55;

    uint16_t varDataLen  = varEntry[2] | ((uint16_t)varEntry[3] << 8);
    uint16_t progDataLen = varEntry[15] | ((uint16_t)varEntry[16] << 8);
    uint8_t* progData    = varEntry + 17;

    // Build 13-byte RTS variable header for TI-83+/84+:
    //   [0..1]  = varDataLen
    //   [2]     = type
    //   [3..10] = name (8 bytes)
    //   [11]    = version
    //   [12]    = archived
    uint8_t varHeader[13];
    varHeader[0] = varEntry[2];   // varDataLen lo
    varHeader[1] = varEntry[3];   // varDataLen hi
    varHeader[2] = varEntry[4];   // type = 0x05
    memcpy(&varHeader[3], &varEntry[5], 8);  // name
    varHeader[11] = varEntry[13]; // version
    varHeader[12] = varEntry[14]; // archived

    char nameBuf[9] = {0};
    memcpy(nameBuf, &varEntry[5], 8);
    Serial.printf("Sending var: %s type=0x%02x varDataLen=%d progDataLen=%d\n",
        nameBuf, varEntry[4], varDataLen, progDataLen);

    int result = cbl.sendToCalc(varEntry[4], varHeader, 13, progData, progDataLen);
    if (result == 0) {
        Serial.println("Transfer done");
    } else {
        Serial.printf("Transfer failed: %d\n", result);
    }
    return result;
}

void tiSendExamExit() {
    Serial.println("Exam exit: downloading BROWSER.8xp");

    uint8_t* fileBuffer = (uint8_t*)malloc(FILE_BUFFER_SIZE);
    if (!fileBuffer) {
        Serial.println("malloc failed");
        return;
    }

    int fileLen = 0;
    if (!nasDownloadFile("BROWSER.8xp", fileBuffer, &fileLen, FILE_BUFFER_SIZE)) {
        Serial.println("Download failed");
        free(fileBuffer);
        return;
    }

    Serial.print("Downloaded ");
    Serial.print(fileLen);
    Serial.println(" bytes, sending to calc");

    int result = tiSendProgram(fileBuffer, fileLen);
    free(fileBuffer);

    if (result == 0) {
        Serial.println("Exam exit done");
    } else {
        Serial.print("Exam exit failed: ");
        Serial.println(result);
    }
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