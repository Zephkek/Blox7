#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "studio7.h"
#include <wincrypt.h>

#include <bearssl.h>
#include "roots.h"

#define MAX_CHAIN 8
#define MAX_CERT 8192
#define ROOT_MISSING (CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN)

typedef struct {
    const br_x509_class *vtable;
    char host[256];
    BYTE certs[MAX_CHAIN][MAX_CERT];
    size_t lengths[MAX_CHAIN];
    int count;
    BOOL overflow;
    br_x509_decoder_context decoder;
    br_x509_minimal_context minimal;
    br_x509_trust_anchor roots[sizeof(g_roots) / sizeof(g_roots[0])];
    br_x509_pkey key;
    BYTE key_data[1200];
    unsigned usages;
    unsigned error;
} Trust;

typedef struct {
    SOCKET socket;
    br_ssl_client_context client;
    br_x509_minimal_context minimal;
    Trust trust;
    br_sslio_context io;
    BYTE buffer[BR_SSL_BUFSIZE_BIDI];
    BOOL secure;
    BYTE pending[16384];
    int pending_size, pending_at;
} Link;

static void trust_start_chain(const br_x509_class **context, const char *server_name) {
    Trust *t = (Trust *)context;

    t->count = 0;
    t->overflow = FALSE;
    t->error = 0;
    lstrcpynA(t->host, server_name ? server_name : "", sizeof(t->host));
}

static void trust_start_cert(const br_x509_class **context, uint32_t length) {
    Trust *t = (Trust *)context;

    if (t->count >= MAX_CHAIN || length > MAX_CERT) {
        t->overflow = TRUE;
        return;
    }
    t->lengths[t->count] = 0;
}

static void trust_append(const br_x509_class **context, const unsigned char *data, size_t length) {
    Trust *t = (Trust *)context;

    if (t->overflow || t->count >= MAX_CHAIN) return;
    if (t->lengths[t->count] + length > MAX_CERT) {
        t->overflow = TRUE;
        return;
    }
    memcpy(t->certs[t->count] + t->lengths[t->count], data, length);
    t->lengths[t->count] += length;
}

static void trust_end_cert(const br_x509_class **context) {
    Trust *t = (Trust *)context;

    if (!t->overflow && t->count < MAX_CHAIN) t->count++;
}

static BOOL root_missing(DWORD error, DWORD bits) {
    return (error == (DWORD)CERT_E_UNTRUSTEDROOT || error == (DWORD)CERT_E_CHAINING) && (bits & ROOT_MISSING) &&
           !(bits & ~ROOT_MISSING);
}

static BOOL path_checked(const Trust *t, PCCERT_CHAIN_CONTEXT chain, size_t used) {
    PCCERT_SIMPLE_CHAIN path;
    size_t i;

    if (chain->cChain != 1 || chain->rgpChain[0]->cElement < used) return FALSE;
    path = chain->rgpChain[0];
    for (i = 0; i < used; i++) {
        PCCERT_CONTEXT cert = path->rgpElement[i]->pCertContext;

        if (cert->cbCertEncoded != t->lengths[i] || memcmp(cert->pbCertEncoded, t->certs[i], t->lengths[i]) ||
            (path->rgpElement[i]->TrustStatus.dwErrorStatus & ~ROOT_MISSING) ||
            CertFindExtension(szOID_NAME_CONSTRAINTS, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension))
            return FALSE;
    }
    return TRUE;
}

