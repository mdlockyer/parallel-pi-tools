/*
 * parallel_lib.c — Shared library for FFI.
 * In-process C: libcurl + cJSON, zero spawn overhead.
 *
 * Thread safety: koffi async calls run on worker threads, so this library
 * must be safe for concurrent entry. Each request creates its own curl easy
 * handle, all attached to one lock-protected CURLSH so they share the
 * connection pool, DNS cache, and TLS session cache across calls.
 * Global init runs once via pthread_once; no other mutable global state is
 * touched per-request.
 *
 * Error reporting: instead of returning NULL (which loses diagnostics),
 * failing entry points return a malloc'd string beginning with
 * "PARALLEL_ERROR: " followed by a human-readable cause (HTTP status +
 * response excerpt, JSON-RPC error, transport error, ...). Callers (JS or
 * the parallel binary) detect the prefix and surface the rest as an error.
 *
 * Build as shared lib:
 *   macOS:  cc -O2 -shared -fPIC -o libparallel.dylib parallel_lib.c cJSON.c -lcurl
 *   Linux:  cc -O2 -shared -fPIC -o libparallel.so parallel_lib.c cJSON.c -lcurl -lpthread
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <curl/curl.h>
#include "cJSON.h"

extern char **environ;

/* ── debug ──────────────────────────────────────────────────── */

static int debug = 0;

static void dbg(const char *fmt, ...) {
    if (!debug) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ── error strings ──────────────────────────────────────────── */

#define ERROR_PREFIX "PARALLEL_ERROR:"

/* Format "PARALLEL_ERROR: <fmt>" into a malloc'd string. Returns NULL only
 * on OOM. All user-visible failures funnel through here. */
static char *vmake_error(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return NULL;
    size_t need = (size_t)n + sizeof(ERROR_PREFIX) + 1;
    char *out = malloc(need);
    if (!out) return NULL;
    memcpy(out, ERROR_PREFIX " ", sizeof(ERROR_PREFIX)); /* includes NUL */
    vsnprintf(out + sizeof(ERROR_PREFIX), (size_t)n + 1, fmt, ap);
    return out;
}

static char *make_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static char *make_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = vmake_error(fmt, ap);
    va_end(ap);
    return out;
}

/* ── reusable global init (thread-safe) ─────────────────────── */

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

/* Shared state for cross-call connection reuse. Easy handles are created
 * per-request (thread safety), but attaching them to one CURLSH lets them
 * share the connection pool, DNS cache, and TLS session cache.
 * Access to each shared data type is serialized by its own mutex — required
 * because koffi async calls arrive on multiple worker threads. */
static CURLSH *g_share = NULL;
static pthread_mutex_t g_share_locks[CURL_LOCK_DATA_LAST];

static void share_lock_cb(CURL *handle, curl_lock_data data,
                          curl_lock_access access, void *userptr) {
    (void)handle;
    (void)access;
    (void)userptr;
    pthread_mutex_lock(&g_share_locks[data]);
}

static void share_unlock_cb(CURL *handle, curl_lock_data data, void *userptr) {
    (void)handle;
    (void)userptr;
    pthread_mutex_unlock(&g_share_locks[data]);
}

static void parallel_init_impl(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    debug = getenv("PARALLEL_DEBUG") != NULL;

    g_share = curl_share_init();
    if (g_share) {
        for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
            pthread_mutex_init(&g_share_locks[i], NULL);
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock_cb);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    } else {
        dbg("parallel_init: curl_share unavailable, no connection reuse");
    }
    dbg("parallel_init: curl initialized");
}

