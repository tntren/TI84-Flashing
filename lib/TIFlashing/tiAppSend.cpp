#include "tiAppSend.h"
#include "tiComms.h"

#define FLASH_APP_TYPE 0x24
#define APP_CHUNK_SIZE 0x80

#ifndef VER
#define VER 0x2D
#endif

struct FlashPage {
    uint16_t addr;
    uint16_t page;
    uint8_t  flag;
    uint8_t  data[0x4000];
    uint16_t size;
};

static uint8_t hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static uint8_t hexByte(const char* s) {
    return (hexNib(s[0]) << 4) | hexNib(s[1]);
}

static bool parseFlashPage(const uint8_t* hexData, uint32_t hexLen, FlashPage* fp) {
    memset(fp->data, 0xFF, sizeof(fp->data));
    fp->addr = 0; fp->page = 0; fp->flag = 0x80; fp->size = 0;
    uint8_t flag = 0x80;
    uint16_t flashPage = 0, flashAddr = 0;
    bool newPage = false, gotAddr = false;
    uint32_t i = 0;
    while (i < hexLen) {
        if (hexData[i] != ':') { i++; continue; }
        i++;
        uint8_t length   = hexByte((const char*)&hexData[i]); i += 2;
        uint16_t addr    = ((uint16_t)hexByte((const char*)&hexData[i])) << 8; i += 2;
        addr            |= hexByte((const char*)&hexData[i]); i += 2;
        uint8_t rectype  = hexByte((const char*)&hexData[i]); i += 2;
        if (rectype == 0x01) {
            flashAddr = 0; flashPage = 0; flag ^= 0x80;
            if (fp->size > 0) break;
            i += 2; continue;
        }
        if (rectype == 0x02) {
            uint16_t ph = hexByte((const char*)&hexData[i]); i += 2;
            uint16_t pl = hexByte((const char*)&hexData[i]); i += 2;
            flashPage = (ph << 8) | pl;
            newPage = true;
            i += 2; continue;
        }
        if (rectype == 0x00) {
            if (newPage) { flashAddr = addr; newPage = false; }
            if (!gotAddr) {
                fp->addr = flashAddr; fp->page = flashPage;
                fp->flag = flag; gotAddr = true;
            }
            uint16_t offset = addr - fp->addr;
            for (int b = 0; b < length; b++) {
                uint8_t byte = hexByte((const char*)&hexData[i]); i += 2;
                if (offset + b < 0x4000) {
                    fp->data[offset + b] = byte;
                    if ((uint16_t)(offset + b + 1) > fp->size)
                        fp->size = offset + b + 1;
                }
            }
            i += 2; continue;
        }
        i += length * 2 + 2;
    }
    return gotAddr;
}

void tiDebugParseApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) { Serial.println("File too small"); return; }
    uint8_t nameLen = fileData[0x10];
    char appName[9] = {0};
    memcpy(appName, &fileData[0x11], min((int)nameLen, 8));
    uint32_t hexLen = fileData[0x4A]
                    | ((uint32_t)fileData[0x4B] << 8)
                    | ((uint32_t)fileData[0x4C] << 16)
                    | ((uint32_t)fileData[0x4D] << 24);
    Serial.print("App name: ");       Serial.println(appName);
    Serial.print("Hex payload len: "); Serial.println(hexLen);
    static FlashPage fp;
    if (!parseFlashPage(&fileData[0x4E], hexLen, &fp)) {
        Serial.println("Parse failed"); return;
    }
    Serial.print("Flash page: ");   Serial.println(fp.page);
    Serial.print("Flash addr: 0x"); Serial.println(fp.addr, HEX);
    Serial.print("Flag: 0x");       Serial.println(fp.flag, HEX);
    Serial.print("Data size: ");    Serial.println(fp.size);
    Serial.print("First 16 bytes: ");
    for (int b = 0; b < 16; b++) {
        if (fp.data[b] < 0x10) Serial.print("0");
        Serial.print(fp.data[b], HEX);
    }
    Serial.println();
}

