#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the DNS responder on port 53.
 * All A queries are answered with 192.168.4.1 (captive portal).
 * Only used in AP mode.
 */
void dns_server_start(void);

/**
 * Stop the DNS responder.
 */
void dns_server_stop(void);

#ifdef __cplusplus
}
#endif
