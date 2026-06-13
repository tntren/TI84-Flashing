#include "tiAppSend.h"
#include "tiComms.h"

#define APP_PAGE_SIZE 128
#define VAR2 0x24

static uint8_t hexByte(const char* s) {
    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    return (nib(s[0]) << 4) | nib(s[1]);
}

int parseIntelHexPage(const uint8_t* hexData, uint32_t hexLen, uint8_t* pageBuf) {
    memset(pageBuf, 0xFF, 0x4000);

    uint32_t extAddr = 0;
    int pageNum = -1;
    uint32_t i = 0;

    while (i < hexLen) {
        if (hexData[i] != ':') { i++; continue; }
        i++;

        uint8_t length = hexByte((const char*)&hexData[i]); i += 2;
        uint16_t addr = (hexByte((const char*)&hexData[i]) << 8); i += 2;
        addr |= hexByte((const char*)&hexData[i]); i += 2;
        uint8_t rectype = hexByte((const char*)&hexData[i]); i += 2;

        if (rectype == 0x01) break;

        if (rectype == 0x02) {
            uint16_t segHi = hexByte((const char*)&hexData[i]); i += 2;
            uint16_t segLo = hexByte((const char*)&hexData[i]); i += 2;
            extAddr = ((uint32_t)((segHi << 8) | segLo)) << 4;
            i += 2;
            continue;
        }

        if (rectype == 0x00) {
            uint32_t fullAddr = extAddr + addr;
            int thisPage = fullAddr >> 14;
            uint32_t offsetInPage = fullAddr & 0x3FFF;

            if (pageNum == -1) pageNum = thisPage;

            for (int b = 0; b < length; b++) {
                if (offsetInPage + b < 0x4000) {
                    pageBuf[offsetInPage + b] = hexByte((const char*)&hexData[i]);
                }
                i += 2;
            }
            i += 2;
            continue;
        }

        i += length * 2 + 2;
    }

    return pageNum;
}

void tiDebugParseApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) {
        Serial.println("File too small to be a valid .8xk");
        return;
    }

    uint8_t nameLen = fileData[0x10];
    char appName[9] = {0};
    memcpy(appName, &fileData[0x11], min((int)nameLen, 8));

    uint32_t hexLen = fileData[0x4A] | (fileData[0x4B] << 8)
                     | ((uint32_t)fileData[0x4C] << 16) | ((uint32_t)fileData[0x4D] << 24);

    Serial.print("App name: ");
    Serial.println(appName);
    Serial.print("Hex payload length: ");
    Serial.println(hexLen);

    if ((uint32_t)fileLen < 0x4E + hexLen) {
        Serial.println("File length mismatch");
        return;
    }

    static uint8_t pageBuf[0x4000];
    int pageNum = parseIntelHexPage(&fileData[0x4E], hexLen, pageBuf);

    Serial.print("Parsed flash page: ");
    Serial.println(pageNum);

    Serial.print("First 16 bytes: ");
    for (int b = 0; b < 16; b++) {
        if (pageBuf[b] < 0x10) Serial.print("0");
        Serial.print(pageBuf[b], HEX);
    }
    Serial.println();

    Serial.print("Last 16 bytes: ");
    for (int b = 0x4000 - 16; b < 0x4000; b++) {
        if (pageBuf[b] < 0x10) Serial.print("0");
        Serial.print(pageBuf[b], HEX);
    }
    Serial.println();
}

static int sendAppPage(uint8_t* pageData, uint16_t pageDataLen,
                        uint16_t flashOffset, uint16_t flashPage) {
    uint8_t msgHeader[4] = {COMP83P, VAR, 10, 0};
    uint8_t varHeader[10];

    varHeader[0] = pageDataLen & 0xFF;
    varHeader[1] = (pageDataLen >> 8) & 0xFF;
    varHeader[2] = VAR2;
    varHeader[3] = pageDataLen & 0xFF;
    varHeader[4] = (pageDataLen >> 8) & 0xFF;
    varHeader[5] = 0x80;
    varHeader[6] = flashOffset & 0xFF;
    varHeader[7] = (flashOffset >> 8) & 0xFF;
    varHeader[8] = flashPage & 0xFF;
    varHeader[9] = (flashPage >> 8) & 0xFF;

    int dataLength = 0;

    if (cbl.send(msgHeader, varHeader, 10)) {
        Serial.println("VAR failed");
        return -1;
    }

    if (cbl.get(msgHeader, NULL, &dataLength, 0) || msgHeader[1] != ACK) {
        Serial.println("ACK (after VAR) failed");
        return -1;
    }

    if (cbl.get(msgHeader, NULL, &dataLength, 0) || msgHeader[1] != CTS) {
        Serial.println("CTS failed");
        return -1;
    }

    msgHeader[1] = ACK;
    msgHeader[2] = 0x00;
    msgHeader[3] = 0x00;
    cbl.send(msgHeader, NULL, 0);

    msgHeader[1] = DATA;
    msgHeader[2] = pageDataLen & 0xFF;
    msgHeader[3] = (pageDataLen >> 8) & 0xFF;
    if (cbl.send(msgHeader, pageData, pageDataLen)) {
        Serial.println("DATA failed");
        return -1;
    }

    if (cbl.get(msgHeader, NULL, &dataLength, 0) || msgHeader[1] != ACK) {
        Serial.println("ACK (after DATA) failed");
        return -1;
    }

    return 0;
}

int tiSendApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) {
        Serial.println("File too small to be a valid .8xk");
        return -1;
    }

    uint32_t hexLen = fileData[0x4A] | (fileData[0x4B] << 8)
                     | ((uint32_t)fileData[0x4C] << 16) | ((uint32_t)fileData[0x4D] << 24);

    if ((uint32_t)fileLen < 0x4E + hexLen) {
        Serial.println("File length mismatch");
        return -1;
    }

    static uint8_t pageBuf[0x4000];
    int pageNum = parseIntelHexPage(&fileData[0x4E], hexLen, pageBuf);
    if (pageNum < 0) {
        Serial.println("Failed to parse Intel HEX data");
        return -1;
    }

    Serial.print("Sending app, flash page: ");
    Serial.println(pageNum);

    int numChunks = 0x4000 / APP_PAGE_SIZE;

    for (int chunk = 0; chunk < numChunks; chunk++) {
        uint16_t flashOffset = chunk * APP_PAGE_SIZE;

        Serial.print("Sending chunk ");
        Serial.print(chunk + 1);
        Serial.print("/");
        Serial.println(numChunks);

        int result = sendAppPage(&pageBuf[flashOffset], APP_PAGE_SIZE, flashOffset, pageNum);
        if (result != 0) {
            Serial.print("Chunk ");
            Serial.print(chunk);
            Serial.println(" failed");
            return result;
        }
    }

    uint8_t msgHeader[4] = {COMP83P, EOT, 0x00, 0x00};
    cbl.send(msgHeader, NULL, 0);

    int dataLength = 0;
    cbl.get(msgHeader, NULL, &dataLength, 0);

    Serial.println("App transfer done");
    return 0;
}