static int getVersionHandshake() {
    uint8_t msgHdr[4];
    int dataLength = 0;

    msgHdr[0] = COMP83P; msgHdr[1] = VER; msgHdr[2] = 0x02; msgHdr[3] = 0x00;
    if (cbl.send(msgHdr, NULL, 0)) { Serial.println("VER send failed"); return -1; }

    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after VER, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    msgHdr[0] = COMP83P; msgHdr[1] = CTS; msgHdr[2] = 0; msgHdr[3] = 0;
    if (cbl.send(msgHdr, NULL, 0)) { Serial.println("CTS send failed"); return -1; }

    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after CTS, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    uint8_t verBuf[16];
    dataLength = 0;
    if (cbl.get(msgHdr, verBuf, &dataLength, sizeof(verBuf)) || msgHdr[1] != DATA) {
        Serial.print("Expected version DATA, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    msgHdr[0] = COMP83P; msgHdr[1] = ACK; msgHdr[2] = 0; msgHdr[3] = 0;
    if (cbl.send(msgHdr, NULL, 0)) { Serial.println("Final ACK failed"); return -1; }

    Serial.print("Calc version info (");
    Serial.print(dataLength); Serial.print(" bytes): ");
    for (int b = 0; b < dataLength; b++) {
        if (verBuf[b] < 0x10) Serial.print("0");
        Serial.print(verBuf[b], HEX);
    }
    Serial.println();
    return 0;
}

// Send one 128-byte chunk:
// VAR -> ACK -> CTS -> ACK -> DATA(128) -> ACK
static int sendChunk(uint8_t* data, uint16_t chunkAddr, uint16_t flashPage, uint8_t flag) {
    uint8_t msgHdr[4];
    int dataLength = 0;

    // VAR header: 10 bytes
    uint8_t varHdr[10];
    varHdr[0] = APP_CHUNK_SIZE & 0xFF;
    varHdr[1] = (APP_CHUNK_SIZE >> 8) & 0xFF;
    varHdr[2] = FLASH_APP_TYPE;
    varHdr[3] = APP_CHUNK_SIZE & 0xFF;
    varHdr[4] = (APP_CHUNK_SIZE >> 8) & 0xFF;
    varHdr[5] = flag;
    varHdr[6] = chunkAddr & 0xFF;
    varHdr[7] = (chunkAddr >> 8) & 0xFF;
    varHdr[8] = flashPage & 0xFF;
    varHdr[9] = (flashPage >> 8) & 0xFF;

    // VAR
    msgHdr[0] = COMP83P; msgHdr[1] = VAR; msgHdr[2] = 10; msgHdr[3] = 0;
    if (cbl.send(msgHdr, varHdr, 10)) {
        Serial.println("VAR send failed"); return -1;
    }

    // ACK
    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after VAR, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    // CTS — 4 bytes only, length field echoes chunk size but there is no payload
    if (cbl.get(msgHdr, NULL, &dataLength, 0, 30000000UL) || msgHdr[1] != CTS) {
        Serial.print("Expected CTS, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    // ACK the CTS
    msgHdr[0] = COMP83P; msgHdr[1] = ACK; msgHdr[2] = 0; msgHdr[3] = 0;
    if (cbl.send(msgHdr, NULL, 0)) {
        Serial.println("ACK after CTS failed"); return -1;
    }

    // DATA — 128 bytes
    msgHdr[0] = COMP83P; msgHdr[1] = DATA;
    msgHdr[2] = APP_CHUNK_SIZE & 0xFF;
    msgHdr[3] = (APP_CHUNK_SIZE >> 8) & 0xFF;
    if (cbl.send(msgHdr, data, APP_CHUNK_SIZE)) {
        Serial.println("DATA send failed"); return -1;
    }

    // ACK after DATA
    dataLength = 0;
    if (cbl.get(msgHdr, NULL, &dataLength, 0, 10000000UL) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after DATA, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }

    return 0;
}

int tiSendApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) { Serial.println("File too small"); return -1; }

    uint32_t hexLen = fileData[0x4A]
                    | ((uint32_t)fileData[0x4B] << 8)
                    | ((uint32_t)fileData[0x4C] << 16)
                    | ((uint32_t)fileData[0x4D] << 24);

    static FlashPage fp;
    if (!parseFlashPage(&fileData[0x4E], hexLen, &fp)) {
        Serial.println("Failed to parse Intel HEX"); return -1;
    }

    Serial.print("Sending app: page="); Serial.print(fp.page);
    Serial.print(" addr=0x"); Serial.print(fp.addr, HEX);
    Serial.print(" flag=0x"); Serial.print(fp.flag, HEX);
    Serial.print(" size="); Serial.println(fp.size);

    if (getVersionHandshake() != 0) {
        Serial.println("Version handshake failed"); return -1;
    }

    // 128 chunks of 128 bytes = 16384 bytes = one full flash page
    int numChunks = 0x4000 / APP_CHUNK_SIZE;
    for (int chunk = 0; chunk < numChunks; chunk++) {
        uint16_t chunkAddr = fp.addr + (chunk * APP_CHUNK_SIZE);
        Serial.print("Chunk "); Serial.print(chunk + 1);
        Serial.print("/128 addr=0x"); Serial.println(chunkAddr, HEX);
        if (sendChunk(&fp.data[chunk * APP_CHUNK_SIZE], chunkAddr, fp.page, fp.flag) != 0) {
            Serial.print("Failed at chunk "); Serial.println(chunk + 1);
            return -1;
        }
    }

    // EOT
    uint8_t msgHdr[4];
    int dataLength = 0;
    msgHdr[0] = COMP83P; msgHdr[1] = EOT; msgHdr[2] = 0; msgHdr[3] = 0;
    if (cbl.send(msgHdr, NULL, 0)) { Serial.println("EOT send failed"); return -1; }
    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after EOT, got: 0x"); Serial.println(msgHdr[1], HEX); return -1;
    }
    Serial.println("App transfer complete.");
    return 0;
}