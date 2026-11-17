#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Enregistre GET/POST /api/sensorfusion. */
void web_sensorfusion_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
