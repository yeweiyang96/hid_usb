#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H
#include "hid_bridge.h"

/* Never returns. Must initialize, maintain and deinitialize CYW43 on Core 1. */
void wifi_service_run(HidBridge *bridge, bool watchdog_boot);
#endif