static const br_x509_pkey *trust_roots(Trust *t, PCCERT_CHAIN_CONTEXT chain) {
    const br_x509_class **x = &t->minimal.vtable;
    FILETIME clock;
    ULONG64 now;
    size_t live = 0, used = 0, i;
    unsigned error;

    if (!t->host[0]) return NULL;
    GetSystemTimeAsFileTime(&clock);
    now = ((((ULONG64)clock.dwHighDateTime << 32) | clock.dwLowDateTime) - 116444736000000000ull) / 10000000;
    for (i = 0; i < sizeof(g_roots) / sizeof(g_roots[0]); i++)
        if (g_root_ends[i] >= now) t->roots[live++] = g_roots[i];
    br_x509_minimal_init_full(&t->minimal, t->roots, live);
    br_x509_minimal_set_time(&t->minimal, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
    (*x)->start_chain(x, t->host);
    while (used < (size_t)t->count && t->minimal.err != BR_ERR_X509_OK) {
        (*x)->start_cert(x, (uint32_t)t->lengths[used]);
        (*x)->append(x, t->certs[used], t->lengths[used]);
        (*x)->end_cert(x);
        used++;
    }
    error = (*x)->end_chain(x);
    if (error) {
        t->error = error;
        return NULL;
    }
    if (!path_checked(t, chain, used)) return NULL;
    return (*x)->get_pkey(x, &t->usages);
}

static unsigned trust_end_chain(const br_x509_class **context) {
    Trust *t = (Trust *)context;
    HCERTSTORE store = NULL;
    PCCERT_CONTEXT leaf = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    CERT_CHAIN_PARA chain_para;
    CERT_CHAIN_POLICY_PARA policy;
    CERT_CHAIN_POLICY_STATUS status;
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl;
    WCHAR host[256];
    char *usages[] = {(char *)szOID_PKIX_KP_SERVER_AUTH};
    const br_x509_pkey *key;
    int i;

    t->error = BR_ERR_X509_NOT_TRUSTED;
    if (t->overflow || !t->count) return t->error;
    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!store) return t->error;
    for (i = 0; i < t->count; i++) {
        PCCERT_CONTEXT added = NULL;

        if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, t->certs[i], (DWORD)t->lengths[i],
                                              CERT_STORE_ADD_ALWAYS, i ? NULL : &added))
            goto done;
        if (!i) leaf = added;
    }
    memset(&chain_para, 0, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);
    chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
    chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    if (!CertGetCertificateChain(NULL, leaf, NULL, store, &chain_para, 0, NULL, &chain)) goto done;
    MultiByteToWideChar(CP_ACP, 0, t->host, -1, host, 256);
    memset(&ssl, 0, sizeof(ssl));
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = host;
    memset(&policy, 0, sizeof(policy));
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    memset(&status, 0, sizeof(status));
    status.cbSize = sizeof(status);
    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status)) goto done;
    if (!status.dwError) {
        br_x509_decoder_init(&t->decoder, NULL, NULL);
        br_x509_decoder_push(&t->decoder, t->certs[0], t->lengths[0]);
        key = br_x509_decoder_get_pkey(&t->decoder);
        if (!key) {
            t->error = BR_ERR_X509_BAD_SIGNATURE;
            goto done;
        }
        t->usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    } else if (!root_missing(status.dwError, chain->TrustStatus.dwErrorStatus) || !(key = trust_roots(t, chain))) {
        goto done;
    }
    t->key = *key;
    if (key->key_type == BR_KEYTYPE_RSA) {
        if (key->key.rsa.nlen + key->key.rsa.elen > sizeof(t->key_data)) goto done;
        memcpy(t->key_data, key->key.rsa.n, key->key.rsa.nlen);
        memcpy(t->key_data + key->key.rsa.nlen, key->key.rsa.e, key->key.rsa.elen);
        t->key.key.rsa.n = t->key_data;
        t->key.key.rsa.e = t->key_data + key->key.rsa.nlen;
    } else if (key->key_type == BR_KEYTYPE_EC) {
        if (key->key.ec.qlen > sizeof(t->key_data)) goto done;
        memcpy(t->key_data, key->key.ec.q, key->key.ec.qlen);
        t->key.key.ec.q = t->key_data;
    } else {
        goto done;
    }
    t->error = 0;
done:
    if (chain) CertFreeCertificateChain(chain);
    if (leaf) CertFreeCertificateContext(leaf);
    if (store) CertCloseStore(store, 0);
    return t->error;
}

static const br_x509_pkey *trust_get_pkey(const br_x509_class *const *context, unsigned *usages) {
    const Trust *t = (const Trust *)context;

    if (t->error) return NULL;
    if (usages) *usages = t->usages;
    return &t->key;
}

static const br_x509_class g_trust = {
    sizeof(Trust), trust_start_chain, trust_start_cert, trust_append, trust_end_cert, trust_end_chain, trust_get_pkey,
};

static BOOL socket_ready(SOCKET s, BOOL to_write) {
    DWORD started = GetTickCount();

    for (;;) {
        struct timeval slice = {0, 200000};
        fd_set wanted, failed;
        int ready;

        FD_ZERO(&wanted);
        FD_SET(s, &wanted);
        FD_ZERO(&failed);
        FD_SET(s, &failed);
        ready = select(0, to_write ? NULL : &wanted, to_write ? &wanted : NULL, &failed, &slice);
        if (ready > 0 && !FD_ISSET(s, &failed)) return TRUE;
        if (ready) return FALSE;
        if (ui_cancelled() || GetTickCount() - started > 30000) {
            WSASetLastError(WSAETIMEDOUT);
            return FALSE;
        }
    }
}

static int socket_read(void *context, unsigned char *data, size_t length) {
    SOCKET s = *(SOCKET *)context;

    for (;;) {
        int got;

        if (!socket_ready(s, FALSE)) return -1;
        got = recv(s, (char *)data, length > 0x10000 ? 0x10000 : (int)length, 0);
        if (got > 0) return got;
        if (!got || WSAGetLastError() != WSAEWOULDBLOCK) return -1;
    }
}

static int socket_write(void *context, const unsigned char *data, size_t length) {
    SOCKET s = *(SOCKET *)context;

    for (;;) {
        int sent;

        if (!socket_ready(s, TRUE)) return -1;
        sent = send(s, (const char *)data, length > 0x10000 ? 0x10000 : (int)length, 0);
        if (sent > 0) return sent;
        if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;
    }
}

static BOOL socket_connect(SOCKET s, const struct addrinfo *to) {
    u_long on = 1;
    int problem = 0, size = sizeof(problem);

    if (ioctlsocket(s, FIONBIO, &on)) return FALSE;
    if (!connect(s, to->ai_addr, (int)to->ai_addrlen)) return TRUE;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return FALSE;
    if (socket_ready(s, TRUE)) return TRUE;
    if (!getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&problem, &size) && problem) WSASetLastError(problem);
    return FALSE;
}

static BOOL link_open(Link *l, const char *host, const char *port, BOOL secure, char *error, size_t room) {
    static BOOL started;
    struct addrinfo hints, *found = NULL, *a;
    WSADATA wsa;
    int problem = 0;

    if (!started) {
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = TRUE;
    }
    l->socket = INVALID_SOCKET;
    l->secure = secure;
    l->pending_size = l->pending_at = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found)) {
        text_format(error, room, "cannot find %s (error %d)", host, WSAGetLastError());
        return FALSE;
    }
    for (a = found; a && !ui_cancelled(); a = a->ai_next) {
        l->socket = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (l->socket == INVALID_SOCKET) continue;
        if (socket_connect(l->socket, a)) break;
        problem = WSAGetLastError();
        closesocket(l->socket);
        l->socket = INVALID_SOCKET;
    }
    freeaddrinfo(found);
    if (l->socket == INVALID_SOCKET) {
        text_format(error, room, "cannot connect to %s (error %d)", host, problem);
        return FALSE;
    }
    if (!secure) return TRUE;
    br_ssl_client_init_full(&l->client, &l->minimal, NULL, 0);
    br_ssl_engine_set_versions(&l->client.eng, BR_TLS12, BR_TLS12);
    memset(&l->trust, 0, sizeof(l->trust));
    l->trust.vtable = &g_trust;
    br_ssl_engine_set_x509(&l->client.eng, &l->trust.vtable);
    br_ssl_engine_set_buffer(&l->client.eng, l->buffer, sizeof(l->buffer), 1);
    if (!br_ssl_client_reset(&l->client, host, 0)) {
        text_format(error, room, "cannot start TLS");
        closesocket(l->socket);
        return FALSE;
    }
    br_sslio_init(&l->io, &l->client.eng, socket_read, &l->socket, socket_write, &l->socket);
    return TRUE;
}

static void link_close(Link *l) {
    if (l->socket != INVALID_SOCKET) closesocket(l->socket);
    l->socket = INVALID_SOCKET;
}

static BOOL link_send(Link *l, const char *data, size_t length) {
    if (l->secure) return !br_sslio_write_all(&l->io, data, length) && !br_sslio_flush(&l->io);
    while (length) {
        int sent = socket_write(&l->socket, (const unsigned char *)data, length);

        if (sent <= 0) return FALSE;
        data += sent;
        length -= (size_t)sent;
    }
    return TRUE;
}

static int link_read(Link *l, BYTE *data, int room) {
    if (l->pending_at < l->pending_size) {
        int n = l->pending_size - l->pending_at;

        if (n > room) n = room;
        memcpy(data, l->pending + l->pending_at, (size_t)n);
        l->pending_at += n;
        return n;
    }
    if (l->secure) return br_sslio_read(&l->io, data, (size_t)room);
    return socket_read(&l->socket, data, (size_t)room);
}

static BOOL link_ended(Link *l) {
    return !l->secure || br_ssl_engine_last_error(&l->client.eng) == BR_ERR_OK;
}

static void link_problem(Link *l, const char *host, char *error, size_t room) {
    int tls = l->secure ? br_ssl_engine_last_error(&l->client.eng) : 0;

    if (tls == BR_ERR_X509_NOT_TRUSTED || (tls >= 32 && tls < 64))
        text_format(error, room, "%s did not present a certificate this Windows trusts (TLS error %d)", host, tls);
    else if (tls)
        text_format(error, room, "the secure connection to %s failed (TLS error %d)", host, tls);
    else
        text_format(error, room, "the connection to %s was lost (error %d)", host, WSAGetLastError());
}