/* ── curl write callback ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *buf = (Buffer *)userdata;
    size_t total = size * nmemb;
    if (total > SIZE_MAX - buf->len - 1) return 0; /* would overflow */
    size_t needed = buf->len + total + 1;
    if (needed > buf->cap) {
        if (needed > SIZE_MAX / 2) return 0;
        size_t ncap = needed * 2;
        char *nd = realloc(buf->data, ncap);
        if (!nd) return 0;
        buf->data = nd;
        buf->cap = ncap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ── curl header callback (capture Content-Type + Mcp-Session-Id) ── */

typedef struct {
    char content_type[256];
    char session_id[256];
} HeaderData;

/* Copy "<name>: value" (bounded by line length) into dst, trimming
 * surrounding whitespace. Everything is bounds-checked: header lines are
 * NOT NUL-terminated by libcurl. */
static void copy_header_value(char *dst, size_t dstsz,
                              const char *line, size_t linelen,
                              size_t namelen) {
    const char *val = line + namelen;
    const char *end = line + linelen;
    while (val < end && (*val == ' ' || *val == '\t')) val++;
    while (end > val &&
           (end[-1] == '\r' || end[-1] == '\n' ||
            end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t vlen = (size_t)(end - val);
    if (vlen >= dstsz) vlen = dstsz - 1;
    memcpy(dst, val, vlen);
    dst[vlen] = '\0';
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    HeaderData *hd = (HeaderData *)userdata;
    size_t total = size * nmemb;
    /* Bound the line at the first newline; never read past `total`. */
    const char *nl = memchr(ptr, '\n', total);
    size_t len = nl ? (size_t)((const char *)nl - ptr) : total;

    if (len > 13 && strncasecmp(ptr, "Content-Type:", 13) == 0) {
        copy_header_value(hd->content_type, sizeof(hd->content_type), ptr, len, 13);
    } else if (len > 15 && strncasecmp(ptr, "Mcp-Session-Id:", 15) == 0) {
        copy_header_value(hd->session_id, sizeof(hd->session_id), ptr, len, 15);
    }
    return total;
}

/* ── HTTP POST (per-call easy handle — thread-safe) ─────────── */

typedef struct {
    char *body;              /* malloc'd, NUL-terminated; NULL on transport error/OOM */
    size_t body_len;
    long status;             /* HTTP status, 0 if the exchange never happened */
    CURLcode curl_rc;        /* CURLE_OK on success */
    char content_type[256];
    char session_id[256];
} HttpResult;

static HttpResult http_post_ex(const char *url, const char *body,
                               const char *api_key, const char *accept,
                               const char *session_id) {
    HttpResult res;
    memset(&res, 0, sizeof(res));

    /* Fresh handle per call (koffi async runs on worker threads), attached
     * to the shared connection cache so repeat calls reuse pooled
     * connections instead of re-handshaking TLS every time. */
    CURL *curl = curl_easy_init();
    if (!curl) { res.curl_rc = CURLE_FAILED_INIT; return res; }
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);

    Buffer buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); res.curl_rc = CURLE_OUT_OF_MEMORY; return res; }
    buf.data[0] = '\0';

    HeaderData hd;
    memset(&hd, 0, sizeof(hd));

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    /* Dynamically sized headers: never truncate long API keys. */
    if (api_key && *api_key) {
        size_t n = strlen(api_key) + 32;
        char *auth = malloc(n);
        if (auth) {
            snprintf(auth, n, "x-api-key: %s", api_key);
            hdrs = curl_slist_append(hdrs, auth);
            free(auth);
        }
    }
    if (accept && *accept) {
        size_t n = strlen(accept) + 16;
        char *h = malloc(n);
        if (h) {
            snprintf(h, n, "Accept: %s", accept);
            hdrs = curl_slist_append(hdrs, h);
            free(h);
        }
    }
    if (session_id && *session_id) {
        size_t n = strlen(session_id) + 32;
        char *h = malloc(n);
        if (h) {
            snprintf(h, n, "Mcp-Session-Id: %s", session_id);
            hdrs = curl_slist_append(hdrs, h);
            free(h);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hd);

    res.curl_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    if (res.curl_rc != CURLE_OK)
        dbg("curl perform: res=%d status=%ld (%s)", (int)res.curl_rc,
            res.status, curl_easy_strerror(res.curl_rc));

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res.curl_rc != CURLE_OK) {
        free(buf.data);
        return res;
    }
    res.body = buf.data;
    res.body_len = buf.len;
    memcpy(res.content_type, hd.content_type, sizeof(res.content_type));
    memcpy(res.session_id, hd.session_id, sizeof(res.session_id));
    return res;
}

