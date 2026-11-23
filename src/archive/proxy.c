/**
 * vimb - a webkit based vim like browser.
 *
 * Copyright (C) 2026 vimb contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#include <string.h>

#include "ca.h"
#include "http1.h"
#include "netcheck.h"
#include "proxy.h"
#include "recorder.h"
#include "spool.h"
#include "warc.h"

#define BUF_SIZE            (64 * 1024)
#define MAX_HEAD            (256 * 1024)
#define MAX_REQUEST_BODY    (64 * 1024 * 1024)
#define SPOOL_MEMORY        (512 * 1024)
#define DEFAULT_MAX_RECORD  (G_GUINT64_CONSTANT(4) << 30)
#define CLIENT_TIMEOUT      120     /* seconds a browser connection may idle */
#define UPSTREAM_TIMEOUT    60
#define CONNECT_TIMEOUT     30
#define ANNOUNCE_TIMEOUT    (10 * G_TIME_SPAN_SECOND)
#define MAX_THREADS         256

typedef enum {
    HOST_PENDING = 1,
    HOST_ANNOUNCED,
} HostState;

struct ArchiveProxy {
    char                  *tmp_dir;
    gboolean              verify_upstream;
    gboolean              allow_internal;
    gboolean              check_peer_user;
    guint64               max_record_size;
    ArchiveProxyAllowFunc allow_certificate;
    gpointer              allow_data;
    GMainContext          *owner_context;

    ArchiveCa             *ca;
    ArchiveRecorder       *recorder;        /* under lock; NULL once finished */
    guint                 recorded;         /* what it wrote, once finished */
    gint                  paused;

    GThread               *thread;          /* accepts connections */
    GMainContext          *context;
    GMainLoop             *loop;
    GThreadPool           *pool;            /* serves them */
    guint16               port;

    GMutex                lock;
    GCond                 cond;
    gboolean              ready;
    GError                *start_error;
    gint                  stopping;
    GHashTable            *hosts;           /* host -> HostState */
    GHashTable            *connections;     /* the Conns being served */
    GPtrArray             *allow_sources;   /* pending calls on the owner context */
};

typedef struct {
    GInputStream *in;
    guint8       *buf;
    gsize        start;
    gsize        end;
} Reader;

/* A browser connection being served, and the site connection it has open. */
typedef struct {
    ArchiveProxy      *proxy;
    GCancellable      *cancellable;
    GSocketConnection *client;
    GIOStream         *stream;      /* client, or the TLS connection over it */
    Reader            reader;
    GSocketConnection *up_socket;
    GIOStream         *up;          /* up_socket, or the TLS connection over it */
    Reader            up_reader;
    char              *up_origin;
    char              *up_ip;
    gboolean          up_reused;
} Conn;

/* Where a request goes. */
typedef struct {
    gboolean tls;
    char     *host;
    guint16  port;
    char     *target;               /* origin-form, as sent to the site */
    char     *uri;                  /* absolute, as archived */
} Destination;

/* ----------------------------------------------------------------------- */

static void reader_init(Reader *r, GInputStream *in)
{
    r->in = in;
    if (!r->buf) {
        r->buf = g_malloc(BUF_SIZE);
    }
    r->start = r->end = 0;
}

static void reader_clear(Reader *r)
{
    g_clear_pointer(&r->buf, g_free);
    r->in    = NULL;
    r->start = r->end = 0;
}

/* What is buffered, or else what one read brings: 0 at the end of the
 * stream, -1 on error. */
static gssize reader_fill(Reader *r, GCancellable *cancellable, GError **error)
{
    if (r->start < r->end) {
        return (gssize)(r->end - r->start);
    }
    r->start = r->end = 0;
    gssize n = g_input_stream_read(r->in, r->buf, BUF_SIZE, cancellable, error);
    if (n > 0) {
        r->end = (gsize)n;
    }
    return n;
}

static gboolean write_out(GIOStream *stream, const void *data, gsize len,
        GCancellable *cancellable, GError **error)
{
    return g_output_stream_write_all(g_io_stream_get_output_stream(stream), data, len,
            NULL, cancellable, error);
}

/* Whether an exchange starting now is to be recorded. */
static gboolean proxy_recording(ArchiveProxy *proxy)
{
    g_mutex_lock(&proxy->lock);
    gboolean recording = proxy->recorder && !g_atomic_int_get(&proxy->paused);
    g_mutex_unlock(&proxy->lock);
    return recording;
}

/* Hands an exchange to the recorder -- or drops it, if recording has
 * finished since it began. */
static void proxy_submit(ArchiveProxy *proxy, ArchiveExchange *exchange)
{
    g_mutex_lock(&proxy->lock);
    if (proxy->recorder) {
        archive_recorder_submit(proxy->recorder, exchange);
        exchange = NULL;
    }
    g_mutex_unlock(&proxy->lock);
    archive_exchange_free(exchange);
}

static void send_error(Conn *conn, guint status, const char *reason, const char *detail)
{
    char *escaped = g_markup_escape_text(detail ? detail : "", -1);
    char *body    = g_strdup_printf("<!doctype html><meta charset=utf-8>"
            "<title>%u %s</title><h1>%u %s</h1><p>%s</p>"
            "<hr><p><small>vimb archiving proxy</small></p>\n",
            status, reason, status, reason, escaped);
    char *head    = g_strdup_printf("HTTP/1.1 %u %s\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %" G_GSIZE_FORMAT "\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n", status, reason, strlen(body));

    if (write_out(conn->stream, head, strlen(head), conn->cancellable, NULL)) {
        write_out(conn->stream, body, strlen(body), conn->cancellable, NULL);
    }
    g_free(head);
    g_free(body);
    g_free(escaped);
}

static char *host_for_uri(const char *host)
{
    return strchr(host, ':') ? g_strdup_printf("[%s]", host) : g_strdup(host);
}

