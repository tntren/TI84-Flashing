#include "tiAppSend.h"
#include "tiComms.h"

// Chunk size for apps is always 128 bytes (0x80) per libticalcs calc_73.cc
#define APP_CHUNK_SIZE 0x80

// DBUS command bytes (from libticalcs dbus_pkt.h)
// VAR  = 0x06  (already defined as VAR in tiComms.h / ArTICL)
// CTS  = 0x09  (already defined as CTS)
// XDP  = 0x15  (same byte as DATA in ArTICL - data packet)
// ACK  = 0x56  (already defined as ACK)
// EOT  = 0x92  (already defined as EOT)
// type byte for flash app = 0x24

#define FLASH_APP_TYPE 0x24

// -----------------------------------------------------------------------
// Intel HEX parser
// Matches libtifiles intelhex.cc logic exactly:
//   HEX_DATA = 0x00, HEX_END = 0x01, HEX_PAGE = 0x02, HEX_EOF = 0x03
//   flag starts at 0x80, toggles on each HEX_END record
//   page comes from HEX_PAGE records
//   addr comes from the first data record after a page change
// -----------------------------------------------------------------------

struct FlashPage {
    uint16_t addr;   // flash address (e.g. 0x4000)
    uint16_t page;   // flash page number
    uint8_t  flag;   // 0x80 or 0x00
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

// Parse the Intel HEX payload from a .8xk file into a single FlashPage struct.
// Returns true on success.
static bool parseFlashPage(const uint8_t* hexData, uint32_t hexLen, FlashPage* fp) {
    memset(fp->data, 0xFF, sizeof(fp->data));
    fp->addr  = 0;
    fp->page  = 0;
    fp->flag  = 0x80;   // starts at 0x80, same as libtifiles
    fp->size  = 0;

    uint8_t  flag       = 0x80;
    uint16_t flashPage  = 0;
    uint16_t flashAddr  = 0;
    bool     newPage    = false;
    bool     gotAddr    = false;

    uint32_t i = 0;
    while (i < hexLen) {
        // skip to next ':'
        if (hexData[i] != ':') { i++; continue; }
        i++; // skip ':'

        uint8_t  length  = hexByte((const char*)&hexData[i]); i += 2;
        uint16_t addr    = ((uint16_t)hexByte((const char*)&hexData[i])) << 8; i += 2;
        addr            |= hexByte((const char*)&hexData[i]); i += 2;
        uint8_t  rectype = hexByte((const char*)&hexData[i]); i += 2;

        if (rectype == 0x01) {
            // HEX_END: new section, toggle flag
            flashAddr = 0;
            flashPage = 0;
            flag ^= 0x80;
            // if we already have data, this marks end of our page
            if (fp->size > 0) break;
            i += 2; // checksum
            continue;
        }

        if (rectype == 0x02) {
            // HEX_PAGE: next two bytes are the page number (big-endian)
            uint16_t ph = hexByte((const char*)&hexData[i]); i += 2;
            uint16_t pl = hexByte((const char*)&hexData[i]); i += 2;
            flashPage = (ph << 8) | pl;
            newPage   = true;
            i += 2; // checksum
            continue;
        }

        if (rectype == 0x00) {
            // HEX_DATA
            if (newPage) {
                flashAddr = addr;
                newPage   = false;
            }
            if (!gotAddr) {
                fp->addr = flashAddr;
                fp->page = flashPage;
                fp->flag = flag;
                gotAddr  = true;
            }
            // copy bytes into page buffer at the right offset
            uint16_t offset = addr - fp->addr;
            for (int b = 0; b < length; b++) {
                uint8_t byte = hexByte((const char*)&hexData[i]); i += 2;
                if (offset + b < 0x4000) {
                    fp->data[offset + b] = byte;
                    if ((uint16_t)(offset + b + 1) > fp->size)
                        fp->size = offset + b + 1;
                }
            }
            i += 2; // checksum
            continue;
        }

        // unknown record type — skip data + checksum
        i += length * 2 + 2;
    }

    return gotAddr;
}

// -----------------------------------------------------------------------
// Debug helper
// -----------------------------------------------------------------------
void tiDebugParseApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) { Serial.println("File too small"); return; }

    uint8_t nameLen = fileData[0x10];
    char appName[9] = {0};
    memcpy(appName, &fileData[0x11], min((int)nameLen, 8));

    uint32_t hexLen = fileData[0x4A]
                    | ((uint32_t)fileData[0x4B] << 8)
                    | ((uint32_t)fileData[0x4C] << 16)
                    | ((uint32_t)fileData[0x4D] << 24);

    Serial.print("App name: ");      Serial.println(appName);
    Serial.print("Hex payload len: "); Serial.println(hexLen);

    static FlashPage fp;
    if (!parseFlashPage(&fileData[0x4E], hexLen, &fp)) {
        Serial.println("Parse failed"); return;
    }

    Serial.print("Flash page: ");  Serial.println(fp.page);
    Serial.print("Flash addr: 0x"); Serial.println(fp.addr, HEX);
    Serial.print("Flag: 0x");      Serial.println(fp.flag,  HEX);
    Serial.print("Data size: ");   Serial.println(fp.size);
    Serial.print("First 16 bytes: ");
    for (int b = 0; b < 16; b++) {
        if (fp.data[b] < 0x10) Serial.print("0");
        Serial.print(fp.data[b], HEX);
    }
    Serial.println();
}

