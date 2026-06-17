#include <stdint.h>
#include "tls.h"
#include "crypto.h"
#include "net.h"          /* tcp_connect/tcp_send/tcp_recv/tcp_close */
#include "kheap.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  TLS 1.2 client. Single cipher family: ECDHE-X25519 + AES-128-GCM + SHA-256
 *  (cipher suites 0xC02B / 0xC02F). The handshake follows RFC 5246 (+ RFC 5288
 *  for the AEAD record format and RFC 7748 for the key exchange). The server
 *  Certificate is consumed but NOT validated -- see tls.h. Layered straight on
 *  top of tcp_* and driven by their blocking, poll-pumped I/O.
 * ===========================================================================*/

#define TLS_DEBUG 0
#if TLS_DEBUG
#  define DBG(...) kprintf(__VA_ARGS__)
#else
#  define DBG(...) ((void)0)
#endif

/* record content types */
#define REC_CCS    20
#define REC_ALERT  21
#define REC_HS     22
#define REC_APP    23
/* handshake message types */
#define HS_CLIENT_HELLO  1
#define HS_SERVER_HELLO  2
#define HS_CERTIFICATE   11
#define HS_SKE           12
#define HS_CERT_REQ      13
#define HS_SHD           14
#define HS_CKE           16
#define HS_FINISHED      20
#define HS_NEW_TICKET    4

#define INBUF   18432
#define RECBUF  16700
#define HSBUF   18432
#define OUTBUF  16700

struct tls_conn {
    struct tcp_conn *tcp;
    int          established;

    uint8_t  cw_key[16], sw_key[16], cw_iv[4], sw_iv[4];
    uint64_t wseq, rseq;
    int      tx_secure, rx_secure;

    uint8_t  client_random[32], server_random[32];
    uint8_t  master[48];
    uint8_t  kx_priv[32], kx_pub[32];

    sha256_ctx hs_hash;          /* running handshake transcript */

    uint8_t  in[INBUF];   uint32_t in_len;          /* raw TCP bytes              */
    uint8_t  rec[RECBUF];                           /* one decrypted record       */
    uint8_t  hs[HSBUF];   uint32_t hs_len, hs_off;  /* handshake reassembly       */
    uint8_t  out[OUTBUF];                           /* outgoing record scratch    */
    uint32_t app_off, app_len;                      /* leftover app data in rec[] */
};

/* ----------------------------- RNG -------------------------------------- */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t rng_s;
static void rng_mix(void) {
    rng_s ^= rdtsc() ^ ((uint64_t)pit_ticks() << 21) ^ 0x9E3779B97F4A7C15ull;
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
}
static void rng_bytes(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { rng_mix(); p[i] = (uint8_t)(rng_s >> 24); }
}

/* ----------------------------- helpers ---------------------------------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void put_seq(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8*i)); }

/* TLS 1.2 PRF (P_SHA256). out gets outlen bytes. */
static void prf(const uint8_t *sec, uint32_t slen, const char *label,
                const uint8_t *seed, uint32_t seedlen, uint8_t *out, uint32_t outlen) {
    uint8_t ls[128]; uint32_t llen = (uint32_t)strlen(label);
    memcpy(ls, label, llen); memcpy(ls + llen, seed, seedlen);
    uint32_t lslen = llen + seedlen;
    uint8_t a[32]; hmac_sha256(sec, slen, ls, lslen, a);    /* A(1) */
    uint32_t done = 0;
    while (done < outlen) {
        uint8_t in[32 + 128]; memcpy(in, a, 32); memcpy(in + 32, ls, lslen);
        uint8_t blk[32]; hmac_sha256(sec, slen, in, 32 + lslen, blk);
        uint32_t n = outlen - done; if (n > 32) n = 32;
        memcpy(out + done, blk, n); done += n;
        hmac_sha256(sec, slen, a, 32, a);                   /* A(i+1) */
    }
}

static void transcript_add(struct tls_conn *c, const uint8_t *body, uint32_t blen) {
    sha256_update(&c->hs_hash, body - 4, 4 + blen);         /* body-4 = msg header */
}