static void destination_clear(Destination *dest)
{
    g_free(dest->host);
    g_free(dest->target);
    g_free(dest->uri);
    memset(dest, 0, sizeof(*dest));
}

static void destination_set_uri(Destination *dest)
{
    char *host   = host_for_uri(dest->host);
    guint16 port = dest->tls ? 443 : 80;
    const char *path = dest->target[0] == '/' ? dest->target : "/";
    if (dest->port == port) {
        dest->uri = g_strdup_printf("%s://%s%s", dest->tls ? "https" : "http", host, path);
    } else {
        dest->uri = g_strdup_printf("%s://%s:%u%s", dest->tls ? "https" : "http", host,
                dest->port, path);
    }
    g_free(host);
}

/* Where a request's target says it goes. tunnel is the destination of the
 * CONNECT it came through, if it came through one. */
static gboolean resolve_destination(const char *target, const Destination *tunnel,
        Destination *dest, GError **error)
{
    memset(dest, 0, sizeof(*dest));

    if (target[0] == '/' || !strcmp(target, "*")) {
        if (!tunnel) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "This is vimb's archiving proxy; it takes proxy requests only.");
            return FALSE;
        }
        dest->tls    = tunnel->tls;
        dest->host   = g_strdup(tunnel->host);
        dest->port   = tunnel->port;
        dest->target = g_strdup(target);
        destination_set_uri(dest);
        return TRUE;
    }

    GUri *uri = g_uri_parse(target, G_URI_FLAGS_ENCODED, error);
    if (!uri) {
        return FALSE;
    }
    const char *scheme = g_uri_get_scheme(uri);
    const char *host   = g_uri_get_host(uri);
    gboolean ok = TRUE;
    if (g_ascii_strcasecmp(scheme, "http") && g_ascii_strcasecmp(scheme, "https")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Only http and https are proxied, not %s.", scheme);
        ok = FALSE;
    } else if (!host || !*host) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "The request names no host.");
        ok = FALSE;
    }
    if (ok) {
        const char *path  = g_uri_get_path(uri);
        const char *query = g_uri_get_query(uri);
        int port          = g_uri_get_port(uri);
        dest->tls    = !g_ascii_strcasecmp(scheme, "https");
        dest->host   = g_strdup(host);
        dest->port   = port > 0 ? (guint16)port : (dest->tls ? 443 : 80);
        dest->target = g_strconcat(*path ? path : "/", query ? "?" : "", query ? query : "",
                NULL);
        if (tunnel && (g_ascii_strcasecmp(dest->host, tunnel->host) || dest->port != tunnel->port)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "The request names another host than its tunnel.");
            destination_clear(dest);
            ok = FALSE;
        } else {
            destination_set_uri(dest);
        }
    }
    g_uri_unref(uri);
    return ok;
}

/* ----------------------------------------------------------------------- */
/* Telling the owner about each site's certificate, before the browser sees it. */

struct ArchiveProxyAllowCall {
    ArchiveProxy    *proxy;     /* only for run_allow, on the owner's thread */
    char            *host;
    GTlsCertificate *cert;
    GSource         *source;
    GMutex          lock;
    GCond           cond;
    gboolean        done;
    gint            refs;
};

static void allow_call_unref(ArchiveProxyAllowCall *call)
{
    if (g_atomic_int_dec_and_test(&call->refs)) {
        g_source_unref(call->source);
        g_object_unref(call->cert);
        g_mutex_clear(&call->lock);
        g_cond_clear(&call->cond);
        g_free(call->host);
        g_free(call);
    }
}

void archive_proxy_allow_call_done(ArchiveProxyAllowCall *call)
{
    g_mutex_lock(&call->lock);
    call->done = TRUE;
    g_cond_broadcast(&call->cond);
    g_mutex_unlock(&call->lock);
    allow_call_unref(call);     /* the owner's reference */
}

static gboolean run_allow(gpointer data)
{
    ArchiveProxyAllowCall *call = data;
    ArchiveProxy *proxy         = call->proxy;

    g_mutex_lock(&proxy->lock);
    g_ptr_array_remove(proxy->allow_sources, call->source);
    g_mutex_unlock(&proxy->lock);

    g_atomic_int_inc(&call->refs);
    proxy->allow_certificate(call->host, call->cert, call, proxy->allow_data);
    return G_SOURCE_REMOVE;
}

/* Has the owner told about host's certificate, once per host, and waits
 * until the browser accepts it. FALSE if that did not happen. */
static gboolean announce_certificate(ArchiveProxy *proxy, const char *host,
        GTlsCertificate *cert)
{
    if (!proxy->allow_certificate) {
        return TRUE;
    }

    char *key = g_ascii_strdown(host, -1);
    g_mutex_lock(&proxy->lock);
    for (;;) {
        HostState state = GPOINTER_TO_INT(g_hash_table_lookup(proxy->hosts, key));
        if (state == HOST_ANNOUNCED || proxy->stopping) {
            g_mutex_unlock(&proxy->lock);
            g_free(key);
            return state == HOST_ANNOUNCED;
        }
        if (!state) {
            break;
        }
        g_cond_wait(&proxy->cond, &proxy->lock);    /* another thread is at it */
    }
    g_hash_table_insert(proxy->hosts, g_strdup(key), GINT_TO_POINTER(HOST_PENDING));

    ArchiveProxyAllowCall *call = g_new0(ArchiveProxyAllowCall, 1);
    call->proxy  = proxy;
    call->host   = g_strdup(host);
    call->cert   = g_object_ref(cert);
    call->refs   = 2;           /* this thread's, and the source's */
    call->source = g_idle_source_new();
    g_mutex_init(&call->lock);
    g_cond_init(&call->cond);
    g_source_set_priority(call->source, G_PRIORITY_HIGH);
    g_source_set_callback(call->source, run_allow, call, (GDestroyNotify)allow_call_unref);
    g_ptr_array_add(proxy->allow_sources, call->source);
    g_source_attach(call->source, proxy->owner_context);
    g_mutex_unlock(&proxy->lock);

    /* Waited for in slices, to notice the proxy stopping. */
    gint64 deadline = g_get_monotonic_time() + ANNOUNCE_TIMEOUT;
    gboolean done   = FALSE;
    g_mutex_lock(&call->lock);
    while (!(done = call->done) && g_get_monotonic_time() < deadline
            && !g_atomic_int_get(&proxy->stopping)) {
        g_cond_wait_until(&call->cond, &call->lock,
                MIN(deadline, g_get_monotonic_time() + G_TIME_SPAN_SECOND / 10));
    }
    g_mutex_unlock(&call->lock);

    g_mutex_lock(&proxy->lock);
    if (done) {
        g_hash_table_insert(proxy->hosts, g_strdup(key), GINT_TO_POINTER(HOST_ANNOUNCED));
    } else {
        if (!proxy->stopping) {
            g_warning("archive: the browser did not accept %s's certificate in time", host);
        }
        g_hash_table_remove(proxy->hosts, key);
    }
    g_cond_broadcast(&proxy->cond);
    g_mutex_unlock(&proxy->lock);
    allow_call_unref(call);
    g_free(key);
    return done;
}

