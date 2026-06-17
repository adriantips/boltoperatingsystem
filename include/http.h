#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Dead-simple HTTP/1.0 GET. Plain TCP for http://, TLS 1.2 for https:// (see
 *  net/tls.c -- certificate NOT verified). No chunked transfer: we ask for
 *  Connection: close and read to EOF. Enough to pull a basic HTML page or text
 *  file into a buffer.
 * ===========================================================================*/

/* Fetch URL ("http[s]://host[:port]/path" or "host/path") into out (NUL-term,
 * truncated to cap-1). Returns body length on success, -1 on failure.
 *  *status   <- HTTP status code (e.g. 200, 404), if non-NULL. On failure it is
 *               a negative reason: -3 DNS, -4 TCP connect, -5 TLS handshake.
 *  location  <- value of a redirect's Location header (3xx), if non-NULL. */
int http_get(const char *url, char *out, uint32_t cap,
             int *status, char *location, uint32_t loc_cap);
