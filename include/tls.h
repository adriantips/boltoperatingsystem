#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Minimal TLS 1.2 client over the BoltOS TCP stack. One cipher family:
 *  ECDHE (X25519) + AES-128-GCM + SHA-256, matching what virtually every public
 *  HTTPS server still offers for TLS 1.2. The server certificate is parsed past
 *  but NOT verified -- there is no clock or trust store -- so this protects
 *  against passive eavesdropping only, not active MITM. Good enough to fetch a
 *  page; do not type secrets into it. Blocking and poll-driven, like tcp_*.
 * ===========================================================================*/

struct tls_conn;

/* Open a TLS session to dst_ip:port. `sni` is the hostname sent in the SNI
 * extension (required by most CDNs). Returns a connection or 0 on failure. */
struct tls_conn *tls_connect(uint32_t dst_ip, uint16_t port,
                             const char *sni, uint32_t timeout_ms);

int  tls_send(struct tls_conn *c, const void *data, uint32_t len);
/* Returns bytes read (>0), 0 on clean close, -1 on error/timeout. */
int  tls_recv(struct tls_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms);
void tls_close(struct tls_conn *c);