/* ----------------------------------------------------------------------- */
/* The site's end. */

typedef struct {
    gboolean             verify;
    GTlsCertificateFlags errors;
} TlsCheck;

static gboolean on_accept_certificate(GTlsConnection *tls, GTlsCertificate *peer,
        GTlsCertificateFlags errors, gpointer data)
{
    TlsCheck *check = data;
    check->errors   = errors;
    return !check->verify;
}

static char *describe_tls_errors(GTlsCertificateFlags flags)
{
    GString *s = g_string_new(NULL);
    static const struct { GTlsCertificateFlags flag; const char *text; } names[] = {
        {G_TLS_CERTIFICATE_UNKNOWN_CA,    "issued by an unknown authority"},
        {G_TLS_CERTIFICATE_BAD_IDENTITY,  "for another site"},
        {G_TLS_CERTIFICATE_NOT_ACTIVATED, "not yet valid"},
        {G_TLS_CERTIFICATE_EXPIRED,       "expired"},
        {G_TLS_CERTIFICATE_REVOKED,       "revoked"},
        {G_TLS_CERTIFICATE_INSECURE,      "insecure"},
        {G_TLS_CERTIFICATE_GENERIC_ERROR, "invalid"},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        if (flags & names[i].flag) {
            g_string_append_printf(s, "%s%s", s->len ? ", " : "", names[i].text);
        }
    }
    return g_string_free(s, FALSE);
}

static void drop_upstream(Conn *conn)
{
    if (conn->up) {
        g_io_stream_close(conn->up, NULL, NULL);
        g_clear_object(&conn->up);
    }
    g_clear_object(&conn->up_socket);
    g_clear_pointer(&conn->up_origin, g_free);
    g_clear_pointer(&conn->up_ip, g_free);
    conn->up_reused = FALSE;
}

/* Makes sure conn has a connection to dest's site, reusing the one it has
 * when it goes there. On failure, *status is what the browser is told. */
static gboolean connect_upstream(Conn *conn, const Destination *dest, guint *status,
        GError **error)
{
    ArchiveProxy *proxy = conn->proxy;
    char *origin = g_strdup_printf("%s://%s:%u", dest->tls ? "https" : "http", dest->host,
            dest->port);

    if (conn->up && !g_strcmp0(conn->up_origin, origin)) {
        conn->up_reused = TRUE;
        g_free(origin);
        return TRUE;
    }
    drop_upstream(conn);
    *status = 502;

    GResolver *resolver = g_resolver_get_default();
    GList *addresses    = g_resolver_lookup_by_name(resolver, dest->host, conn->cancellable,
            error);
    g_object_unref(resolver);
    if (!addresses) {
        g_free(origin);
        return FALSE;
    }

    /* Connect to an address checked here, not to the name: looking it up
     * again could give another answer. */
    GSocketClient *client = g_socket_client_new();
    GError *last          = NULL;
    gboolean blocked      = FALSE;
    g_socket_client_set_timeout(client, CONNECT_TIMEOUT);
    for (GList *l = addresses; l && !conn->up_socket; l = l->next) {
        GInetAddress *address = l->data;
        if (!proxy->allow_internal && archive_address_is_internal(address)) {
            blocked = TRUE;
            continue;
        }
        GSocketAddress *sa = g_inet_socket_address_new(address, dest->port);
        g_clear_error(&last);
        conn->up_socket = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(sa),
                conn->cancellable, &last);
        if (conn->up_socket) {
            conn->up_ip = g_inet_address_to_string(address);
        }
        g_object_unref(sa);
    }
    g_resolver_free_addresses(addresses);
    g_object_unref(client);

    if (!conn->up_socket) {
        if (last) {
            g_propagate_error(error, last);
        } else if (blocked) {
            *status = 403;
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s is on an internal network, which archiving doesn't reach.",
                    dest->host);
        }
        g_free(origin);
        return FALSE;
    }
    g_socket_set_timeout(g_socket_connection_get_socket(conn->up_socket), UPSTREAM_TIMEOUT);
    conn->up = G_IO_STREAM(g_object_ref(conn->up_socket));

    if (dest->tls) {
        GSocketConnectable *identity = g_network_address_new(dest->host, dest->port);
        GIOStream *tls = g_tls_client_connection_new(G_IO_STREAM(conn->up_socket), identity,
                error);
        g_object_unref(identity);
        if (!tls) {
            drop_upstream(conn);
            g_free(origin);
            return FALSE;
        }
        /* HTTP/1.1 only: that is what gets recorded as it came. */
        const char *protocols[] = {"http/1.1", NULL};
        g_tls_connection_set_advertised_protocols(G_TLS_CONNECTION(tls), protocols);

        TlsCheck check = {proxy->verify_upstream, 0};
        gulong handler = g_signal_connect(tls, "accept-certificate",
                G_CALLBACK(on_accept_certificate), &check);
        GError *tls_error = NULL;
        gboolean ok = g_tls_connection_handshake(G_TLS_CONNECTION(tls), conn->cancellable,
                &tls_error);
        g_signal_handler_disconnect(tls, handler);
        if (!ok) {
            if (check.errors) {
                char *why = describe_tls_errors(check.errors);
                g_set_error(error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE,
                        "The certificate of %s is %s, so it wasn't archived.", dest->host, why);
                g_free(why);
                g_clear_error(&tls_error);
            } else {
                g_propagate_error(error, tls_error);
            }
            g_object_unref(tls);
            drop_upstream(conn);
            g_free(origin);
            return FALSE;
        }
        if (check.errors) {
            char *why = describe_tls_errors(check.errors);
            g_message("archive: %s's certificate is %s; archiving it anyway", dest->host, why);
            g_free(why);
        }
        g_object_unref(conn->up);
        conn->up = tls;
    }
    reader_init(&conn->up_reader, g_io_stream_get_input_stream(conn->up));
    conn->up_origin = origin;
    conn->up_reused = FALSE;
    return TRUE;
}

