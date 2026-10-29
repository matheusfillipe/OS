#include "http.h"
#include "json.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#define CA_BUNDLE_BUILTIN "/usr/share/os/ca-certificates.crt"

static struct {
    bool ready;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
} tls;

struct conn {
    int fd;
    int timeout_ms;
    bool use_tls;
    mbedtls_ssl_context ssl;
};

static int tls_global_init(char *err, size_t errlen) {
    if (tls.ready)
        return 0;
    if (psa_crypto_init() != PSA_SUCCESS) {
        snprintf(err, errlen, "crypto library failed to start");
        return -1;
    }
    mbedtls_entropy_init(&tls.entropy);
    mbedtls_ctr_drbg_init(&tls.drbg);
    mbedtls_x509_crt_init(&tls.ca);
    mbedtls_ssl_config_init(&tls.conf);
    const char *pers = "os-http";
    if (mbedtls_ctr_drbg_seed(&tls.drbg, mbedtls_entropy_func, &tls.entropy, (const unsigned char *)pers,
                              strlen(pers)) != 0) {
        snprintf(err, errlen, "could not seed the random generator");
        return -1;
    }
    int r = mbedtls_x509_crt_parse_file(&tls.ca, CA_BUNDLE);
    if (r < 0)
        r = mbedtls_x509_crt_parse_file(&tls.ca, CA_BUNDLE_BUILTIN);
    if (r < 0) {
        snprintf(err, errlen, "could not load trusted certificates (%s)", CA_BUNDLE);
        return -1;
    }
    mbedtls_ssl_config_defaults(&tls.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&tls.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls.conf, &tls.ca, NULL);
    mbedtls_ssl_conf_rng(&tls.conf, mbedtls_ctr_drbg_random, &tls.drbg);
    tls.ready = true;
    return 0;
}

static int wait_fd(int fd, short events, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = events};
    int r = poll(&p, 1, timeout_ms);
    return r > 0 ? 0 : -1;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    struct conn *c = ctx;
    ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (n < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    struct conn *c = ctx;
    if (wait_fd(c->fd, POLLIN, c->timeout_ms) < 0)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t n = recv(c->fd, buf, len, 0);
    if (n < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)n;
}

static int conn_write(struct conn *c, const char *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        int n;
        if (c->use_tls) {
            n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + done, len - done);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
        } else {
            n = (int)send(c->fd, data + done, len - done, MSG_NOSIGNAL);
        }
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Returns bytes read, 0 at end of stream, negative on error/timeout. */
static int conn_read(struct conn *c, char *buf, size_t len) {
    if (!c->use_tls) {
        if (wait_fd(c->fd, POLLIN, c->timeout_ms) < 0)
            return -2;
        return (int)recv(c->fd, buf, len, 0);
    }
    for (;;) {
        int n = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (n == MBEDTLS_ERR_SSL_TIMEOUT)
            return -2;
        return n;
    }
}

static int parse_url(const char *url, bool *tls_out, char *host, size_t hostlen, char *port, size_t portlen,
                     const char **path) {
    const char *p;
    if (!strncmp(url, "https://", 8)) {
        *tls_out = true;
        p = url + 8;
        snprintf(port, portlen, "443");
    } else if (!strncmp(url, "http://", 7)) {
        *tls_out = false;
        p = url + 7;
        snprintf(port, portlen, "80");
    } else {
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (!hl || hl >= hostlen)
        return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (colon) {
        size_t pl = (size_t)(end - colon - 1);
        if (!pl || pl >= portlen)
            return -1;
        memcpy(port, colon + 1, pl);
        port[pl] = 0;
    }
    *path = slash ? slash : "/";
    return 0;
}

static int dial(const char *host, const char *port, int timeout_ms, char *err, size_t errlen) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    int g = getaddrinfo(host, port, &hints, &res);
    if (g != 0) {
        snprintf(err, errlen, "could not find %s (%s)", host, gai_strerror(g));
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        snprintf(err, errlen, "could not connect to %s:%s (%s)", host, port, strerror(errno));
        if (fd >= 0)
            close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    (void)timeout_ms;
    return fd;
}

static char *find_header(char *headers, const char *name) {
    size_t nl = strlen(name);
    for (char *line = headers; line && *line;) {
        char *eol = strstr(line, "\r\n");
        if (!strncasecmp(line, name, nl) && line[nl] == ':') {
            char *v = line + nl + 1;
            while (*v == ' ')
                v++;
            return v;
        }
        line = eol ? eol + 2 : NULL;
    }
    return NULL;
}

/* Decodes a chunked body in place; returns the decoded length. */
static size_t dechunk(char *body, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        size_t chunk = strtoul(body + in, NULL, 16);
        char *eol = strstr(body + in, "\r\n");
        if (!eol)
            break;
        in = (size_t)(eol - body) + 2;
        if (chunk == 0 || in + chunk > len)
            break;
        memmove(body + out, body + in, chunk);
        out += chunk;
        in += chunk + 2;
    }
    body[out] = 0;
    return out;
}