/* ── string builder (length-aware, allocation-failure sticky) ─ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int oom;   /* sticky: once set, all further appends are no-ops */
} StringBuilder;

static void sb_init_cap(StringBuilder *sb, size_t cap) {
    if (cap < 256) cap = 256;
    sb->cap = cap;
    sb->len = 0;
    sb->oom = 0;
    sb->data = malloc(sb->cap);
    if (!sb->data) sb->oom = 1;
    else sb->data[0] = '\0';
}

static void sb_init(StringBuilder *sb) {
    sb_init_cap(sb, 4096);
}

static int sb_check(StringBuilder *sb, size_t need) {
    if (sb->oom || !sb->data) return 0;
    if (need > SIZE_MAX - sb->len - 1) { sb->oom = 1; return 0; }
    if (sb->len + need + 1 <= sb->cap) return 1;
    size_t ncap = sb->cap;
    while (ncap < sb->len + need + 1) {
        if (ncap > SIZE_MAX / 2) { sb->oom = 1; return 0; }
        ncap *= 2;
    }
    char *nd = realloc(sb->data, ncap);
    if (!nd) { sb->oom = 1; return 0; }  /* data left valid & consistent */
    sb->data = nd;
    sb->cap = ncap;
    return 1;
}

#define SB_LIT(sb, lit) sb_append_n(sb, lit, sizeof(lit) - 1)

static inline void sb_append_n(StringBuilder *sb, const char *s, size_t n) {
    if (!sb_check(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static inline void sb_append(StringBuilder *sb, const char *s) {
    sb_append_n(sb, s, strlen(s));
}

static inline void sb_append_char(StringBuilder *sb, char c) {
    if (!sb_check(sb, 1)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

/* JSON-escape a string directly into the builder. */
static inline void sb_append_json_escaped(StringBuilder *sb, const char *s) {
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        switch (c) {
            case '"':  SB_LIT(sb, "\\\""); break;
            case '\\': SB_LIT(sb, "\\\\"); break;
            case '\n': SB_LIT(sb, "\\n"); break;
            case '\r': SB_LIT(sb, "\\r"); break;
            case '\t': SB_LIT(sb, "\\t"); break;
            case '\b': SB_LIT(sb, "\\b"); break;
            case '\f': SB_LIT(sb, "\\f"); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    // \u00XX — 6 chars
                    esc[0]='\\'; esc[1]='u'; esc[2]='0'; esc[3]='0';
                    const char hex[] = "0123456789abcdef";
                    esc[4]=hex[c>>4]; esc[5]=hex[c & 0xF]; esc[6]='\0';
                    sb_append_n(sb, esc, 6);
                } else {
                    sb_append_char(sb, (char)c);
                }
                break;
        }
    }
}

static inline char *sb_detach(StringBuilder *sb) {
    if (sb->oom || !sb->data) return NULL;
    sb->data[sb->len] = '\0';
    return sb->data;
}

/* ── formatters ──────────────────────────────────────────────── */

static void format_search(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);
        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            SB_LIT(sb, "### ");
            sb_append(sb, title->valuestring);
            SB_LIT(sb, "\n");
        }
        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            SB_LIT(sb, "URL: ");
            sb_append(sb, url->valuestring);
            SB_LIT(sb, "\n\n");
        }
        cJSON *excerpts = cJSON_GetObjectItem(r, "excerpts");
        if (cJSON_IsArray(excerpts)) {
            int m = cJSON_GetArraySize(excerpts);
            for (int j = 0; j < m; j++) {
                cJSON *e = cJSON_GetArrayItem(excerpts, j);
                if (cJSON_IsString(e)) {
                    SB_LIT(sb, "> ");
                    sb_append(sb, e->valuestring);
                    if (j < m - 1) SB_LIT(sb, "\n\n");
                }
            }
        }
        if (i < n - 1) SB_LIT(sb, "\n\n---\n\n");
    }
}