/* ----------------------------------------------------------------------- */
/* Websockets and other upgrades: once switched, bytes are passed both ways
 * until either side stops. Only the handshake is archived. */

typedef struct {
    GInputStream  *in;
    GOutputStream *out;
    GCancellable  *cancellable;
} Pipe;

static gpointer pipe_run(gpointer data)
{
    Pipe *pipe  = data;
    guint8 *buf = g_malloc(BUF_SIZE);
    for (;;) {
        gssize n = g_input_stream_read(pipe->in, buf, BUF_SIZE, pipe->cancellable, NULL);
        if (n <= 0 || !g_output_stream_write_all(pipe->out, buf, (gsize)n, NULL,
                    pipe->cancellable, NULL)) {
            break;
        }
    }
    g_free(buf);
    g_cancellable_cancel(pipe->cancellable);
    return NULL;
}

static void splice_upgraded(Conn *conn)
{
    /* Whatever either side sent after the handshake is already buffered. */
    if (conn->up_reader.start < conn->up_reader.end
            && !write_out(conn->stream, conn->up_reader.buf + conn->up_reader.start,
                conn->up_reader.end - conn->up_reader.start, conn->cancellable, NULL)) {
        return;
    }
    if (conn->reader.start < conn->reader.end
            && !write_out(conn->up, conn->reader.buf + conn->reader.start,
                conn->reader.end - conn->reader.start, conn->cancellable, NULL)) {
        return;
    }
    conn->up_reader.start = conn->up_reader.end = 0;
    conn->reader.start    = conn->reader.end = 0;

    /* A websocket may sit quiet for longer than a request would. */
    g_socket_set_timeout(g_socket_connection_get_socket(conn->client), 0);
    g_socket_set_timeout(g_socket_connection_get_socket(conn->up_socket), 0);

    Pipe down = {g_io_stream_get_input_stream(conn->up),
        g_io_stream_get_output_stream(conn->stream), conn->cancellable};
    Pipe up   = {g_io_stream_get_input_stream(conn->stream),
        g_io_stream_get_output_stream(conn->up), conn->cancellable};
    GThread *thread = g_thread_new("archive-upgraded", pipe_run, &down);
    pipe_run(&up);
    g_thread_join(thread);
}

/* ----------------------------------------------------------------------- */
/* One request and its response. */

static void on_payload(const guint8 *data, gsize len, gpointer user_data)
{
    g_checksum_update(user_data, data, (gssize)len);
}

static void on_raw_body(const guint8 *data, gsize len, gpointer user_data)
{
    g_byte_array_append(user_data, data, (guint)len);
}

static gboolean expects_continue(const Http1Parser *request)
{
    return http1_head_has_token(http1_parser_head(request), "Expect", "100-continue");
}

/* Reads a request from the browser: its head into parser, its body, as it
 * came, into body. FALSE, with no error, when the browser closes the
 * connection between requests. */
static gboolean read_request(Conn *conn, Http1Parser *parser, GByteArray *body,
        GError **error)
{
    gboolean any = FALSE;

    http1_parser_reset(parser);
    g_byte_array_set_size(body, 0);
    http1_parser_set_raw_body_func(parser, on_raw_body, body);

    while (!http1_parser_is_done(parser)) {
        gssize n = reader_fill(&conn->reader, conn->cancellable, error);
        if (n < 0) {
            return FALSE;
        }
        if (n == 0) {
            if (any) {
                g_set_error_literal(error, HTTP1_ERROR, HTTP1_ERROR_TRUNCATED,
                        "The browser closed its connection within a request");
            }
            return FALSE;
        }
        gboolean had_head = http1_parser_has_head(parser);
        gssize used = http1_parser_feed(parser, conn->reader.buf + conn->reader.start,
                (gsize)n, error);
        if (used < 0) {
            return FALSE;
        }
        conn->reader.start += (gsize)used;
        any = TRUE;

        /* The proxy reads the whole body before passing the request on, so a
         * browser waiting to be told to go ahead is told so here. */
        if (!had_head && http1_parser_has_head(parser) && !http1_parser_is_done(parser)
                && expects_continue(parser)) {
            static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
            if (!write_out(conn->stream, go, sizeof(go) - 1, conn->cancellable, error)) {
                return FALSE;
            }
        }
        if (body->len > MAX_REQUEST_BODY) {
            g_set_error_literal(error, HTTP1_ERROR, HTTP1_ERROR_TOO_LARGE,
                    "Request body too large");
            return FALSE;
        }
    }
    return TRUE;
}

/* The request as it goes to the site: in origin-form, without the headers
 * that were for the proxy. */
