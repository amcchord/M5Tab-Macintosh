/*
 * automation.h - host-attached closed-loop control for the emulator
 *
 * A small, line-oriented protocol runs over the same USB CDC/UART serial
 * connection used by the diagnostic console. Commands are namespaced with
 * "@B2 " so normal log output can coexist with the automation client.
 */
#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool AutomationInit(void);
void AutomationExit(void);
void AutomationPrepareNetwork(void);
bool AutomationSerialCaptureActive(void);

#ifdef __cplusplus
}
#endif

#endif