/* Decodes a chunked body as it arrives, handing each piece of data on. */
struct dechunker {
    int state; /* 0: chunk size line, 1: chunk data, 2: the CRLF after it, 3: done */
    size_t remain;
    char line[24];
    int ll;
};

static void dechunk_feed(struct dechunker *d, const char *p, size_t n, http_body_fn fn, void *ctx) {
    size_t i = 0;
    while (i < n && d->state != 3) {
        if (d->state == 1) {
            size_t take = n - i < d->remain ? n - i : d->remain;
            fn(p + i, take, ctx);
            i += take;
            d->remain -= take;
            if (!d->remain)
                d->state = 2;
            continue;
        }
        char c = p[i++];
        if (d->state == 0) {
            if (c == '\n') {
                d->line[d->ll] = 0;
                d->remain = strtoul(d->line, NULL, 16);
                d->ll = 0;
                d->state = d->remain ? 1 : 3;
            } else if (c != '\r' && d->ll < (int)sizeof d->line - 1) {
                d->line[d->ll++] = c;
            }
        } else if (d->state == 2 && c == '\n') {
            d->state = 0;
        }
    }
}

static int do_request(const char *method, const char *url, const char *const *headers, const char *body,
                      size_t body_len, int timeout_s, http_body_fn on_body, void *ctx, struct http_response *out);

int http_request(const char *method, const char *url, const char *const *headers, const char *body,
                 size_t body_len, int timeout_s, struct http_response *out) {
    return do_request(method, url, headers, body, body_len, timeout_s, NULL, NULL, out);
}

int http_request_stream(const char *method, const char *url, const char *const *headers, const char *body,
                        size_t body_len, int timeout_s, http_body_fn on_body, void *ctx, struct http_response *out) {
    return do_request(method, url, headers, body, body_len, timeout_s, on_body, ctx, out);
}

