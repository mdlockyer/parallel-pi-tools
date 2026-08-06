/*
 * parallel_lib.c — Shared library for FFI.
 * Exposes parallel_search() and parallel_extract() as plain C functions.
 * Caller is responsible for calling parallel_free() on the result.
 *
 * Build as shared lib:
 *   macOS:  cc -O2 -shared -fPIC -o libparallel.dylib parallel_lib.c cJSON.c -lcurl
 *   Linux:  cc -O2 -shared -fPIC -o libparallel.so parallel_lib.c cJSON.c -lcurl
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include "cJSON.h"

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

/* ── forward declarations ───────────────────────────────────── */

char *parallel_search(const char *api_base, const char *api_key, const char *objective);
char *parallel_extract(const char *api_base, const char *api_key, const char *urls_csv, const char *objective);

/* ── reusable curl handle + headers ─────────────────────────── */

static CURL *g_curl = NULL;
static struct curl_slist *g_headers_json = NULL;   /* Content-Type only */
static struct curl_slist *g_headers_auth = NULL;    /* Content-Type + x-api-key */

static char g_current_key[256] = {0};

static void ensure_curl(void) {
    if (!g_curl) {
        g_curl = curl_easy_init();
        /* Pre-set options that never change */
        curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, NULL); /* set per-call */
        curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);        /* skip signal handlers */
        curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);    /* reuse connections */
        curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPIDLE, 120L);

        g_headers_json = curl_slist_append(NULL, "Content-Type: application/json");
    }
}

static struct curl_slist *get_headers(const char *api_key) {
    if (!api_key || !*api_key) return g_headers_json;

    /* Rebuild auth header only when key changes */
    if (strcmp(api_key, g_current_key) != 0) {
        if (g_headers_auth) curl_slist_free_all(g_headers_auth);
        char auth[512];
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
        g_headers_auth = curl_slist_append(
            curl_slist_append(NULL, "Content-Type: application/json"), auth);
        snprintf(g_current_key, sizeof(g_current_key), "%s", api_key);
    }
    return g_headers_auth;
}

/* ── curl write callback ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    Buffer *buf = (Buffer *)userdata;
    if (buf->len + total + 1 > buf->cap) {
        buf->cap = (buf->len + total + 1) * 2;
        buf->data = realloc(buf->data, buf->cap);
        if (!buf->data) return 0;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ── HTTP POST ───────────────────────────────────────────────── */

static char *http_post(const char *url, const char *body, const char *api_key) {
    ensure_curl();

    Buffer buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    buf.data[0] = '\0';

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, get_headers(api_key));
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &buf);

    /* Reset connection reuse for different hosts */
    curl_easy_setopt(g_curl, CURLOPT_FRESH_CONNECT, 0L);

    CURLcode res = curl_easy_perform(g_curl);
    long status = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);

    dbg("curl perform: res=%d status=%ld", (int)res, status);
    if (res != CURLE_OK) {
        dbg("curl error: %s", curl_easy_strerror(res));
    }

    if (res != CURLE_OK || status >= 400) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── string builder (length-aware, no repeated strlen) ──────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->cap = 4096;
    sb->data = malloc(sb->cap);
    sb->len = 0;
}

static inline void sb_append_n(StringBuilder *sb, const char *s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = (sb->len + n + 1) * 2;
        sb->data = realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

/* Compile-time strlen for string literals */
#define SB_LIT(sb, lit) sb_append_n(sb, lit, sizeof(lit) - 1)

static inline void sb_append(StringBuilder *sb, const char *s) {
    sb_append_n(sb, s, strlen(s));
}

static inline char *sb_detach(StringBuilder *sb) {
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

static char *try_env(const char *name) {
    const char *val = getenv(name);
    if (!val || !*val) return NULL;
    while (*val == ' ' || *val == '\t') val++;
    if (!*val) return NULL;
    const char *end = val + strlen(val) - 1;
    while (end > val && (*end == ' ' || *end == '\t')) end--;
    size_t len = end - val + 1;
    char *result = malloc(len + 1);
    memcpy(result, val, len);
    result[len] = '\0';
    return result;
}

char *parallel_find_key(void) {
    static const char *candidates[] = {
        "PARALLEL_API_KEY", "PARALLEL_KEY", "PARALLELSECRET",
        "PARALLEL_TOKEN", "PARALLEL-API-KEY",
        "parallel_api_key", "parallel_key", "parallelsecret",
        "parallel_token", "parallel-api-key",
        "Parallel_Key", "Parallel_Token",
        NULL
    };
    for (const char **c = candidates; *c; c++) {
        char *val = try_env(*c);
        if (val) {
            dbg("parallel_find_key: found %s", *c);
            return val;
        }
    }
    dbg("parallel_find_key: no key found");
    return NULL;
}

/* ── core: fetch + parse + format ────────────────────────────── */

static char *do_request(const char *api_base, const char *api_key,
                        const char *endpoint, const char *req_body,
                        int is_search) {
    /* Build URL on stack — no malloc */
    char url[1024];
    int base_len = strlen(api_base);
    int ep_len = strlen(endpoint);
    if (base_len + 1 + ep_len >= (int)sizeof(url)) return NULL;
    memcpy(url, api_base, base_len);
    url[base_len] = '/';
    memcpy(url + base_len + 1, endpoint, ep_len + 1);

    char *resp = http_post(url, req_body, api_key);
    if (!resp) return NULL;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return NULL;

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) { cJSON_Delete(root); return NULL; }

    StringBuilder sb;
    sb_init(&sb);

    if (is_search) format_search(results, &sb);
    else           format_extract(results, &sb);

    cJSON_Delete(root);
    return sb_detach(&sb);
}

/* ── init (call once) ───────────────────────────────────────── */

static int initialized = 0;

void parallel_init(void) {
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (getenv("PARALLEL_DEBUG")) debug = 1;
        dbg("parallel_init: curl initialized");
        initialized = 1;
    }
}