static GString *upstream_head(const Http1Parser *request, const char *target)
{
    const Http1Head *head = http1_parser_head(request);
    gboolean drop_expect  = expects_continue(request);
    GString *out          = g_string_new(NULL);

    g_string_append_printf(out, "%s %s HTTP/1.%d\r\n", head->method, target,
            head->minor_version);
    for (guint i = 0; i < head->headers->len; i++) {
        Http1Header *h = &g_array_index(head->headers, Http1Header, i);
        if (!g_ascii_strncasecmp(h->name, "Proxy-", 6)
                || (drop_expect && !g_ascii_strcasecmp(h->name, "Expect"))) {
            continue;
        }
        g_string_append_printf(out, "%s: %s\r\n", h->name, h->value);
    }
    g_string_append(out, "\r\n");
    return out;
}

typedef enum {
    RELAY_DONE,         /* answered; *keep says whether the browser may send more */
    RELAY_STALE,        /* a reused site connection had closed; try a new one */
    RELAY_FAILED,       /* the browser was told, or is gone */
} RelayResult;

/* Passes the site's response to the browser as it comes, recording it into
 * exchange when there is one. Sets *submitted when it has handed the
 * exchange to the recorder itself. */
static RelayResult relay_response(Conn *conn, const char *method, gboolean upgrade,
        ArchiveExchange *exchange, gboolean *keep, gboolean *submitted)
{
    ArchiveProxy *proxy = conn->proxy;
    Http1Parser *parser = http1_parser_new(HTTP1_RESPONSE, MAX_HEAD);
    GChecksum *sha1     = g_checksum_new(G_CHECKSUM_SHA1);
    Spool *spool        = exchange ? spool_new(proxy->tmp_dir, SPOOL_MEMORY) : NULL;
    gboolean spooling   = exchange != NULL;
    gboolean client_ok  = TRUE;
    gboolean received   = FALSE;
    RelayResult result  = RELAY_DONE;
    GError *error       = NULL;

    *keep      = FALSE;
    *submitted = FALSE;
    http1_parser_set_request_method(parser, method);
    http1_parser_set_payload_func(parser, on_payload, sha1);

    for (;;) {
        gssize n = reader_fill(&conn->up_reader, conn->cancellable, &error);
        if (n == 0 && http1_parser_eof(parser, NULL)) {
            break;                              /* a body that ran to the close */
        }
        if (n <= 0) {
            if (!received && conn->up_reused && !g_cancellable_is_cancelled(conn->cancellable)) {
                result = RELAY_STALE;
            } else if (!received) {
                send_error(conn, 502, "Bad Gateway", error ? error->message
                        : "The site closed the connection without answering.");
                result = RELAY_FAILED;
            } else if (exchange && http1_parser_has_head(parser)) {
                /* Cut off within the body: keep what came, marked so. */
                g_free(exchange->truncated);
                exchange->truncated = g_strdup("disconnect");
                break;
            } else {
                result = RELAY_FAILED;
            }
            goto out;
        }
        received = TRUE;

        const guint8 *data = conn->up_reader.buf + conn->up_reader.start;
        gssize used = http1_parser_feed(parser, data, (gsize)n, &error);
        if (used < 0) {
            if (!http1_parser_has_head(parser)) {
                char *why = g_strdup_printf("The site's answer was not HTTP/1.x: %s",
                        error->message);
                send_error(conn, 502, "Bad Gateway", why);
                g_free(why);
            }
            result = RELAY_FAILED;
            goto out;
        }
        conn->up_reader.start += (gsize)used;

        if (client_ok && !write_out(conn->stream, data, (gsize)used, conn->cancellable, NULL)) {
            client_ok = FALSE;              /* gone; finish the record if it's wanted */
        }
        if (spooling) {
            if (spool_length(spool) + (guint64)used > proxy->max_record_size
                    || !spool_append(spool, data, (gsize)used, NULL)) {
                spooling = FALSE;
                g_free(exchange->truncated);
                exchange->truncated = g_strdup("length");
            }
        }
        if (!client_ok && !spooling) {
            result = RELAY_FAILED;
            goto out;
        }

        if (http1_parser_is_done(parser)) {
            guint status = http1_parser_head(parser)->status;
            if (status / 100 == 1 && status != 101) {
                /* 100 Continue, 103 Early Hints: passed on, not archived. */
                http1_parser_reset(parser);
                g_checksum_reset(sha1);
                if (spool) {
                    spool_free(spool);
                    spool    = spool_new(proxy->tmp_dir, SPOOL_MEMORY);
                    spooling = TRUE;
                    g_clear_pointer(&exchange->truncated, g_free);
                }
                continue;
            }
            break;
        }
    }

    gboolean done     = http1_parser_is_done(parser);
    gboolean switched = done && upgrade && http1_parser_head(parser)->status == 101;
    if (exchange && http1_parser_has_head(parser)) {
        guint8 digest[20];
        gsize digest_len = sizeof(digest);
        g_checksum_get_digest(sha1, digest, &digest_len);
        exchange->payload_digest = warc_label_sha1(digest);
        exchange->response       = spool;
        spool = NULL;
    }
    if (switched) {
        /* The handshake is recorded now; the websocket may stay open long. */
        if (exchange && exchange->response) {
            proxy_submit(proxy, exchange);
            *submitted = TRUE;
        }
        if (client_ok) {
            splice_upgraded(conn);
        }
    } else {
        *keep = client_ok && done && http1_parser_keep_alive(parser);
    }
out:
    /* The site's connection is good for another request only when it ended
     * cleanly where the response did. */
    if (result != RELAY_DONE || !*keep || conn->up_reader.start != conn->up_reader.end) {
        drop_upstream(conn);
    }
    spool_free(spool);
    g_checksum_free(sha1);
    http1_parser_free(parser);
    g_clear_error(&error);
    return result;
}

/* Passes one request from the browser to its site and the answer back.
 * Returns whether the browser's connection may carry another request. */
