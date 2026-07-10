#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start HTTP server and DNS responder (captive portal for AP mode).
 */
void web_server_start(void);

/**
 * Stop HTTP server and DNS responder.
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif
