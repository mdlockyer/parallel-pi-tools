/*
 * parallel.c — Native Parallel Search/Extract CLI.
 * Thin front-end over parallel_lib.c (same HTTP + formatting code path as
 * the FFI library — no duplicated logic).
 *
 * Build: cc -O2 -o parallel parallel.c parallel_lib.c cJSON.c -lcurl -lpthread
 * Usage:
 *   ./parallel search <api_base> <api_key|-> <objective> [search_queries_json]
 *   ./parallel extract <api_base> <api_key|-> <urls_json> [objective]
 *
 * A key argument of "-" reads the key from the PARALLEL_API_KEY environment
 * variable instead of the command line, so keys never show up in `ps`.
 *   ./parallel search https://api.parallel.ai/v1 - "objective" '["q1","q2"]'
 * If search_queries_json is omitted, defaults to ["objective"].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

char *parallel_search(const char *api_base, const char *api_key,
                      const char *objective, const char *search_queries_json);
char *parallel_extract(const char *api_base, const char *api_key,
                       const char *urls_json, const char *objective);
void parallel_init(void);

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s search <api_base> <api_key|-> <objective> [search_queries_json]\n", prog);
    fprintf(stderr, "  %s extract <api_base> <api_key|-> <urls_json> [objective]\n", prog);
    fprintf(stderr, "A key of \"-\" reads the key from $PARALLEL_API_KEY.\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s search https://api.parallel.ai/v1 - \"find cats\" '[\"cats\",\"kittens\"]'\n", prog);
    fprintf(stderr, "  %s extract https://api.parallel.ai/v1 - '[\"https://example.com/a,b\"]'\n", prog);
}

/* Parse an argv string that must be a JSON array. Returns the parsed cJSON
 * array or NULL (after printing a message). */
static cJSON *parse_array_arg(const char *arg, const char *what) {
    cJSON *arr = cJSON_Parse(arg);
    if (!arr || !cJSON_IsArray(arr)) {
        fprintf(stderr, "%s must be a JSON array\n", what);
        if (arr) cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

int main(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }

    const char *mode = argv[1];
    const char *base = argv[2];
    const char *key_arg = argv[3];
    int is_search = (strcmp(mode, "search") == 0);
    int is_extract = (strcmp(mode, "extract") == 0);
    if (!is_search && !is_extract) { usage(argv[0]); return 1; }

    const char *key = strcmp(key_arg, "-") == 0 ? getenv("PARALLEL_API_KEY")
                                                : key_arg;
    if (!key || !*key) {
        fprintf(stderr, "error: no API key (pass as argument, or use \"-\" with $PARALLEL_API_KEY set)\n");
        return 1;
    }

    char *result = NULL;
    if (is_search) {
        if (argc < 5) { fprintf(stderr, "missing objective\n"); return 1; }
        const char *objective = argv[4];
        // Optional 5th arg: JSON array for search_queries. Defaults to [objective].
        const char *queries_json = NULL;
        if (argc >= 6) {
            cJSON *queries = parse_array_arg(argv[5], "search_queries_json");
            if (!queries) return 1;
            queries_json = argv[5];
            cJSON_Delete(queries);
        }
        result = parallel_search(base, key, objective, queries_json);
    } else {
        if (argc < 5) { fprintf(stderr, "missing urls_json\n"); return 1; }
        cJSON *urls = parse_array_arg(argv[4], "urls_json");
        if (!urls) return 1;
        cJSON_Delete(urls);
        result = parallel_extract(base, key, argv[4], argc >= 6 ? argv[5] : NULL);
    }

    if (!result) { fprintf(stderr, "error: out of memory\n"); return 1; }

    // Library reports failures as "PARALLEL_ERROR: <cause>" strings.
    static const char prefix[] = "PARALLEL_ERROR:";
    if (strncmp(result, prefix, sizeof(prefix) - 1) == 0) {
        const char *cause = result + sizeof(prefix) - 1;
        while (*cause == ' ') cause++;
        fprintf(stderr, "error: %s\n", cause);
        free(result);
        return 1;
    }

    fputs(result, stdout);
    free(result);
    return 0;
}