static int do_request(const char *method, const char *url, const char *const *headers, const char *body,
                      size_t body_len, int timeout_s, http_body_fn on_body, void *ctx, struct http_response *out) {
    memset(out, 0, sizeof *out);
    char host[256], port[8];
    const char *path;
    struct conn c = {.fd = -1, .timeout_ms = (timeout_s > 0 ? timeout_s : 60) * 1000};
    if (parse_url(url, &c.use_tls, host, sizeof host, port, sizeof port, &path) < 0) {
        snprintf(out->error, sizeof out->error, "not a valid web address: %s", url);
        return -1;
    }
    if (c.use_tls && tls_global_init(out->error, sizeof out->error) < 0)
        return -1;
    c.fd = dial(host, port, c.timeout_ms, out->error, sizeof out->error);
    if (c.fd < 0)
        return -1;

    int ret = -1;
    sbuf req, resp;
    sb_init(&req);
    sb_init(&resp);
    if (c.use_tls) {
        mbedtls_ssl_init(&c.ssl);
        mbedtls_ssl_setup(&c.ssl, &tls.conf);
        mbedtls_ssl_set_hostname(&c.ssl, host);
        mbedtls_ssl_set_bio(&c.ssl, &c, bio_send, bio_recv, NULL);
        int r;
        while ((r = mbedtls_ssl_handshake(&c.ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
            if (flags && flags != (uint32_t)-1) {
                char why[160];
                mbedtls_x509_crt_verify_info(why, sizeof why, "", flags);
                char *nl = strchr(why, '\n');
                if (nl)
                    *nl = 0;
                snprintf(out->error, sizeof out->error, "%s's certificate was not trusted: %s", host, why);
            } else {
                char why[120];
                mbedtls_strerror(r, why, sizeof why);
                snprintf(out->error, sizeof out->error, "secure connection to %s failed: %s", host, why);
            }
            goto done;
        }
    }

    bool default_port = (c.use_tls && !strcmp(port, "443")) || (!c.use_tls && !strcmp(port, "80"));
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: OperatingSystem/0.4\r\nAccept: */*\r\n"
                    "Connection: close\r\n",
              method, path, host, default_port ? "" : ":", default_port ? "" : port);
    if (body)
        sb_printf(&req, "Content-Length: %zu\r\n", body_len);
    for (const char *const *h = headers; h && *h; h++)
        sb_printf(&req, "%s\r\n", *h);
    sb_add(&req, "\r\n");
    if (conn_write(&c, req.p, req.len) < 0 || (body && conn_write(&c, body, body_len) < 0)) {
        snprintf(out->error, sizeof out->error, "sending the request to %s failed", host);
        goto done;
    }

    char buf[16384];
    /* streaming: where the body starts, how much of it went out, and how */
    size_t body_at = 0, handed = 0;
    bool chunked_stream = false;
    struct dechunker dch = {0};
    for (;;) {
        int n = conn_read(&c, buf, sizeof buf);
        if (n == 0)
            break;
        if (n < 0) {
            if (resp.len)
                break; /* some servers reset instead of closing cleanly */
            snprintf(out->error, sizeof out->error, n == -2 ? "%s took too long to answer" : "reading from %s failed",
                     host);
            goto done;
        }
        sb_addn(&resp, buf, (size_t)n);
        if (!on_body)
            continue;
        if (!body_at) {
            char *he = strstr(resp.p, "\r\n\r\n");
            if (!he)
                continue;
            body_at = (size_t)(he - resp.p) + 4;
            handed = body_at;
            /* only a successful reply streams; an error is read whole */
            if (strncmp(resp.p, "HTTP/1.", 7) || atoi(resp.p + 9) / 100 != 2) {
                on_body = NULL;
                continue;
            }
            char save = resp.p[body_at - 2];
            resp.p[body_at - 2] = 0;
            char *te = find_header(strchr(resp.p, '\n') + 1, "Transfer-Encoding");
            chunked_stream = te && !strncasecmp(te, "chunked", 7);
            resp.p[body_at - 2] = save;
        }
        if (resp.len > handed) {
            if (chunked_stream)
                dechunk_feed(&dch, resp.p + handed, resp.len - handed, on_body, ctx);
            else
                on_body(resp.p + handed, resp.len - handed, ctx);
            handed = resp.len;
        }
    }
    if (!resp.p || strncmp(resp.p, "HTTP/1.", 7)) {
        snprintf(out->error, sizeof out->error, "%s sent an unexpected reply", host);
        goto done;
    }
    out->status = atoi(resp.p + 9);
    char *hdr_end = strstr(resp.p, "\r\n\r\n");
    if (!hdr_end) {
        snprintf(out->error, sizeof out->error, "%s sent an incomplete reply", host);
        out->status = 0;
        goto done;
    }
    hdr_end[2] = 0; /* terminate the header block after the last CRLF */
    char *b = hdr_end + 4;
    size_t blen = resp.len - (size_t)(b - resp.p);
    char *te = find_header(strchr(resp.p, '\n') + 1, "Transfer-Encoding");
    if (te && !strncasecmp(te, "chunked", 7))
        blen = dechunk(b, blen);
    out->body = malloc(blen + 1);
    if (out->body) {
        memcpy(out->body, b, blen);
        out->body[blen] = 0;
        out->body_len = blen;
    }
    ret = 0;
done:
    if (c.use_tls) {
        mbedtls_ssl_close_notify(&c.ssl);
        mbedtls_ssl_free(&c.ssl);
    }
    close(c.fd);
    sb_free(&req);
    sb_free(&resp);
    return ret;
}

void http_response_free(struct http_response *r) {
    free(r->body);
    r->body = NULL;
}