static gboolean serve_request(Conn *conn, Http1Parser *request, GByteArray *body,
        const Destination *dest)
{
    ArchiveProxy *proxy   = conn->proxy;
    const Http1Head *head = http1_parser_head(request);
    gboolean recording    = proxy_recording(proxy);
    gboolean upgrade      = http1_head_has_token(head, "Connection", "upgrade")
        && http1_head_get(head, "Upgrade");
    GString *out          = upstream_head(request, dest->target);
    gboolean keep         = FALSE;

    for (int attempt = 0; attempt < 2; attempt++) {
        GError *error = NULL;
        guint status;
        if (!connect_upstream(conn, dest, &status, &error)) {
            send_error(conn, status, status == 403 ? "Forbidden" : "Bad Gateway",
                    error ? error->message : NULL);
            g_debug("%s: %s", dest->uri, error ? error->message : "failed");
            g_clear_error(&error);
            break;
        }

        ArchiveExchange *exchange = NULL;
        if (recording) {
            exchange             = archive_exchange_new();
            exchange->target_uri = g_strdup(dest->uri);
            exchange->ip_address = g_strdup(conn->up_ip);
            exchange->date       = g_date_time_new_now_utc();
            exchange->request    = spool_new(proxy->tmp_dir, SPOOL_MEMORY);
            spool_append(exchange->request, out->str, out->len, NULL);
            spool_append(exchange->request, body->data, body->len, NULL);
        }

        gboolean reused = conn->up_reused;
        gboolean sent   = write_out(conn->up, out->str, out->len, conn->cancellable, &error)
            && (!body->len || write_out(conn->up, body->data, body->len, conn->cancellable,
                        &error));
        if (!sent) {
            archive_exchange_free(exchange);
            drop_upstream(conn);
            if (reused && attempt == 0 && !g_cancellable_is_cancelled(conn->cancellable)) {
                g_clear_error(&error);
                continue;
            }
            send_error(conn, 502, "Bad Gateway", error ? error->message : NULL);
            g_clear_error(&error);
            break;
        }

        gboolean submitted;
        RelayResult result = relay_response(conn, head->method, upgrade, exchange, &keep,
                &submitted);
        if (submitted) {
            exchange = NULL;
        }
        if (result == RELAY_STALE && attempt == 0) {
            archive_exchange_free(exchange);
            continue;
        }
        if (exchange && exchange->response) {
            g_debug("%s %s", head->method, dest->uri);
            proxy_submit(proxy, exchange);
        } else {
            archive_exchange_free(exchange);
        }
        if (result != RELAY_DONE) {
            keep = FALSE;
        }
        break;
    }
    g_string_free(out, TRUE);
    return keep && http1_parser_keep_alive(request);
}

/* ----------------------------------------------------------------------- */
/* Browser connections. */

static gboolean parse_authority(const char *authority, char **host, guint16 *port)
{
    const char *colon;
    guint64 n = 443;

    if (authority[0] == '[') {
        const char *close = strchr(authority, ']');
        if (!close) {
            return FALSE;
        }
        *host = g_strndup(authority + 1, close - authority - 1);
        colon = close[1] == ':' ? close + 1 : NULL;
    } else {
        colon = strrchr(authority, ':');
        *host = colon ? g_strndup(authority, colon - authority) : g_strdup(authority);
    }
    if ((colon && !g_ascii_string_to_unsigned(colon + 1, 10, 1, 65535, &n, NULL)) || !**host) {
        g_clear_pointer(host, g_free);
        return FALSE;
    }
    *port = (guint16)n;
    return TRUE;
}

/* Whether the next byte the browser sends opens a TLS handshake. */
static gboolean next_is_tls(Conn *conn)
{
    guint8 byte;
    GInputVector vector = {&byte, 1};
    gint flags          = G_SOCKET_MSG_PEEK;
    gssize n = g_socket_receive_message(g_socket_connection_get_socket(conn->client), NULL,
            &vector, 1, NULL, NULL, &flags, conn->cancellable, NULL);
    return n == 1 && byte == 0x16;
}

/* Serves a CONNECT: answers it, then -- with the site's certificate, when
 * the browser speaks TLS -- serves the requests that come through it. */
static void serve_tunnel(Conn *conn, const char *authority, Http1Parser *request,
        GByteArray *body)
{
    ArchiveProxy *proxy = conn->proxy;
    Destination tunnel  = {0};
    GError *error       = NULL;

    if (!parse_authority(authority, &tunnel.host, &tunnel.port)) {
        send_error(conn, 400, "Bad Request", "CONNECT needs a host and port.");
        return;
    }
    if (conn->reader.start != conn->reader.end) {
        g_debug("CONNECT %s: data before the tunnel was open", authority);
        goto out;
    }
    GTlsCertificate *cert = archive_ca_cert_for_host(proxy->ca, tunnel.host, &error);
    if (!cert) {
        send_error(conn, 502, "Bad Gateway", error->message);
        goto out;
    }
    if (!announce_certificate(proxy, tunnel.host, cert)) {
        send_error(conn, 503, "Service Unavailable",
                "The browser could not be told about the site's certificate.");
        g_object_unref(cert);
        goto out;
    }
    static const char open[] = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (!write_out(conn->stream, open, sizeof(open) - 1, conn->cancellable, NULL)) {
        g_object_unref(cert);
        goto out;
    }

    tunnel.tls = next_is_tls(conn);
    if (tunnel.tls) {
        GIOStream *tls = g_tls_server_connection_new(G_IO_STREAM(conn->client), cert, &error);
        if (!tls) {
            g_object_unref(cert);
            goto out;
        }
        const char *protocols[] = {"http/1.1", NULL};
        g_tls_connection_set_advertised_protocols(G_TLS_CONNECTION(tls), protocols);
        if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), conn->cancellable, &error)) {
            g_debug("CONNECT %s: TLS with the browser failed: %s", authority, error->message);
            g_object_unref(tls);
            g_object_unref(cert);
            goto out;
        }
        g_object_unref(conn->stream);
        conn->stream = tls;
        reader_init(&conn->reader, g_io_stream_get_input_stream(conn->stream));
    }
    g_object_unref(cert);

    while (read_request(conn, request, body, &error)) {
        Destination dest;
        if (!resolve_destination(http1_parser_head(request)->target, &tunnel, &dest, &error)) {
            send_error(conn, 400, "Bad Request", error->message);
            break;
        }
        gboolean keep = serve_request(conn, request, body, &dest);
        destination_clear(&dest);
        if (!keep) {
            break;
        }
    }
out:
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_debug("CONNECT %s: %s", authority, error->message);
    }
    g_clear_error(&error);
    destination_clear(&tunnel);
}

static Conn *conn_new(ArchiveProxy *proxy, GSocketConnection *client)
{
    Conn *conn        = g_new0(Conn, 1);
    conn->proxy       = proxy;
    conn->cancellable = g_cancellable_new();
    conn->client      = g_object_ref(client);
    conn->stream      = G_IO_STREAM(g_object_ref(client));
    g_socket_set_timeout(g_socket_connection_get_socket(client), CLIENT_TIMEOUT);
    reader_init(&conn->reader, g_io_stream_get_input_stream(conn->stream));

    g_mutex_lock(&proxy->lock);
    if (proxy->stopping) {
        g_cancellable_cancel(conn->cancellable);
    }
    g_hash_table_add(proxy->connections, conn);
    g_mutex_unlock(&proxy->lock);
    return conn;
}

static void conn_free(Conn *conn)
{
    ArchiveProxy *proxy = conn->proxy;

    drop_upstream(conn);
    reader_clear(&conn->up_reader);
    if (conn->stream != G_IO_STREAM(conn->client)) {
        g_io_stream_close(conn->stream, NULL, NULL);
    }
    g_object_unref(conn->stream);
    g_io_stream_close(G_IO_STREAM(conn->client), NULL, NULL);
    g_object_unref(conn->client);
    reader_clear(&conn->reader);

    g_mutex_lock(&proxy->lock);
    g_hash_table_remove(proxy->connections, conn);
    g_cond_broadcast(&proxy->cond);
    g_mutex_unlock(&proxy->lock);

    g_object_unref(conn->cancellable);
    g_free(conn);
}

static void serve_connection(gpointer data, gpointer user_data)
{
    GSocketConnection *client = data;
    ArchiveProxy *proxy       = user_data;

    if (proxy->check_peer_user && !archive_peer_is_same_user(client)) {
        g_message("archive: refused a connection from another user's process");
        g_io_stream_close(G_IO_STREAM(client), NULL, NULL);
        g_object_unref(client);
        return;
    }

    Conn *conn          = conn_new(proxy, client);
    Http1Parser *request = http1_parser_new(HTTP1_REQUEST, MAX_HEAD);
    GByteArray *body    = g_byte_array_new();
    GError *error       = NULL;

    while (read_request(conn, request, body, &error)) {
        const Http1Head *head = http1_parser_head(request);
        if (!g_ascii_strcasecmp(head->method, "CONNECT")) {
            serve_tunnel(conn, head->target, request, body);
            break;
        }
        Destination dest;
        if (!resolve_destination(head->target, NULL, &dest, &error)) {
            send_error(conn, 400, "Bad Request", error->message);
            break;
        }
        gboolean keep = serve_request(conn, request, body, &dest);
        destination_clear(&dest);
        if (!keep) {
            break;
        }
    }
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (g_error_matches(error, HTTP1_ERROR, HTTP1_ERROR_MALFORMED)
                || g_error_matches(error, HTTP1_ERROR, HTTP1_ERROR_TOO_LARGE)) {
            send_error(conn, 400, "Bad Request", error->message);
        }
        g_debug("connection: %s", error->message);
    }
    g_clear_error(&error);
    g_byte_array_free(body, TRUE);
    http1_parser_free(request);
    conn_free(conn);
    g_object_unref(client);
}

/* ----------------------------------------------------------------------- */

static gboolean on_incoming(GSocketService *service, GSocketConnection *connection,
        GObject *source, gpointer user_data)
{
    ArchiveProxy *proxy = user_data;
    g_thread_pool_push(proxy->pool, g_object_ref(connection), NULL);
    return TRUE;
}

static gpointer accept_run(gpointer data)
{
    ArchiveProxy *proxy      = data;
    GError *error            = NULL;
    GSocketAddress *bound    = NULL;

    g_main_context_push_thread_default(proxy->context);
    GSocketService *service  = g_socket_service_new();
    GInetAddress *loopback   = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address  = g_inet_socket_address_new(loopback, 0);
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), proxy);
    gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(service), address,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error);
    g_object_unref(address);
    g_object_unref(loopback);
    if (ok) {
        proxy->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
        g_object_unref(bound);
        g_socket_service_start(service);
    }

    g_mutex_lock(&proxy->lock);
    proxy->ready       = TRUE;
    proxy->start_error = error;
    g_cond_broadcast(&proxy->cond);
    g_mutex_unlock(&proxy->lock);

    if (ok) {
        g_main_loop_run(proxy->loop);
    }
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    g_object_unref(service);
    /* Stopping cancelled the accept in progress, which finishes -- letting go
     * of the context it holds -- only when the context runs once more. */
    while (g_main_context_pending(proxy->context)) {
        g_main_context_iteration(proxy->context, FALSE);
    }
    g_main_context_pop_thread_default(proxy->context);
    return NULL;
}