static void format_extract(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);
        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            SB_LIT(sb, "### ");
            sb_append(sb, title->valuestring);
            SB_LIT(sb, "\n");
        }
        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            SB_LIT(sb, "URL: ");
            sb_append(sb, url->valuestring);
            SB_LIT(sb, "\n\n");
        }
        cJSON *full = cJSON_GetObjectItem(r, "full_content");
        if (cJSON_IsString(full) && full->valuestring[0]) {
            sb_append(sb, full->valuestring);
        } else {
            cJSON *excerpts = cJSON_GetObjectItem(r, "excerpts");
            if (cJSON_IsArray(excerpts)) {
                int m = cJSON_GetArraySize(excerpts);
                for (int j = 0; j < m; j++) {
                    cJSON *e = cJSON_GetArrayItem(excerpts, j);
                    if (cJSON_IsString(e)) {
                        sb_append(sb, e->valuestring);
                        if (j < m - 1) SB_LIT(sb, "\n\n");
                    }
                }
            }
        }
        if (i < n - 1) SB_LIT(sb, "\n\n---\n\n");
    }
}

/* ── API key detection ──────────────────────────────────────── */

/* Mirror of the JS pattern in lib/web-search/parallel.mjs:
 * ^PARALLEL[_-]?(?:API[_-]?)?(?:KEY|SECRET|TOKEN)$ (case-insensitive).
 * Single source of truth for the shape; no hand-maintained list. */
static int ci_starts_with(const char **p, const char *lit) {
    size_t n = strlen(lit);
    if (strncasecmp(*p, lit, n) != 0) return 0;
    *p += n;
    return 1;
}

static int is_parallel_key_name(const char *name) {
    const char *p = name;
    if (!ci_starts_with(&p, "PARALLEL")) return 0;
    if (*p == '_' || *p == '-') p++;
    if (ci_starts_with(&p, "API")) {
        if (*p == '_' || *p == '-') p++;
    }
    if (!(ci_starts_with(&p, "KEY") ||
          ci_starts_with(&p, "SECRET") ||
          ci_starts_with(&p, "TOKEN"))) return 0;
    return *p == '\0';
}

static char *try_env(const char *name) {
    const char *val = getenv(name);
    if (!val || !*val) return NULL;
    while (*val == ' ' || *val == '\t') val++;
    if (!*val) return NULL;
    const char *end = val + strlen(val) - 1;
    while (end > val && (*end == ' ' || *end == '\t')) end--;
    size_t len = (size_t)(end - val + 1);
    char *res = malloc(len + 1);
    if (!res) return NULL;
    memcpy(res, val, len);
    res[len] = '\0';
    return res;
}

char *parallel_find_key(void) {
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        size_t nlen = (size_t)(eq - *e);
        char name[128];
        if (nlen == 0 || nlen >= sizeof(name)) continue;
        memcpy(name, *e, nlen);
        name[nlen] = '\0';
        if (!is_parallel_key_name(name)) continue;
        char *v = try_env(name);
        if (v) {
            dbg("parallel_find_key: found %s", name);
            return v;
        }
    }
    dbg("parallel_find_key: no key found");
    return NULL;
}

/* ── core: fetch + parse + format ────────────────────────────── */

static char *do_request(const char *api_base, const char *api_key,
                        const char *endpoint, const char *req_body,
                        int is_search) {
    // Build URL: api_base + "/" + endpoint. Trim trailing slashes so a base
    // like "https://api.parallel.ai/v1/" doesn't yield "//search".
    size_t base_len = strlen(api_base);
    while (base_len > 0 && api_base[base_len - 1] == '/') base_len--;
    size_t ep_len = strlen(endpoint);
    size_t url_len = base_len + 1 + ep_len;
    char stack_url[1024];
    char *url = stack_url;
    int heap = 0;
    if (url_len >= sizeof(stack_url)) {
        url = malloc(url_len + 1);
        if (!url) return make_error("out of memory");
        heap = 1;
    }
    memcpy(url, api_base, base_len);
    url[base_len] = '/';
    memcpy(url + base_len + 1, endpoint, ep_len + 1);

    HttpResult hr = http_post_ex(url, req_body, api_key, NULL, NULL);
    if (heap) free(url);

    if (hr.curl_rc != CURLE_OK) {
        free(hr.body);
        return make_error("request failed: %s", curl_easy_strerror(hr.curl_rc));
    }
    if (hr.status >= 400) {
        /* Surface status + response excerpt so 401 vs 429 vs 400 are
         * distinguishable by the caller. */
        size_t elen = hr.body_len < 1500 ? hr.body_len : 1500;
        char *err = make_error("HTTP %ld: %.*s", hr.status, (int)elen,
                               hr.body ? hr.body : "");
        free(hr.body);
        return err;
    }
    if (!hr.body) return make_error("empty response body");

    cJSON *root = cJSON_Parse(hr.body);
    free(hr.body);
    if (!root) return make_error("invalid JSON in response");

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        return make_error("unexpected response shape (missing results array)");
    }

    StringBuilder sb;
    sb_init(&sb);
    if (sb.oom) { cJSON_Delete(root); return make_error("out of memory"); }

    if (is_search) format_search(results, &sb);
    else           format_extract(results, &sb);

    cJSON_Delete(root);

    char *out = sb_detach(&sb);
    if (!out) return make_error("out of memory formatting output");
    return out;
}

/* ── init ───────────────────────────────────────────────────── */

void parallel_init(void) {
    pthread_once(&g_init_once, parallel_init_impl);
}

/* ── MCP protocol ─────────────────────────────────────────────── */

#define DEFAULT_MCP_URL "https://search.parallel.ai/mcp"
#define MCP_ACCEPT "application/json, text/event-stream"
#define MCP_PROTOCOL_VERSION "2025-06-18"

/* Build a JSON-RPC 2.0 request. params_json is embedded verbatim (or {}). */
static char *build_mcp_request(const char *method, long id,
                               const char *params_json) {
    StringBuilder sb;
    sb_init(&sb);
    if (sb.oom) return NULL;

    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%ld", id);

    SB_LIT(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_append(&sb, idbuf);
    SB_LIT(&sb, ",\"method\":\"");
    sb_append_json_escaped(&sb, method);
    SB_LIT(&sb, "\",\"params\":");
    if (params_json && *params_json) sb_append(&sb, params_json);
    else SB_LIT(&sb, "{}");
    SB_LIT(&sb, "}");
    return sb_detach(&sb);
}

/* Build a JSON-RPC 2.0 notification (no id). */
static char *build_mcp_notification(const char *method, const char *params_json) {
    StringBuilder sb;
    sb_init(&sb);
    if (sb.oom) return NULL;

    SB_LIT(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"");
    sb_append_json_escaped(&sb, method);
    SB_LIT(&sb, "\",\"params\":");
    if (params_json && *params_json) sb_append(&sb, params_json);
    else SB_LIT(&sb, "{}");
    SB_LIT(&sb, "}");
    return sb_detach(&sb);
}

/* Build tools/call params: {"name":"<esc>","arguments":<raw|{}>} */
static char *build_tools_call_params(const char *tool_name,
                                     const char *arguments_json) {
    StringBuilder sb;
    sb_init(&sb);
    if (sb.oom) return NULL;

    SB_LIT(&sb, "{\"name\":\"");
    sb_append_json_escaped(&sb, tool_name);
    SB_LIT(&sb, "\",\"arguments\":");
    if (arguments_json && *arguments_json) sb_append(&sb, arguments_json);
    else SB_LIT(&sb, "{}");
    SB_LIT(&sb, "}");
    return sb_detach(&sb);
}

static char *build_initialize_params(void) {
    StringBuilder sb;
    sb_init(&sb);
    if (sb.oom) return NULL;
    SB_LIT(&sb, "{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
                "\"capabilities\":{},"
                "\"clientInfo\":{\"name\":\"parallel-pi-tools\",\"version\":\"1.0.0\"}}");
    return sb_detach(&sb);
}

/* Extract text from MCP result.content[{type:"text",text:"..."}].
 * Returns a malloc'd string, or NULL if there is no text content. */
static char *extract_mcp_text(cJSON *result) {
    cJSON *content = cJSON_GetObjectItem(result, "content");
    if (!cJSON_IsArray(content)) return NULL;

    int n = cJSON_GetArraySize(content);
    if (n == 0) return NULL;

    /* First pass: measure total length */
    size_t total = 0;
    int text_count = 0;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(content, i);
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
            cJSON_IsString(text) && text->valuestring[0]) {
            if (text_count > 0) total += 2;
            total += strlen(text->valuestring);
            text_count++;
        }
    }
    if (text_count == 0) return NULL;

    /* Second pass: copy */
    char *out = malloc(total + 1);
    if (!out) return NULL;
    char *p = out;
    int idx = 0;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(content, i);
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
            cJSON_IsString(text) && text->valuestring[0]) {
            if (idx > 0) { *p++ = '\n'; *p++ = '\n'; }
            size_t len = strlen(text->valuestring);
            memcpy(p, text->valuestring, len);
            p += len;
            idx++;
        }
    }
    *p = '\0';
    return out;
}

/* Build "PARALLEL_ERROR: MCP error <code>: <message>" from a JSON-RPC error. */
static char *mcp_error_str(cJSON *error) {
    cJSON *code = cJSON_GetObjectItem(error, "code");
    cJSON *msg = cJSON_GetObjectItem(error, "message");
    const char *m = (cJSON_IsString(msg) && msg->valuestring[0])
                        ? msg->valuestring : "unknown error";
    if (cJSON_IsNumber(code))
        return make_error("MCP error %d: %s", (int)code->valuedouble, m);
    return make_error("MCP error: %s", m);
}

/* Parse SSE stream: scan "data: " lines for a JSON-RPC response carrying
 * usable text. Returns text on success, a PARALLEL_ERROR string when the
 * stream carried a JSON-RPC error, or NULL if nothing was usable.
 * (The old version deleted the parsed message twice when a result had no
 * text — a heap-use-after-free.) */
static char *parse_sse_result(const char *body) {
    char *err_detail = NULL;
    const char *p = body;
    while (p && *p) {
        if (p == body || *(p - 1) == '\n') {
            if (strncmp(p, "data: ", 6) == 0) {
                const char *json_start = p + 6;
                const char *eol = strchr(json_start, '\n');
                size_t jlen = eol ? (size_t)(eol - json_start) : strlen(json_start);
                while (jlen > 0 && json_start[jlen - 1] == '\r') jlen--;

                char *json_str = malloc(jlen + 1);
                if (!json_str) { free(err_detail); return NULL; }
                memcpy(json_str, json_start, jlen);
                json_str[jlen] = '\0';

                cJSON *msg = cJSON_Parse(json_str);
                free(json_str);
                if (msg) {
                    cJSON *result = cJSON_GetObjectItem(msg, "result");
                    if (result) {
                        char *text = extract_mcp_text(result);
                        if (text) {
                            cJSON_Delete(msg);      /* deleted exactly once */
                            free(err_detail);
                            return text;
                        }
                    }
                    if (!err_detail) {
                        cJSON *error = cJSON_GetObjectItem(msg, "error");
                        if (error) err_detail = mcp_error_str(error);
                    }
                    cJSON_Delete(msg);              /* single delete site */
                }
            }
        }
        p++;
    }
    return err_detail; /* NULL if no error captured either */
}

/* Parse regular JSON-RPC response. Returns extracted text, a
 * PARALLEL_ERROR string on a protocol error, or NULL on parse failure. */
