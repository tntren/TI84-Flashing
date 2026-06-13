#pragma once

#include <Arduino.h>

int parseIntelHexPage(const uint8_t* hexData, uint32_t hexLen, uint8_t* pageBuf);
void tiDebugParseApp(uint8_t* fileData, int fileLen);
int tiSendApp(uint8_t* fileData, int fileLen);