static void proxy_free(ArchiveProxy *proxy)
{
    if (proxy->pool) {
        g_thread_pool_free(proxy->pool, FALSE, TRUE);
    }
    archive_ca_free(proxy->ca);
    g_clear_pointer(&proxy->hosts, g_hash_table_unref);
    g_clear_pointer(&proxy->connections, g_hash_table_unref);
    g_clear_pointer(&proxy->allow_sources, g_ptr_array_unref);
    g_clear_pointer(&proxy->loop, g_main_loop_unref);
    g_clear_pointer(&proxy->context, g_main_context_unref);
    g_clear_pointer(&proxy->owner_context, g_main_context_unref);
    g_clear_error(&proxy->start_error);
    g_mutex_clear(&proxy->lock);
    g_cond_clear(&proxy->cond);
    g_free(proxy->tmp_dir);
    g_free(proxy);
}

ArchiveProxy *archive_proxy_start(const ArchiveProxyOptions *options, GError **error)
{
    /* Without a TLS backend (glib-networking), every https page would fail
     * as it loaded, saying only that its load was interrupted. */
    if (!g_tls_backend_supports_tls(g_tls_backend_get_default())) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "GIO has no TLS backend, so https can't be archived (install glib-networking)");
        return NULL;
    }
    ArchiveProxy *proxy      = g_new0(ArchiveProxy, 1);
    proxy->tmp_dir           = g_strdup(options->tmp_dir ? options->tmp_dir : g_get_tmp_dir());
    proxy->verify_upstream   = options->verify_upstream;
    proxy->allow_internal    = options->allow_internal;
    proxy->check_peer_user   = options->check_peer_user;
    proxy->max_record_size   = options->max_record_size ? options->max_record_size
        : DEFAULT_MAX_RECORD;
    proxy->allow_certificate = options->allow_certificate;
    proxy->allow_data        = options->allow_data;
    proxy->owner_context     = g_main_context_ref(options->owner_context
            ? options->owner_context : g_main_context_default());
    proxy->hosts             = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    proxy->connections       = g_hash_table_new(g_direct_hash, g_direct_equal);
    proxy->allow_sources     = g_ptr_array_new();
    g_mutex_init(&proxy->lock);
    g_cond_init(&proxy->cond);

    if (!(proxy->ca = archive_ca_open(options->ca_dir, error))) {
        proxy_free(proxy);
        return NULL;
    }
    proxy->pool = g_thread_pool_new(serve_connection, proxy, MAX_THREADS, FALSE, error);
    if (!proxy->pool) {
        proxy_free(proxy);
        return NULL;
    }
    if (options->warc_path) {
        proxy->recorder = archive_recorder_start(options->warc_path,
                options->software ? options->software : "vimb", error);
        if (!proxy->recorder) {
            proxy_free(proxy);
            return NULL;
        }
    }

    proxy->context = g_main_context_new();
    proxy->loop    = g_main_loop_new(proxy->context, FALSE);
    proxy->thread  = g_thread_new("archive-accept", accept_run, proxy);

    g_mutex_lock(&proxy->lock);
    while (!proxy->ready) {
        g_cond_wait(&proxy->cond, &proxy->lock);
    }
    g_mutex_unlock(&proxy->lock);
    if (proxy->start_error) {
        g_propagate_error(error, proxy->start_error);
        proxy->start_error = NULL;
        g_thread_join(proxy->thread);
        if (proxy->recorder) {
            archive_recorder_finish(proxy->recorder, NULL, NULL);
        }
        proxy_free(proxy);
        return NULL;
    }
    return proxy;
}

guint16 archive_proxy_get_port(ArchiveProxy *proxy)
{
    return proxy->port;
}

const char *archive_proxy_get_ca_file(ArchiveProxy *proxy)
{
    return archive_ca_file(proxy->ca);
}

void archive_proxy_set_paused(ArchiveProxy *proxy, gboolean paused)
{
    g_atomic_int_set(&proxy->paused, paused ? 1 : 0);
}

guint archive_proxy_get_recorded(ArchiveProxy *proxy)
{
    g_mutex_lock(&proxy->lock);
    guint count = proxy->recorder ? archive_recorder_count(proxy->recorder) : proxy->recorded;
    g_mutex_unlock(&proxy->lock);
    return count;
}

gboolean archive_proxy_finish_recording(ArchiveProxy *proxy, GError **error)
{
    g_mutex_lock(&proxy->lock);
    ArchiveRecorder *recorder = proxy->recorder;
    proxy->recorder = NULL;
    g_mutex_unlock(&proxy->lock);

    /* No exchange reaches it now (see proxy_submit()), so it is ours to end. */
    guint written = 0;
    gboolean ok   = !recorder || archive_recorder_finish(recorder, &written, error);
    if (recorder) {
        g_mutex_lock(&proxy->lock);
        proxy->recorded = written;
        g_mutex_unlock(&proxy->lock);
    }
    return ok;
}

/* Must be called from the thread of the owner's main context, so that no
 * certificate call is running on it meanwhile. */
gboolean archive_proxy_stop(ArchiveProxy *proxy, GError **error)
{
    GHashTableIter iter;
    gpointer conn;

    g_mutex_lock(&proxy->lock);
    g_atomic_int_set(&proxy->stopping, TRUE);
    g_hash_table_iter_init(&iter, proxy->connections);
    while (g_hash_table_iter_next(&iter, &conn, NULL)) {
        g_cancellable_cancel(((Conn *)conn)->cancellable);
    }
    /* Certificate calls not yet made won't be: their waiters are woken. */
    for (guint i = 0; i < proxy->allow_sources->len; i++) {
        g_source_destroy(g_ptr_array_index(proxy->allow_sources, i));
    }
    g_ptr_array_set_size(proxy->allow_sources, 0);
    g_cond_broadcast(&proxy->cond);
    g_mutex_unlock(&proxy->lock);

    g_main_loop_quit(proxy->loop);
    g_thread_join(proxy->thread);

    /* Connections still queued see that it's stopping, and end at once. */
    g_thread_pool_free(proxy->pool, FALSE, TRUE);
    proxy->pool = NULL;

    gboolean ok = archive_proxy_finish_recording(proxy, error);
    proxy_free(proxy);
    return ok;
}