static char *parse_mcp_json(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return NULL;

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        char *err = mcp_error_str(error);
        cJSON_Delete(root);
        return err;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result) { cJSON_Delete(root); return NULL; }

    char *text = extract_mcp_text(result);
    cJSON_Delete(root);
    return text;
}

/* Generic MCP tool call over the streamable-HTTP transport.
 * Performs the initialize handshake first (spec-compliant servers reply
 * with an Mcp-Session-Id that must accompany subsequent calls); fully
 * stateless servers ignore it and everything degrades to a plain
 * tools/call, matching the previous behaviour.
 * Returns extracted text, or a PARALLEL_ERROR string. */
static char *mcp_call(const char *mcp_url, const char *tool_name,
                      const char *arguments_json) {
    parallel_init();

    const char *url = (mcp_url && *mcp_url) ? mcp_url : DEFAULT_MCP_URL;
    char session[sizeof(((HttpResult *)0)->session_id)] = "";

    /* Step 1: initialize. Best-effort — a rejection here doesn't preclude
     * the server accepting tools/call directly. */
    char *init_params = build_initialize_params();
    char *req = init_params ? build_mcp_request("initialize", 1, init_params) : NULL;
    free(init_params);
    if (req) {
        HttpResult hr = http_post_ex(url, req, NULL, MCP_ACCEPT, NULL);
        free(req);
        if (hr.curl_rc == CURLE_OK && hr.session_id[0]) {
            memcpy(session, hr.session_id, sizeof(session));
            /* Step 1b: initialized notification (required by the spec once
             * a session exists). Response is intentionally ignored. */
            char *note = build_mcp_notification("notifications/initialized", "{}");
            if (note) {
                HttpResult hn = http_post_ex(url, note, NULL, MCP_ACCEPT, session);
                free(note);
                free(hn.body);
            }
        }
        free(hr.body);
        dbg("MCP initialize: status=%ld session=%s", hr.status,
            session[0] ? session : "(none)");
    } else {
        dbg("MCP initialize: skipped (could not build request)");
    }

    /* Step 2: tools/call */
    char *params = build_tools_call_params(tool_name, arguments_json);
    if (!params) return make_error("out of memory");
    char *req2 = build_mcp_request("tools/call", 2, params);
    free(params);
    if (!req2) return make_error("out of memory");

    dbg("MCP call: url=%s tool=%s", url, tool_name);
    HttpResult tr = http_post_ex(url, req2, NULL, MCP_ACCEPT,
                                 session[0] ? session : NULL);
    free(req2);

    if (tr.curl_rc != CURLE_OK) {
        char *err = make_error("request failed: %s",
                               curl_easy_strerror(tr.curl_rc));
        free(tr.body);
        return err;
    }
    if (tr.status >= 400) {
        size_t elen = tr.body_len < 1500 ? tr.body_len : 1500;
        char *err = make_error("HTTP %ld: %.*s", tr.status, (int)elen,
                               tr.body ? tr.body : "");
        free(tr.body);
        return err;
    }
    if (!tr.body) return make_error("empty response body from MCP endpoint");

    char *text;
    if (strstr(tr.content_type, "text/event-stream") != NULL) {
        dbg("MCP call: parsing SSE response");
        text = parse_sse_result(tr.body);
    } else {
        dbg("MCP call: parsing JSON response");
        text = parse_mcp_json(tr.body);
    }
    free(tr.body);

    if (!text) return make_error("no usable response from MCP endpoint");
    return text;
}

/* ── build request bodies (dynamic, no fixed limits) ─────────── */