// -----------------------------------------------------------------------
// Send one 128-byte chunk — matches libticalcs calc_73.cc send_flash loop:
//   SEND_VAR2 -> RECV_ACK -> RECV_CTS(10) -> SEND_ACK -> SEND_XDP -> RECV_ACK
// No resetLines() — libticalcs doesn't use it here
// -----------------------------------------------------------------------
static int sendChunk(uint8_t* data, uint16_t chunkAddr, uint16_t flashPage, uint8_t flag) {
    int dataLength = 0;

    // --- VAR packet: 23 06 0A 00 + 10-byte flash header ---
    uint8_t msgHdr[4] = {COMP83P, VAR, 10, 0};
    uint8_t varHdr[10];
    varHdr[0] = APP_CHUNK_SIZE & 0xFF;        // size lo
    varHdr[1] = (APP_CHUNK_SIZE >> 8) & 0xFF; // size hi
    varHdr[2] = FLASH_APP_TYPE;               // 0x24
    varHdr[3] = APP_CHUNK_SIZE & 0xFF;        // size lo (repeated)
    varHdr[4] = (APP_CHUNK_SIZE >> 8) & 0xFF; // size hi (repeated)
    varHdr[5] = flag;                          // 0x80 for apps
    varHdr[6] = chunkAddr & 0xFF;             // flash addr lo
    varHdr[7] = (chunkAddr >> 8) & 0xFF;      // flash addr hi
    varHdr[8] = flashPage & 0xFF;             // page lo
    varHdr[9] = (flashPage >> 8) & 0xFF;      // page hi

    if (cbl.send(msgHdr, varHdr, 10)) {
        Serial.println("VAR send failed");
        return -1;
    }

    // --- RECV_ACK ---
    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after VAR, got: 0x");
        Serial.println(msgHdr[1], HEX);
        return -1;
    }

    // --- RECV_CTS — calc sends "73 09 0A 00" + 10 bytes echoed header ---
    // Must provide a real buffer so CBL2 absorbs those 10 bytes
    uint8_t ctsBuf[16];
    dataLength = 0;
    if (cbl.get(msgHdr, ctsBuf, &dataLength, 0) || msgHdr[1] != CTS) {
        Serial.print("Expected CTS, got: 0x");
        Serial.println(msgHdr[1], HEX);
        Serial.print("CTS dataLength: ");
        Serial.println(dataLength);
        return -1;
    }

    // --- SEND_ACK ---
    msgHdr[1] = ACK; msgHdr[2] = 0; msgHdr[3] = 0;
    cbl.send(msgHdr, NULL, 0);

    // --- SEND_XDP (DATA) — 23 15 80 00 + 128 bytes + checksum ---
    msgHdr[1] = DATA;
    msgHdr[2] = APP_CHUNK_SIZE & 0xFF;
    msgHdr[3] = (APP_CHUNK_SIZE >> 8) & 0xFF;
    if (cbl.send(msgHdr, data, APP_CHUNK_SIZE)) {
        Serial.println("XDP send failed");
        return -1;
    }

    // --- RECV_ACK ---
    dataLength = 0;
    if (cbl.get(msgHdr, NULL, &dataLength, 0) || msgHdr[1] != ACK) {
        Serial.print("Expected ACK after XDP, got: 0x");
        Serial.println(msgHdr[1], HEX);
        return -1;
    }

    return 0;
}

// -----------------------------------------------------------------------
// Main entry point
// -----------------------------------------------------------------------
int tiSendApp(uint8_t* fileData, int fileLen) {
    if (fileLen < 0x4E + 2) {
        Serial.println("File too small");
        return -1;
    }

    uint32_t hexLen = fileData[0x4A]
                    | ((uint32_t)fileData[0x4B] << 8)
                    | ((uint32_t)fileData[0x4C] << 16)
                    | ((uint32_t)fileData[0x4D] << 24);

    static FlashPage fp;
    if (!parseFlashPage(&fileData[0x4E], hexLen, &fp)) {
        Serial.println("Failed to parse Intel HEX");
        return -1;
    }

    Serial.print("Sending app: page=");
    Serial.print(fp.page);
    Serial.print(" addr=0x");
    Serial.print(fp.addr, HEX);
    Serial.print(" flag=0x");
    Serial.print(fp.flag, HEX);
    Serial.print(" size=");
    Serial.println(fp.size);

    // Send 128-byte chunks across the full 16KB page
    // addr increments by APP_CHUNK_SIZE each iteration, matching calc_73.cc:
    //   addr = fp->addr + j  where j += size (0x80)
    int numChunks = 0x4000 / APP_CHUNK_SIZE; // 128 chunks
    for (int chunk = 0; chunk < numChunks; chunk++) {
        uint16_t chunkAddr = fp.addr + (chunk * APP_CHUNK_SIZE);

        Serial.print("Chunk ");
        Serial.print(chunk + 1);
        Serial.print("/128 addr=0x");
        Serial.println(chunkAddr, HEX);

        int result = sendChunk(&fp.data[chunk * APP_CHUNK_SIZE], chunkAddr, fp.page, fp.flag);
        if (result != 0) {
            Serial.print("Failed at chunk ");
            Serial.println(chunk);
            return result;
        }
    }

    // --- SEND_EOT -> RECV_ACK ---
    uint8_t msgHdr[4] = {COMP83P, EOT, 0, 0};
    cbl.send(msgHdr, NULL, 0);
    int dataLength = 0;
    cbl.get(msgHdr, NULL, &dataLength, 0);

    Serial.println("App transfer complete.");
    return 0;
}