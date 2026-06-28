#pragma once

#include <Arduino.h>
#include "CBL2.h"
#include "TIVar.h"
#include "config.h"

extern CBL2 cbl;
extern uint8_t tiHeader[MAXHDRLEN];
extern uint8_t tiData[MAXDATALEN];

void tiSetup();
void tiTick();
void tiQueueReal(char varName, double value);
void tiQueueString(char strIndex, const char* text);
void tiClearStringQueue();
int tiSendProgram(uint8_t* fileData, int fileLen);
void tiSendExamExit();

int onReceived(uint8_t type, enum Endpoint model, int datalen);
int onRequest(uint8_t type, enum Endpoint model, int* headerlen, int* datalen, data_callback* cb);