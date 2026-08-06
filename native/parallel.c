/*
 * parallel.c — Native Parallel Search/Extract tool.
 * Full pipeline: HTTP via libcurl + JSON parsing + text formatting.
 *
 * Build: cc -O2 -o parallel parallel.c cJSON.c -lcurl
 * Usage: ./parallel search <api_base> <api_key> <objective>
 *        ./parallel extract <api_base> <api_key> <urls_csv> [objective]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

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

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    if (status >= 400) {
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

static char *sb_detach(StringBuilder *sb) {
    return sb->data;
}

/* ── format search results ───────────────────────────────────── */

static void format_search_results(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);

        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            sb_append(sb, "### ");
            sb_append(sb, title->valuestring);
            sb_append(sb, "\n");
        }

        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            sb_append(sb, "URL: ");
            sb_append(sb, url->valuestring);
            sb_append(sb, "\n\n");
        }

        cJSON *excerpts = cJSON_GetObjectItem(r, "excerpts");
        if (cJSON_IsArray(excerpts)) {
            int m = cJSON_GetArraySize(excerpts);
            for (int j = 0; j < m; j++) {
                cJSON *e = cJSON_GetArrayItem(excerpts, j);
                if (cJSON_IsString(e)) {
                    sb_append(sb, "> ");
                    sb_append(sb, e->valuestring);
                    if (j < m - 1) sb_append(sb, "\n\n");
                }
            }
        }

        if (i < n - 1) sb_append(sb, "\n\n---\n\n");
    }
}

/* ── format extract results ──────────────────────────────────── */

static void format_extract_results(cJSON *results, StringBuilder *sb) {
    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_GetArrayItem(results, i);

        cJSON *title = cJSON_GetObjectItem(r, "title");
        if (cJSON_IsString(title) && title->valuestring[0]) {
            sb_append(sb, "### ");
            sb_append(sb, title->valuestring);
            sb_append(sb, "\n");
        }

        cJSON *url = cJSON_GetObjectItem(r, "url");
        if (cJSON_IsString(url)) {
            sb_append(sb, "URL: ");
            sb_append(sb, url->valuestring);
            sb_append(sb, "\n\n");
        }

        /* prefer full_content, fall back to excerpts */
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

/* ── main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s search <api_base> <api_key> <objective>\n", argv[0]);
        fprintf(stderr, "  %s extract <api_base> <api_key> <urls_csv> [objective]\n", argv[0]);
        return 1;
    }

    const char *mode   = argv[1];
    const char *base   = argv[2];
    const char *key    = argv[3];
    int is_search = (strcmp(mode, "search") == 0);

    /* build request JSON */
    cJSON *req = cJSON_CreateObject();
    if (is_search) {
        if (argc < 5) { fprintf(stderr, "missing objective\n"); return 1; }
        cJSON_AddStringToObject(req, "objective", argv[4]);
        /* API requires search_queries — default to [objective] */
        cJSON *queries = cJSON_CreateArray();
        cJSON_AddItemToArray(queries, cJSON_CreateString(argv[4]));
        cJSON_AddItemToObject(req, "search_queries", queries);
    } else {
        if (argc < 5) { fprintf(stderr, "missing urls\n"); return 1; }
        cJSON *urls = cJSON_CreateArray();
        /* split on comma */
        char *dup = strdup(argv[4]);
        char *tok = strtok(dup, ",");
        while (tok) {
            cJSON_AddItemToArray(urls, cJSON_CreateString(tok));
            tok = strtok(NULL, ",");
        }
        free(dup);
        cJSON_AddItemToObject(req, "urls", urls);
        if (argc >= 6) {
            cJSON_AddStringToObject(req, "objective", argv[5]);
        }
    }

    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    /* build URL */
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s", base, is_search ? "search" : "extract");

    /* HTTP POST */
    char *resp = http_post(url, req_str, key);
    free(req_str);

    if (!resp) {
        fprintf(stderr, "HTTP request failed\n");
        return 1;
    }

    /* parse response */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        fprintf(stderr, "JSON parse failed\n");
        return 1;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results)) {
        fprintf(stderr, "no results array\n");
        cJSON_Delete(root);
        return 1;
    }

    /* format output */
    StringBuilder sb;
    sb_init(&sb);

    if (is_search) {
        format_search_results(results, &sb);
    } else {
        format_extract_results(results, &sb);
    }

    cJSON_Delete(root);

    /* output */
    char *output = sb_detach(&sb);
    fputs(output, stdout);
    free(output);

    return 0;
}