/* ------------------------ raw TCP input buffering ----------------------- */
static int in_fill(struct tls_conn *c, uint32_t need, uint32_t timeout) {
    while (c->in_len < need) {
        if (c->in_len >= INBUF) return -1;
        int r = tcp_recv(c->tcp, c->in + c->in_len, INBUF - c->in_len, timeout);
        if (r <= 0) return r;                  /* 0 = peer closed, -1 = timeout */
        c->in_len += (uint32_t)r;
    }
    return 1;
}
static void in_consume(struct tls_conn *c, uint32_t n) {
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

/* Read one TLS record. Decrypts if rx is secure. out must hold >= 16384 bytes.
 * Returns 1 ok, 0 on clean close, -1 on error. */
static int record_read(struct tls_conn *c, uint8_t *type, uint8_t *out,
                       uint32_t *outlen, uint32_t timeout) {
    int r = in_fill(c, 5, timeout);
    if (r <= 0) return r;
    uint8_t t = c->in[0];
    uint32_t len = ((uint32_t)c->in[3] << 8) | c->in[4];
    if (len > 16384 + 2048) return -1;
    r = in_fill(c, 5 + len, timeout);
    if (r <= 0) return -1;                     /* closed mid-record = error */

    uint8_t *p = c->in + 5;
    if (c->rx_secure && (t == REC_HS || t == REC_APP || t == REC_ALERT)) {
        if (len < 8 + 16) return -1;
        uint32_t ctlen = len - 8 - 16;
        uint8_t nonce[12]; memcpy(nonce, c->sw_iv, 4); memcpy(nonce + 4, p, 8);
        uint8_t aad[13];
        put_seq(aad, c->rseq); aad[8] = t; aad[9] = 3; aad[10] = 3; put16(aad + 11, (uint16_t)ctlen);
        if (aes_gcm_open(c->sw_key, nonce, aad, 13, p + 8, ctlen, p + 8 + ctlen, out) != 0) {
            DBG("tls: bad GCM tag\n"); return -1;
        }
        *outlen = ctlen; c->rseq++;
    } else {
        memcpy(out, p, len);
        *outlen = len;
    }
    *type = t;
    in_consume(c, 5 + len);
    return 1;
}

/* Send a plaintext record (handshake / CCS before the cipher is active). */
static int record_send_plain(struct tls_conn *c, uint8_t type, uint8_t minor,
                             const uint8_t *data, uint32_t len) {
    c->out[0] = type; c->out[1] = 3; c->out[2] = minor; put16(c->out + 3, (uint16_t)len);
    memcpy(c->out + 5, data, len);
    return tcp_send(c->tcp, c->out, 5 + len);
}

/* Send an encrypted record (AES-GCM, explicit nonce = write sequence number). */
static int record_send_enc(struct tls_conn *c, uint8_t type,
                           const uint8_t *data, uint32_t len) {
    uint8_t *p = c->out + 5;
    put_seq(p, c->wseq);                       /* explicit nonce */
    uint8_t nonce[12]; memcpy(nonce, c->cw_iv, 4); memcpy(nonce + 4, p, 8);
    uint8_t aad[13];
    put_seq(aad, c->wseq); aad[8] = type; aad[9] = 3; aad[10] = 3; put16(aad + 11, (uint16_t)len);
    aes_gcm_seal(c->cw_key, nonce, aad, 13, data, len, p + 8, p + 8 + len);
    uint32_t rlen = 8 + len + 16;
    c->out[0] = type; c->out[1] = 3; c->out[2] = 3; put16(c->out + 3, (uint16_t)rlen);
    c->wseq++;
    return tcp_send(c->tcp, c->out, 5 + rlen);
}

static int record_send(struct tls_conn *c, uint8_t type, const uint8_t *data, uint32_t len) {
    return c->tx_secure ? record_send_enc(c, type, data, len)
                        : record_send_plain(c, type, 3, data, len);
}

/* Pull the next handshake message during the (plaintext) server-hello flight.
 * Returns 1 ok, 0 closed, -1 error, -2 alert, -3 a non-handshake record arrived
 * (its type is left in c->rec[0..]). Does NOT touch the transcript. */
static int hs_next(struct tls_conn *c, uint8_t *mtype, uint8_t **body,
                   uint32_t *blen, uint32_t timeout) {
    for (;;) {
        if (c->hs_len - c->hs_off >= 4) {
            uint8_t *m = c->hs + c->hs_off;
            uint32_t ml = ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
            if (c->hs_len - c->hs_off >= 4 + ml) {
                *mtype = m[0]; *body = m + 4; *blen = ml;
                c->hs_off += 4 + ml;
                return 1;
            }
        }
        if (c->hs_off) { memmove(c->hs, c->hs + c->hs_off, c->hs_len - c->hs_off);
                         c->hs_len -= c->hs_off; c->hs_off = 0; }
        uint8_t t; uint32_t rl;
        int r = record_read(c, &t, c->rec, &rl, timeout);
        if (r <= 0) return r;
        if (t == REC_ALERT) return -2;
        if (t != REC_HS) return -3;
        if (c->hs_len + rl > HSBUF) return -1;
        memcpy(c->hs + c->hs_len, c->rec, rl);
        c->hs_len += rl;
    }
}

/* --------------------------- ClientHello -------------------------------- */
static uint32_t build_client_hello(struct tls_conn *c, const char *sni, uint8_t *buf) {
    uint32_t p = 0;
    buf[p++] = 3; buf[p++] = 3;                         /* client_version 1.2 */
    memcpy(buf + p, c->client_random, 32); p += 32;
    buf[p++] = 0;                                       /* session_id len = 0 */
    put16(buf + p, 4); p += 2;                          /* cipher suites: 2    */
    put16(buf + p, 0xC02B); p += 2;                     /* ECDHE_ECDSA_AES128_GCM */
    put16(buf + p, 0xC02F); p += 2;                     /* ECDHE_RSA_AES128_GCM   */
    buf[p++] = 1; buf[p++] = 0;                         /* compression: null  */

    uint32_t ext_len_at = p; p += 2;                    /* extensions length  */
    uint32_t ext0 = p;

    /* server_name (SNI) */
    uint32_t hlen = (uint32_t)strlen(sni);
    put16(buf + p, 0x0000); p += 2;
    put16(buf + p, (uint16_t)(5 + hlen)); p += 2;
    put16(buf + p, (uint16_t)(3 + hlen)); p += 2;       /* server_name_list len */
    buf[p++] = 0;                                       /* host_name type      */
    put16(buf + p, (uint16_t)hlen); p += 2;
    memcpy(buf + p, sni, hlen); p += hlen;

    /* supported_groups: x25519 only */
    put16(buf + p, 0x000a); p += 2; put16(buf + p, 4); p += 2;
    put16(buf + p, 2); p += 2; put16(buf + p, 0x001d); p += 2;

    /* ec_point_formats: uncompressed */
    put16(buf + p, 0x000b); p += 2; put16(buf + p, 2); p += 2;
    buf[p++] = 1; buf[p++] = 0;

    /* signature_algorithms */
    static const uint16_t sa[] = { 0x0403,0x0804,0x0401,0x0503,0x0805,0x0501,0x0806,0x0601,0x0201 };
    uint32_t sn = sizeof(sa) / sizeof(sa[0]);
    put16(buf + p, 0x000d); p += 2; put16(buf + p, (uint16_t)(2 + 2*sn)); p += 2;
    put16(buf + p, (uint16_t)(2*sn)); p += 2;
    for (uint32_t i = 0; i < sn; i++) { put16(buf + p, sa[i]); p += 2; }

    /* renegotiation_info (empty) */
    put16(buf + p, 0xff01); p += 2; put16(buf + p, 1); p += 2; buf[p++] = 0;

    put16(buf + ext_len_at, (uint16_t)(p - ext0));
    return p;
}

/* ----------------------------- handshake -------------------------------- */
static int do_handshake(struct tls_conn *c, const char *sni, uint32_t timeout) {
    uint8_t msg[800];

    sha256_init(&c->hs_hash);
    rng_bytes(c->client_random, 32);

    /* ClientHello */
    uint32_t blen = build_client_hello(c, sni, msg + 4);
    msg[0] = HS_CLIENT_HELLO; msg[1] = 0; put16(msg + 2, (uint16_t)blen);
    transcript_add(c, msg + 4, blen);
    if (record_send_plain(c, REC_HS, 1, msg, 4 + blen) < 0) return -1;

    /* ephemeral X25519 keypair */
    rng_bytes(c->kx_priv, 32);
    x25519(c->kx_pub, c->kx_priv, x25519_basepoint);

    /* ---- server flight: ServerHello, Certificate, ServerKeyExchange, Done -- */
    uint8_t server_pub[32]; int have_pub = 0, got_sh = 0, done = 0;
    while (!done) {
        uint8_t mt; uint8_t *b; uint32_t bl;
        int r = hs_next(c, &mt, &b, &bl, timeout);
        if (r != 1) { DBG("tls: hs_next=%d\n", r); return -1; }
        transcript_add(c, b, bl);
        switch (mt) {
        case HS_SERVER_HELLO: {
            if (bl < 38) return -1;
            memcpy(c->server_random, b + 2, 32);
            uint32_t o = 34; uint8_t sid = b[o++]; o += sid;
            if (o + 3 > bl) return -1;
            uint16_t suite = ((uint16_t)b[o] << 8) | b[o+1];
            if (suite != 0xC02B && suite != 0xC02F) { DBG("tls: bad suite %x\n", suite); return -1; }
            got_sh = 1;
            break;
        }
        case HS_CERTIFICATE:
            break;                                  /* not verified */
        case HS_SKE: {
            if (bl < 4) return -1;
            if (b[0] != 3) return -1;               /* named_curve */
            uint16_t curve = ((uint16_t)b[1] << 8) | b[2];
            if (curve != 0x001d) { DBG("tls: curve %x not x25519\n", curve); return -1; }
            uint8_t pl = b[3];
            if (pl != 32 || 4u + pl > bl) return -1;
            memcpy(server_pub, b + 4, 32);
            have_pub = 1;
            break;                                  /* signature deliberately skipped */
        }
        case HS_CERT_REQ:
            break;                                  /* ignored: we send no client cert */
        case HS_SHD:
            done = 1;
            break;
        default:
            break;
        }
    }
    if (!got_sh || !have_pub) return -1;

    /* ---- derive keys ---- */
    uint8_t pms[32];
    x25519(pms, c->kx_priv, server_pub);

    uint8_t seed[64];
    memcpy(seed, c->client_random, 32); memcpy(seed + 32, c->server_random, 32);
    prf(pms, 32, "master secret", seed, 64, c->master, 48);

    memcpy(seed, c->server_random, 32); memcpy(seed + 32, c->client_random, 32);
    uint8_t kb[40];
    prf(c->master, 48, "key expansion", seed, 64, kb, 40);
    memcpy(c->cw_key, kb,      16);
    memcpy(c->sw_key, kb + 16, 16);
    memcpy(c->cw_iv,  kb + 32, 4);
    memcpy(c->sw_iv,  kb + 36, 4);

    /* ---- ClientKeyExchange ---- */
    msg[0] = HS_CKE; msg[1] = 0; put16(msg + 2, 33);
    msg[4] = 32; memcpy(msg + 5, c->kx_pub, 32);
    transcript_add(c, msg + 4, 33);
    if (record_send_plain(c, REC_HS, 3, msg, 4 + 33) < 0) return -1;

    /* ---- ChangeCipherSpec, then encrypted client Finished ---- */
    uint8_t one = 1;
    if (record_send_plain(c, REC_CCS, 3, &one, 1) < 0) return -1;
    c->tx_secure = 1; c->wseq = 0;

    uint8_t th[32]; sha256_ctx snap = c->hs_hash; sha256_final(&snap, th);
    uint8_t vd[12];
    prf(c->master, 48, "client finished", th, 32, vd, 12);
    msg[0] = HS_FINISHED; msg[1] = 0; put16(msg + 2, 12);
    memcpy(msg + 4, vd, 12);
    transcript_add(c, msg + 4, 12);                 /* covered by server Finished */
    if (record_send(c, REC_HS, msg, 4 + 12) < 0) return -1;

    /* ---- server flight 2: [NewSessionTicket] CCS Finished ---- */
    int got_fin = 0;
    while (!got_fin) {
        uint8_t t; uint32_t rl;
        int r = record_read(c, &t, c->rec, &rl, timeout);
        if (r != 1) { DBG("tls: flight2 read=%d\n", r); return -1; }
        if (t == REC_CCS) { c->rx_secure = 1; c->rseq = 0; continue; }
        if (t == REC_ALERT) { DBG("tls: server alert\n"); return -1; }
        if (t != REC_HS) continue;
        /* parse handshake messages in this record */
        uint32_t o = 0;
        while (o + 4 <= rl) {
            uint8_t mt = c->rec[o];
            uint32_t ml = ((uint32_t)c->rec[o+1] << 16) | ((uint32_t)c->rec[o+2] << 8) | c->rec[o+3];
            if (o + 4 + ml > rl) break;
            uint8_t *b = c->rec + o + 4;
            if (mt == HS_NEW_TICKET) {
                transcript_add(c, b, ml);           /* precedes server Finished */
            } else if (mt == HS_FINISHED) {
                uint8_t exp[12]; sha256_ctx s2 = c->hs_hash; uint8_t h2[32];
                sha256_final(&s2, h2);
                prf(c->master, 48, "server finished", h2, 32, exp, 12);
                if (ml != 12 || memcmp(exp, b, 12) != 0) { DBG("tls: server Finished mismatch\n"); return -1; }
                got_fin = 1;
                break;
            }
            o += 4 + ml;
        }
    }

    c->established = 1;
    return 0;
}

/* ------------------------------ public API ------------------------------ */
struct tls_conn *tls_connect(uint32_t dst_ip, uint16_t port,
                             const char *sni, uint32_t timeout_ms) {
    rng_mix();
    struct tls_conn *c = (struct tls_conn *)kmalloc(sizeof(*c));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));

    c->tcp = tcp_connect(dst_ip, port, timeout_ms);
    if (!c->tcp) { kfree(c); return 0; }

    if (do_handshake(c, sni, timeout_ms ? timeout_ms : 8000) != 0) {
        tcp_close(c->tcp);
        kfree(c);
        return 0;
    }
    return c;
}