static char *build_search_body_sb(const char *objective, const char *search_queries_json) {
    // {"objective":"<esc>","search_queries":<json>}
    // search_queries_json is expected to be a JSON array string like '["a","b"]'.
    // If NULL/empty, synthesize ["objective"].
    StringBuilder sb;
    size_t est = strlen(objective) * 2 + 64;
    if (search_queries_json) est += strlen(search_queries_json);
    sb_init_cap(&sb, est);
    if (sb.oom) return NULL;

    SB_LIT(&sb, "{\"objective\":\"");
    sb_append_json_escaped(&sb, objective);
    SB_LIT(&sb, "\",\"search_queries\":");
    if (search_queries_json && *search_queries_json) {
        sb_append(&sb, search_queries_json);
    } else {
        SB_LIT(&sb, "[\"");
        sb_append_json_escaped(&sb, objective);
        SB_LIT(&sb, "\"]");
    }
    SB_LIT(&sb, "}");
    return sb_detach(&sb);
}

static char *build_extract_body_sb(const char *urls_json, const char *objective) {
    // {"urls":<json>[,"objective":"<esc>"]}
    StringBuilder sb;
    size_t est = 32;
    if (urls_json) est += strlen(urls_json);
    if (objective && *objective) est += strlen(objective) * 2 + 32;
    sb_init_cap(&sb, est);
    if (sb.oom) return NULL;

    SB_LIT(&sb, "{\"urls\":");
    if (urls_json && *urls_json) sb_append(&sb, urls_json);
    else SB_LIT(&sb, "[]");

    if (objective && *objective) {
        SB_LIT(&sb, ",\"objective\":\"");
        sb_append_json_escaped(&sb, objective);
        SB_LIT(&sb, "\"");
    }
    SB_LIT(&sb, "}");
    return sb_detach(&sb);
}

/* ── public API — full surface (JSON arrays, no CSV, no limits) ─ */

char *parallel_search(const char *api_base, const char *api_key,
                      const char *objective, const char *search_queries_json) {
    parallel_init();
    char *body = build_search_body_sb(objective, search_queries_json);
    if (!body) return make_error("out of memory");
    char *res = do_request(api_base, api_key, "search", body, 1);
    free(body);
    return res;
}

char *parallel_extract(const char *api_base, const char *api_key,
                       const char *urls_json, const char *objective) {
    parallel_init();
    char *body = build_extract_body_sb(urls_json, objective);
    if (!body) return make_error("out of memory");
    char *res = do_request(api_base, api_key, "extract", body, 0);
    free(body);
    return res;
}

/* ── public MCP API ──────────────────────────────────────────── */

/* MCP web_search: free fallback when no API key is set.
 * objective: natural language search objective
 * search_queries_json: JSON array of queries (NULL to default to [objective])
 * mcp_url: MCP endpoint URL (NULL for default)
 * Returns extracted text, or a PARALLEL_ERROR string. Caller must parallel_free(). */
char *parallel_mcp_search(const char *objective, const char *search_queries_json,
                          const char *mcp_url) {
    char *args = build_search_body_sb(objective, search_queries_json);
    if (!args) return make_error("out of memory");
    char *text = mcp_call(mcp_url, "web_search", args);
    free(args);
    return text;
}

/* MCP web_fetch: free fallback when no API key is set.
 * urls_json: JSON array of URLs
 * objective: optional focus objective (NULL or empty to omit)
 * mcp_url: MCP endpoint URL (NULL for default)
 * Returns extracted text, or a PARALLEL_ERROR string. Caller must parallel_free(). */
char *parallel_mcp_fetch(const char *urls_json, const char *objective,
                         const char *mcp_url) {
    char *args = build_extract_body_sb(urls_json, objective);
    if (!args) return make_error("out of memory");
    char *text = mcp_call(mcp_url, "web_fetch", args);
    free(args);
    return text;
}

void parallel_free(char *ptr) {
    free(ptr);
}

/* Teardown-only: like curl_global_cleanup, this must not race in-flight
 * calls. The shared cache is released only if no easy handle is still
 * attached; otherwise the OS reclaims everything at exit. */
void parallel_cleanup(void) {
    if (g_share && curl_share_cleanup(g_share) == CURLSHE_OK) {
        g_share = NULL;
        for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
            pthread_mutex_destroy(&g_share_locks[i]);
    }
    curl_global_cleanup();
}