static BOOL read_head(Link *l, char *head, int room) {
    int size = 0;

    while (size < room - 1) {
        int got = link_read(l, (BYTE *)head + size, room - 1 - size), i;

        if (got <= 0) return FALSE;
        size += got;
        head[size] = 0;
        for (i = 3; i < size; i++) {
            if (head[i - 3] == '\r' && head[i - 2] == '\n' && head[i - 1] == '\r' && head[i] == '\n') {
                l->pending_size = size - (i + 1);
                l->pending_at = 0;
                if (l->pending_size > (int)sizeof(l->pending)) return FALSE;
                memcpy(l->pending, head + i + 1, (size_t)l->pending_size);
                head[i + 1] = 0;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static const char *header(const char *head, const char *name) {
    size_t length = strlen(name);
    const char *line = head;

    while ((line = strstr(line, "\r\n")) != NULL) {
        line += 2;
        if (!_strnicmp(line, name, length) && line[length] == ':') {
            line += length + 1;
            while (*line == ' ') line++;
            return line;
        }
    }
    return NULL;
}

static BOOL split_url(const char *url, char *host, size_t host_room, const char **path, BOOL *secure) {
    const char *start, *slash;
    size_t length;

    if (!strncmp(url, "https://", 8)) {
        *secure = TRUE;
        start = url + 8;
    } else if (!strncmp(url, "http://", 7)) {
        *secure = FALSE;
        start = url + 7;
    } else {
        return FALSE;
    }
    slash = strchr(start, '/');
    length = slash ? (size_t)(slash - start) : strlen(start);
    if (!length || length >= host_room) return FALSE;
    memcpy(host, start, length);
    host[length] = 0;
    *path = slash ? slash : "/";
    return TRUE;
}

static BOOL get_once(const char *url, HttpSink sink, void *context, ULONG64 resume_from, HttpResult *result, char *moved,
                     size_t moved_room) {
    Link *l = calloc(1, sizeof(Link));
    char host[256], request[2400], head[8192], *port;
    const char *path, *value;
    BYTE *block = NULL;
    BOOL secure, chunked, ok = FALSE;
    ULONG64 remaining = 0;
    BOOL sized;
    int got;

    memset(result, 0, sizeof(*result));
    if (!l || !split_url(url, host, sizeof(host), &path, &secure)) {
        text_format(result->error, sizeof(result->error), "bad address %s", url);
        free(l);
        return FALSE;
    }
    port = strchr(host, ':');
    if (port) *port++ = 0;
    if (!link_open(l, host, port ? port : secure ? "443" : "80", secure, result->error, sizeof(result->error))) {
        free(l);
        return FALSE;
    }
    if (resume_from)
        text_format(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Studio7\r\nAccept: */*\r\n"
                    "Range: bytes=%I64u-\r\nConnection: close\r\n\r\n", path, host, resume_from);
    else
        text_format(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Studio7\r\nAccept: */*\r\n"
                    "Connection: close\r\n\r\n", path, host);
    if (!link_send(l, request, strlen(request)) || !read_head(l, head, sizeof(head))) {
        link_problem(l, host, result->error, sizeof(result->error));
        goto done;
    }
    if (strncmp(head, "HTTP/1.", 7) || strlen(head) < 12) {
        text_format(result->error, sizeof(result->error), "%s did not answer with HTTP", host);
        goto done;
    }
    result->status = atoi(head + 9);
    value = header(head, "Content-Length");
    sized = value != NULL;
    if (sized) remaining = _strtoui64(value, NULL, 10);
    result->length = remaining;
    value = header(head, "Transfer-Encoding");
    chunked = value && !_strnicmp(value, "chunked", 7);
    value = header(head, "Location");
    if (value && (result->status == 301 || result->status == 302 || result->status == 303 || result->status == 307 ||
                  result->status == 308)) {
        size_t n = 0;

        while (value[n] && value[n] != '\r' && value[n] != '\n' && n + 1 < moved_room) n++;
        memcpy(moved, value, n);
        moved[n] = 0;
        goto done;
    }
    if (result->status != 200 && result->status != 206) {
        text_format(result->error, sizeof(result->error), "%s answered %d for %.200s", host, result->status, path);
        goto done;
    }
    if (resume_from && result->status != 206) {
        text_format(result->error, sizeof(result->error), "%s cannot resume a download", host);
        goto done;
    }
    block = malloc(65536);
    if (!block) goto done;
    if (chunked) {
        for (;;) {
            char line[64];
            int n = 0;
            ULONG64 size;

            for (;;) {
                BYTE c;

                if (link_read(l, &c, 1) != 1 || n == 63) goto lost;
                if (c == '\n') break;
                if (c != '\r') line[n++] = (char)c;
            }
            line[n] = 0;
            size = _strtoui64(line, NULL, 16);
            if (!size) break;
            while (size) {
                got = link_read(l, block, size > 65536 ? 65536 : (int)size);
                if (got <= 0) goto lost;
                if (!sink(context, block, (DWORD)got)) goto stopped;
                result->received += (ULONG64)got;
                size -= (ULONG64)got;
            }
            if (link_read(l, block, 1) != 1 || (block[0] == '\r' && link_read(l, block, 1) != 1)) goto lost;
        }
    } else {
        while (!sized || remaining) {
            got = link_read(l, block, sized && remaining < 65536 ? (int)remaining : 65536);
            if (got <= 0) {
                if (!sized && link_ended(l)) break;
                goto lost;
            }
            if (!sink(context, block, (DWORD)got)) goto stopped;
            result->received += (ULONG64)got;
            if (sized) remaining -= (ULONG64)got;
        }
    }
    ok = TRUE;
    goto done;
lost:
    link_problem(l, host, result->error, sizeof(result->error));
    goto done;
stopped:
    text_format(result->error, sizeof(result->error), "stopped");
done:
    free(block);
    link_close(l);
    free(l);
    return ok;
}

BOOL http_get(const char *url, HttpSink sink, void *context, ULONG64 resume_from, HttpResult *result) {
    char next[2048], moved[2048];
    int hop;

    memset(result, 0, sizeof(*result));
    if (strlen(url) >= sizeof(next)) return FALSE;
    strcpy(next, url);
    for (hop = 0; hop < 6; hop++) {
        moved[0] = 0;
        if (get_once(next, sink, context, resume_from, result, moved, sizeof(moved))) return TRUE;
        if (!moved[0]) return FALSE;
        if (!strncmp(next, "https://", 8) && strncmp(moved, "https://", 8)) break;
        strcpy(next, moved);
    }
    text_format(result->error, sizeof(result->error), "%.80s sent the request somewhere it cannot follow", url);
    return FALSE;
}