int tls_send(struct tls_conn *c, const void *data, uint32_t len) {
    if (!c || !c->established) return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off; if (chunk > 16384) chunk = 16384;
        if (record_send(c, REC_APP, p + off, chunk) < 0) return -1;
        off += chunk;
    }
    return (int)len;
}

int tls_recv(struct tls_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms) {
    if (!c || !c->established) return -1;
    if (c->app_off >= c->app_len) {
        for (;;) {
            uint8_t t; uint32_t rl;
            int r = record_read(c, &t, c->rec, &rl, timeout_ms);
            if (r <= 0) return r;                   /* 0 = closed, -1 = error */
            if (t == REC_APP)   { c->app_off = 0; c->app_len = rl; break; }
            if (t == REC_ALERT) return 0;           /* treat any alert as EOF */
            if (t == REC_HS)    continue;           /* post-handshake ticket, ignore */
        }
    }
    uint32_t n = c->app_len - c->app_off;
    if (n > cap) n = cap;
    memcpy(buf, c->rec + c->app_off, n);
    c->app_off += n;
    return (int)n;
}

void tls_close(struct tls_conn *c) {
    if (!c) return;
    if (c->established && c->tx_secure) {
        uint8_t alert[2] = { 1, 0 };               /* warning, close_notify */
        record_send(c, REC_ALERT, alert, 2);
    }
    if (c->tcp) tcp_close(c->tcp);
    kfree(c);
}