/* ── build request JSON without cJSON overhead ───────────────── */

static char *build_search_body(const char *objective, char *buf, size_t bufsz) {
    /* {"objective":"...","search_queries":["..."]} — API requires search_queries */
    int n = snprintf(buf, bufsz, "{\"objective\":\"%s\",\"search_queries\":[\"%s\"]}", objective, objective);
    return (n > 0 && (size_t)n < bufsz) ? buf : NULL;
}

static char *build_extract_body(const char *urls_csv, const char *objective,
                                char *buf, size_t bufsz) {
    char *p = buf;
    char *end = buf + bufsz - 1;

    /* Open brace + urls array open */
    int n = snprintf(p, end - p, "{\"urls\":[");
    if (n < 0 || p + n >= end) return NULL;
    p += n;

    /* Split urls_csv on comma, wrap each in quotes */
    const char *tok = urls_csv;
    int first = 1;
    while (*tok) {
        const char *comma = strchr(tok, ',');
        size_t tlen = comma ? (size_t)(comma - tok) : strlen(tok);
        if (!first) { *p++ = ','; }
        *p++ = '"';
        if (p + tlen >= end) return NULL;
        memcpy(p, tok, tlen);
        p += tlen;
        *p++ = '"';
        first = 0;
        tok += tlen;
        if (comma) tok++; /* skip comma */
    }

    /* Close urls array */
    n = snprintf(p, end - p, "]");
    if (n < 0 || p + n >= end) return NULL;
    p += n;

    /* Optional objective */
    if (objective && *objective) {
        n = snprintf(p, end - p, ",\"objective\":\"%s\"", objective);
        if (n < 0 || p + n >= end) return NULL;
        p += n;
    }

    *p++ = '}';
    *p = '\0';
    return buf;
}

/* ── convenience: search with auto-key ─────────────────────── */

char *parallel_search_auto(const char *api_base, const char *objective) {
    parallel_init();
    char *key = parallel_find_key();
    char *result = parallel_search(api_base, key ? key : "", objective);
    free(key);
    return result;
}

char *parallel_extract_auto(const char *api_base, const char *urls_csv, const char *objective) {
    parallel_init();
    char *key = parallel_find_key();
    char *result = parallel_extract(api_base, key ? key : "", urls_csv, objective);
    free(key);
    return result;
}

/* ── public API ──────────────────────────────────────────────── */

char *parallel_search(const char *api_base, const char *api_key, const char *objective) {
    parallel_init();
    char body[4096];
    if (!build_search_body(objective, body, sizeof(body))) return NULL;
    return do_request(api_base, api_key, "search", body, 1);
}

char *parallel_extract(const char *api_base, const char *api_key,
                       const char *urls_csv, const char *objective) {
    parallel_init();
    char body[8192];
    if (!build_extract_body(urls_csv, objective, body, sizeof(body))) return NULL;
    return do_request(api_base, api_key, "extract", body, 0);
}

void parallel_free(char *ptr) {
    free(ptr);
}

/* ── cleanup (optional, for testing) ────────────────────────── */

void parallel_cleanup(void) {
    if (g_headers_auth) { curl_slist_free_all(g_headers_auth); g_headers_auth = NULL; }
    if (g_headers_json) { curl_slist_free_all(g_headers_json); g_headers_json = NULL; }
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
    curl_global_cleanup();
    initialized = 0;
}
