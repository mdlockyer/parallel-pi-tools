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

// Debug mode: set PARALLEL_DEBUG=1 to get stderr logging
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

/* ── API key detection ──────────────────────────────────────── */

/*
 * Matches: PARALLEL_API_KEY, PARALLEL_KEY, PARALLELSECRET,
 *          PARALLEL_TOKEN, PARALLEL-API-KEY, plus case variants.
 * Pattern: PARALLEL[_-]?(API[_-]?)?(KEY|SECRET|TOKEN)
 */
static int match_key_var(const char *name) {
    const char *p = name;
    /* match PARALLEL (case-insensitive) */
    if (tolower(p[0]) != 'p' || tolower(p[1]) != 'a' || tolower(p[2]) != 'r' ||
        tolower(p[3]) != 'a' || tolower(p[4]) != 'l' || tolower(p[5]) != 'l' ||
        tolower(p[6]) != 'l' || tolower(p[7]) != 'e' || tolower(p[8]) != 'l')
        return 0;
    p += 9;

    /* optional [_-] */
    if (*p == '_' || *p == '-') p++;

    /* optional API[_-] */
    if (tolower(p[0]) == 'a' && tolower(p[1]) == 'p' && tolower(p[2]) == 'i') {
        p += 3;
        if (*p == '_' || *p == '-') p++;
    }

    /* KEY, SECRET, or TOKEN */
    if ((p[0]|0x20) == 'k' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'y' && p[3] == '\0') return 1;
    if ((p[0]|0x20) == 's' && (p[1]|0x20) == 'e' && (p[2]|0x20) == 'c' &&
        (p[3]|0x20) == 'r' && (p[4]|0x20) == 'e' && (p[5]|0x20) == 't' && p[6] == '\0') return 1;
    if ((p[0]|0x20) == 't' && (p[1]|0x20) == 'o' && (p[2]|0x20) == 'k' &&
        (p[3]|0x20) == 'e' && (p[4]|0x20) == 'n' && p[5] == '\0') return 1;

    return 0;
}

/* Check a single env var, return trimmed value or NULL */
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
    /* Ordered by likelihood. getenv is fast but we check each name explicitly
       because environ may not be available in shared library context. */
    static const char *candidates[] = {
        "PARALLEL_API_KEY",
        "PARALLEL_KEY",
        "PARALLELSECRET",
        "PARALLEL_TOKEN",
        "PARALLEL-API-KEY",
        "parallel_api_key",
        "parallel_key",
        "parallelsecret",
        "parallel_token",
        "parallel-api-key",
        "Parallel_Key",
        "Parallel_Token",
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
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    Buffer buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    buf.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (api_key && *api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    dbg("curl perform: res=%d status=%ld", (int)res, status);
    if (res != CURLE_OK) {
        dbg("curl error: %s", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || status >= 400) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── string builder ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->cap = 4096;
    sb->data = malloc(sb->cap);
    sb->len = 0;
    sb->data[0] = '\0';
}

static void sb_append(StringBuilder *sb, const char *s) {
    size_t slen = strlen(s);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        sb->data = realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

/* ── formatters ──────────────────────────────────────────────── */

static void format_search(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);
        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            sb_append(sb, "### "); sb_append(sb, title->valuestring); sb_append(sb, "\n");
        }
        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            sb_append(sb, "URL: "); sb_append(sb, url->valuestring); sb_append(sb, "\n\n");
        }
        cJSON *excerpts = cJSON_GetObjectItem(r, "excerpts");
        if (cJSON_IsArray(excerpts)) {
            int m = cJSON_GetArraySize(excerpts);
            for (int j = 0; j < m; j++) {
                cJSON *e = cJSON_GetArrayItem(excerpts, j);
                if (cJSON_IsString(e)) {
                    sb_append(sb, "> "); sb_append(sb, e->valuestring);
                    if (j < m - 1) sb_append(sb, "\n\n");
                }
            }
        }
        if (i < n - 1) sb_append(sb, "\n\n---\n\n");
    }
}

static void format_extract(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);
        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            sb_append(sb, "### "); sb_append(sb, title->valuestring); sb_append(sb, "\n");
        }
        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            sb_append(sb, "URL: "); sb_append(sb, url->valuestring); sb_append(sb, "\n\n");
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
                        if (j < m - 1) sb_append(sb, "\n\n");
                    }
                }
            }
        }
        if (i < n - 1) sb_append(sb, "\n\n---\n\n");
    }
}

/* ── core: fetch + parse + format ────────────────────────────── */

static char *do_request(const char *api_base, const char *api_key,
                        const char *endpoint, const char *req_body,
                        int is_search) {
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s", api_base, endpoint);

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
    return sb.data;  /* caller must free */
}

/* ── public API ──────────────────────────────────────────────── */

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
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "objective", objective);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char *result = do_request(api_base, api_key, "search", body, 1);
    free(body);
    return result;
}

char *parallel_extract(const char *api_base, const char *api_key,
                       const char *urls_csv, const char *objective) {
    parallel_init();
    cJSON *req = cJSON_CreateObject();
    cJSON *urls = cJSON_CreateArray();
    char *dup = strdup(urls_csv);
    char *tok = strtok(dup, ",");
    while (tok) {
        cJSON_AddItemToArray(urls, cJSON_CreateString(tok));
        tok = strtok(NULL, ",");
    }
    free(dup);
    cJSON_AddItemToObject(req, "urls", urls);
    if (objective && *objective) {
        cJSON_AddStringToObject(req, "objective", objective);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char *result = do_request(api_base, api_key, "extract", body, 0);
    free(body);
    return result;
}

void parallel_free(char *ptr) {
    free(ptr);
}
