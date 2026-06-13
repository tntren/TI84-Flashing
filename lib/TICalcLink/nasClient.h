#pragma once

#include <Arduino.h>

bool nasConnect();
void nasDisconnect();
bool nasListFiles(char* outBuffer, int maxLen);
bool nasDownloadFile(const char* filename, uint8_t* outBuffer, int* outLen, int maxLen);
bool nasIsConnected();