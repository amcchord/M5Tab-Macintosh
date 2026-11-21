#pragma once
#include <Arduino.h>

// These transfer methods belong to the preboot UI task.
String FirmwareUpdateStart(char *arguments);
String FirmwareUpdateStatus();
String FirmwareUpdateCancel();
bool FirmwareUpdateBusy();
bool FirmwareUpdateReady();
void FirmwareUpdatePoll();
void FirmwareUpdateHealth(bool passed);
