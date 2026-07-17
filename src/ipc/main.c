/*
 * libsavesync IPC binary — Layer 1 entry point
 *
 * A thin protocol adapter over savesync.c: reads newline-delimited JSON
 * from stdin, dispatches to sv_* calls, writes newline-delimited JSON
 * responses to stdout.
 *
 * Protocol: see docs/ipc-protocol-v1.md
 * JSON parser: hand-rolled, minimal, sufficient for flat objects with
 * string/number/bool/null values. No nested objects/arrays beyond one
 * level of params.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include "savesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===================================================================
 *  Minimal JSON parser
 * =================================================================== */

typedef enum {
    JV_NULL, JV_BOOL, JV_INT, JV_STRING, JV_OBJECT, JV_ARRAY
} json_value_type;

typedef struct json_value json_value;
struct json_value {
    json_value_type type;
    union {
        int int_val;
        int bool_val;
        char *string_val;
        struct { char **keys; json_value **vals; int count; } object;
        struct { json_value **items; int count; } array;
    } u;
};

static void json_free(json_value *v);

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *parse_string_raw(const char *p, char **out) {
    if (*p != '"') return NULL;
    p++;
    int cap = 64, len = 0;
    char *buf = malloc(cap);
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default: break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
    }
    if (*p != '"') { free(buf); return NULL; }
    buf[len] = '\0';
    *out = buf;
    return p + 1;
}

static const char *parse_value(const char *p, json_value **out);

static const char *parse_object(const char *p, json_value **out) {
    if (*p != '{') return NULL;
    p = skip_ws(p + 1);
    int cap = 8, count = 0;
    char **keys = malloc(cap * sizeof(char*));
    json_value **vals = malloc(cap * sizeof(json_value*));
    if (*p == '}') { p++; } else {
        while (1) {
            p = skip_ws(p);
            char *key = NULL;
            p = parse_string_raw(p, &key);
            if (!p) goto fail;
            p = skip_ws(p);
            if (*p != ':') { free(key); goto fail; }
            p = skip_ws(p + 1);
            json_value *val = NULL;
            p = parse_value(p, &val);
            if (!p) { free(key); goto fail; }
            if (count >= cap) { cap *= 2; keys = realloc(keys, cap*sizeof(char*)); vals = realloc(vals, cap*sizeof(json_value*)); }
            keys[count] = key;
            vals[count] = val;
            count++;
            p = skip_ws(p);
            if (*p == '}') { p++; break; }
            if (*p != ',') goto fail;
            p++;
        }
    }
    json_value *v = calloc(1, sizeof(json_value));
    v->type = JV_OBJECT;
    v->u.object.keys = keys;
    v->u.object.vals = vals;
    v->u.object.count = count;
    *out = v;
    return p;
fail:
    for (int i = 0; i < count; i++) { free(keys[i]); json_free(vals[i]); }
    free(keys); free(vals);
    return NULL;
}

static const char *parse_array(const char *p, json_value **out) {
    if (*p != '[') return NULL;
    p = skip_ws(p + 1);
    int cap = 8, count = 0;
    json_value **items = malloc(cap * sizeof(json_value*));
    if (*p == ']') { p++; } else {
        while (1) {
            p = skip_ws(p);
            json_value *val = NULL;
            p = parse_value(p, &val);
            if (!p) goto fail;
            if (count >= cap) { cap *= 2; items = realloc(items, cap*sizeof(json_value*)); }
            items[count++] = val;
            p = skip_ws(p);
            if (*p == ']') { p++; break; }
            if (*p != ',') goto fail;
            p++;
        }
    }
    json_value *v = calloc(1, sizeof(json_value));
    v->type = JV_ARRAY;
    v->u.array.items = items;
    v->u.array.count = count;
    *out = v;
    return p;
fail:
    for (int i = 0; i < count; i++) json_free(items[i]);
    free(items);
    return NULL;
}

static const char *parse_value(const char *p, json_value **out) {
    p = skip_ws(p);
    if (*p == '"') {
        char *str = NULL;
        p = parse_string_raw(p, &str);
        if (!p) return NULL;
        json_value *v = calloc(1, sizeof(json_value));
        v->type = JV_STRING;
        v->u.string_val = str;
        *out = v;
        return p;
    }
    if (*p == '{') { return parse_object(p, out); }
    if (*p == '[') { return parse_array(p, out); }
    if (*p == 't' || *p == 'f') {
        json_value *v = calloc(1, sizeof(json_value));
        v->type = JV_BOOL;
        v->u.bool_val = (*p == 't') ? 1 : 0;
        *out = v;
        return p + (v->u.bool_val ? 4 : 5);
    }
    if (*p == 'n') {
        json_value *v = calloc(1, sizeof(json_value));
        v->type = JV_NULL;
        *out = v;
        return p + 4;
    }
    if (*p == '-' || isdigit(*p)) {
        json_value *v = calloc(1, sizeof(json_value));
        v->type = JV_INT;
        v->u.int_val = (int)strtol(p, (char**)&p, 10);
        *out = v;
        return p;
    }
    return NULL;
}

static json_value *json_parse(const char *str) {
    json_value *v = NULL;
    const char *p = parse_value(str, &v);
    if (p) { p = skip_ws(p); if (*p != '\0' && *p != '\n' && *p != '\r') { json_free(v); return NULL; } }
    return v;
}

static void json_free(json_value *v) {
    if (!v) return;
    switch (v->type) {
        case JV_STRING: free(v->u.string_val); break;
        case JV_OBJECT:
            for (int i = 0; i < v->u.object.count; i++) {
                free(v->u.object.keys[i]);
                json_free(v->u.object.vals[i]);
            }
            free(v->u.object.keys);
            free(v->u.object.vals);
            break;
        case JV_ARRAY:
            for (int i = 0; i < v->u.array.count; i++) json_free(v->u.array.items[i]);
            free(v->u.array.items);
            break;
        default: break;
    }
    free(v);
}

/* Look up a key in a parsed JSON object */
static json_value *json_obj_get(const json_value *obj, const char *key) {
    if (!obj || obj->type != JV_OBJECT) return NULL;
    for (int i = 0; i < obj->u.object.count; i++)
        if (strcmp(obj->u.object.keys[i], key) == 0)
            return obj->u.object.vals[i];
    return NULL;
}

/* Get string value, or default */
static const char *json_str(const json_value *v, const char *dflt) {
    if (!v || v->type != JV_STRING) return dflt;
    return v->u.string_val;
}

/* Get int value, or default */
static int json_int(const json_value *v, int dflt) {
    if (!v || v->type != JV_INT) return dflt;
    return v->u.int_val;
}

/* ===================================================================
 *  Minimal JSON writer (StringBuilder)
 * =================================================================== */

typedef struct {
    char *buf;
    int len, cap;
} sb;

static void sb_init(sb *s) { s->cap = 256; s->len = 0; s->buf = malloc(s->cap); s->buf[0] = '\0'; }
static void sb_put(sb *s, const char *str) {
    int slen = (int)strlen(str);
    while (s->len + slen + 1 > s->cap) { s->cap *= 2; s->buf = realloc(s->buf, s->cap); }
    memcpy(s->buf + s->len, str, slen + 1);
    s->len += slen;
}
static void sb_putc(sb *s, char c) { char tmp[2] = {c, 0}; sb_put(s, tmp); }
static void sb_put_escaped(sb *s, const char *str) {
    sb_putc(s, '"');
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  sb_put(s, "\\\""); break;
            case '\\': sb_put(s, "\\\\"); break;
            case '\n': sb_put(s, "\\n"); break;
            case '\t': sb_put(s, "\\t"); break;
            default: sb_putc(s, *p); break;
        }
    }
    sb_putc(s, '"');
}

/* Forward declarations */
static const char *sv_status_str(sv_status_t st);

/* ===================================================================
 *  Response helpers
 * =================================================================== */

static void respond_error(const char *id, int code, const char *message) {
    sb s; sb_init(&s);
    sb_put(&s, "{\"id\":");
    sb_put_escaped(&s, id ? id : "");
    char buf[128];
    snprintf(buf, sizeof(buf), ",\"error\":{\"code\":%d,\"message\":", code);
    sb_put(&s, buf);
    sb_put_escaped(&s, message);
    sb_put(&s, "}}\n");
    fwrite(s.buf, 1, s.len, stdout);
    fflush(stdout);
    free(s.buf);
}

static void respond_ok_start(sb *s, const char *id) {
    sb_put(s, "{\"id\":");
    sb_put_escaped(s, id ? id : "");
    sb_put(s, ",\"result\":");
}

static void respond_finish(sb *s) {
    sb_put(s, "}\n");
    fwrite(s->buf, 1, s->len, stdout);
    fflush(stdout);
    free(s->buf);
}

/* ===================================================================
 *  Manifest & Registration handle storage (v1: small fixed tables)
 * =================================================================== */

#define MAX_MANIFESTS 32
#define MAX_REGS 256

static sv_manifest_t *manifests[MAX_MANIFESTS];
static int manifest_count = 0;

static sv_registration_t *registrations[MAX_REGS];
static int reg_count = 0;

static int find_manifest(int id) {
    if (id < 0 || id >= manifest_count || !manifests[id]) return -1;
    return id;
}

static int store_manifest(sv_manifest_t *m) {
    for (int i = 0; i < MAX_MANIFESTS; i++)
        if (!manifests[i]) { manifests[i] = m; return i; }
    return -1;
}

static sv_registration_t *find_reg(const char *id_str) {
    int id = atoi(id_str);
    if (id < 0 || id >= reg_count) return NULL;
    return registrations[id];
}

static int store_reg(sv_registration_t *r) {
    for (int i = 0; i < MAX_REGS; i++)
        if (!registrations[i]) { registrations[i] = r; return i; }
    return -1;
}

/* ===================================================================
 *  Dispatch table
 * =================================================================== */

static int handle_init(const char *id, const json_value *params) {
    const char *base_path = json_str(json_obj_get(params, "base_path"), NULL);
    if (!base_path) { respond_error(id, -2, "missing base_path"); return 0; }
    sv_status_t st = sv_init(base_path);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"ok\":true}");
    respond_finish(&s);
    return 0;
}

static const char *sv_status_str(sv_status_t st) {
    switch (st) {
        case SV_OK: return "ok";
        case SV_ERR_GENERIC: return "generic error";
        case SV_ERR_INVALID_ARG: return "invalid argument";
        case SV_ERR_NOT_FOUND: return "not found";
        case SV_ERR_IO: return "I/O error";
        case SV_ERR_OUT_OF_MEMORY: return "out of memory";
        case SV_ERR_CONFLICT: return "conflict";
        case SV_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown error";
}

static int handle_manifest_load(const char *id, const json_value *params) {
    const char *path = json_str(json_obj_get(params, "path"), NULL);
    if (!path) { respond_error(id, -2, "missing path"); return 0; }
    sv_manifest_t *m = sv_manifest_create();
    sv_status_t st = sv_manifest_load(path, m);
    if (st != SV_OK) { sv_manifest_free(m); respond_error(id, (int)st, sv_status_str(st)); return 0; }
    int mid = store_manifest(m);
    if (mid < 0) { sv_manifest_free(m); respond_error(id, -5, "too many manifests"); return 0; }
    char buf[64]; snprintf(buf, sizeof(buf), "m%d", mid);
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"manifest_id\":"); sb_put_escaped(&s, buf); sb_put(&s, "}");
    respond_finish(&s);
    return 0;
}

static int handle_register(const char *id, const json_value *params) {
    const char *mid_str = json_str(json_obj_get(params, "manifest_id"), NULL);
    const char *live_path = json_str(json_obj_get(params, "live_path"), NULL);
    if (!mid_str || !live_path) { respond_error(id, -2, "missing manifest_id or live_path"); return 0; }
    int mid = atoi(mid_str + 1);
    sv_manifest_t *manifest = NULL;
    if (find_manifest(mid) >= 0) manifest = manifests[mid];

    sv_register_opts_t opts = {0};
    opts.live_path = live_path;
    opts.rom_path = json_str(json_obj_get(params, "rom_path"), NULL);
    opts.game_id = json_str(json_obj_get(params, "game_id"), NULL);
    const char *shape_str = json_str(json_obj_get(params, "shape"), NULL);
    if (shape_str) {
        if (strcmp(shape_str, "file") == 0) opts.shape = SV_SHAPE_FILE;
        else if (strcmp(shape_str, "directory") == 0) opts.shape = SV_SHAPE_DIRECTORY;
        else if (strcmp(shape_str, "container") == 0) opts.shape = SV_SHAPE_CONTAINER;
        else if (strcmp(shape_str, "archive") == 0) opts.shape = SV_SHAPE_ARCHIVE;
    }
    opts.retention_count = (uint32_t)json_int(json_obj_get(params, "retention_count"), 5);

    sv_status_t st;
    sv_registration_t *reg;
    if (manifest)
        reg = sv_register_with_manifest(&opts, manifest, &st);
    else
        reg = sv_register(&opts, &st);
    if (!reg) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    int rid = store_reg(reg);
    if (rid < 0) { sv_unregister(reg); respond_error(id, -5, "too many registrations"); return 0; }
    char buf[64]; snprintf(buf, sizeof(buf), "r%d", rid);
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"registration_id\":"); sb_put_escaped(&s, buf); sb_put(&s, "}");
    respond_finish(&s);
    return 0;
}

static int handle_save(const char *id, const json_value *params) {
    const char *rid_str = json_str(json_obj_get(params, "registration_id"), NULL);
    if (!rid_str) { respond_error(id, -2, "missing registration_id"); return 0; }
    sv_registration_t *reg = find_reg(rid_str);
    if (!reg) { respond_error(id, -3, "registration not found"); return 0; }
    sv_save_result_t result;
    sv_status_t st = sv_save(reg, NULL, &result);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"entry_id\":"); sb_put_escaped(&s, result.entry_id);
    char buf[128];
    snprintf(buf, sizeof(buf), ",\"entry_created\":%s,\"dedup_skipped\":%s",
             result.entry_created ? "true" : "false",
             result.dedup_skipped ? "true" : "false");
    sb_put(&s, buf);
    sb_put(&s, "}");
    respond_finish(&s);
    return 0;
}

static int handle_pull(const char *id, const json_value *params) {
    const char *rid_str = json_str(json_obj_get(params, "registration_id"), NULL);
    if (!rid_str) { respond_error(id, -2, "missing registration_id"); return 0; }
    sv_registration_t *reg = find_reg(rid_str);
    if (!reg) { respond_error(id, -3, "registration not found"); return 0; }
    sv_pull_result_t result;
    sv_status_t st = sv_pull(reg, NULL, &result);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"conflicted\":%s,\"did_pull\":%s,\"did_backup\":%s}",
             result.conflicted ? "true" : "false",
             result.did_pull ? "true" : "false",
             result.did_backup ? "true" : "false");
    sb_put(&s, buf);
    sb_put(&s, "}");
    respond_finish(&s);
    return 0;
}

static int handle_pull_select(const char *id, const json_value *params) {
    const char *rid_str = json_str(json_obj_get(params, "registration_id"), NULL);
    const char *eid_str = json_str(json_obj_get(params, "entry_id"), NULL);
    if (!rid_str || !eid_str) { respond_error(id, -2, "missing registration_id or entry_id"); return 0; }
    sv_registration_t *reg = find_reg(rid_str);
    if (!reg) { respond_error(id, -3, "registration not found"); return 0; }
    sv_id_t eid;
    strncpy(eid, eid_str, sizeof(eid) - 1); eid[8] = '\0';
    sv_pull_result_t result;
    sv_status_t st = sv_pull_select(reg, eid, NULL, &result);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"conflicted\":%s,\"did_pull\":%s,\"did_backup\":%s}",
             result.conflicted ? "true" : "false",
             result.did_pull ? "true" : "false",
             result.did_backup ? "true" : "false");
    sb_put(&s, buf);
    sb_put(&s, "}");
    respond_finish(&s);
    return 0;
}

static int handle_list_entries(const char *id, const json_value *params) {
    const char *rid_str = json_str(json_obj_get(params, "registration_id"), NULL);
    if (!rid_str) { respond_error(id, -2, "missing registration_id"); return 0; }
    sv_id_t reg_id;
    strncpy(reg_id, rid_str, sizeof(reg_id) - 1); reg_id[8] = '\0';
    sv_id_t ids[64];
    size_t count = 0;
    sv_status_t st = sv_list_entries(reg_id, ids, 64, &count);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"entries\":[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_putc(&s, ',');
        sv_entry_info_t info;
        st = sv_read_entry(ids[i], &info);
        sb_putc(&s, '{');
        sb_put(&s, "\"id\":"); sb_put_escaped(&s, ids[i]);
        sb_put(&s, ",\"label\":"); sb_put_escaped(&s, info.label);
        char buf[128];
        snprintf(buf, sizeof(buf), ",\"mtime\":%lld,\"size_bytes\":%llu",
                 (long long)info.mtime, (unsigned long long)info.size_bytes);
        sb_put(&s, buf);
        sb_putc(&s, '}');
    }
    sb_put(&s, "]}");
    respond_finish(&s);
    return 0;
}

static int handle_list_registrations(const char *id, const json_value *params) {
    (void)params;
    sv_id_t ids[MAX_REGS];
    size_t count = 0;
    sv_status_t st = sv_list_registrations(ids, MAX_REGS, &count);
    if (st != SV_OK) { respond_error(id, (int)st, sv_status_str(st)); return 0; }
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"registrations\":[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_putc(&s, ',');
        sv_registration_info_t info;
        st = sv_read_registration(ids[i], &info);
        sb_putc(&s, '{');
        sb_put(&s, "\"id\":"); sb_put_escaped(&s, ids[i]);
        sb_put(&s, ",\"game_id\":"); sb_put_escaped(&s, info.game_id);
        sb_put(&s, ",\"platform\":"); sb_put_escaped(&s, info.platform);
        sb_put(&s, ",\"emulator\":"); sb_put_escaped(&s, info.emulator);
        sb_putc(&s, '}');
    }
    sb_put(&s, "]}");
    respond_finish(&s);
    return 0;
}

static int handle_unregister(const char *id, const json_value *params) {
    const char *rid_str = json_str(json_obj_get(params, "registration_id"), NULL);
    if (!rid_str) { respond_error(id, -2, "missing registration_id"); return 0; }
    sv_registration_t *reg = find_reg(rid_str);
    if (!reg) { respond_error(id, -3, "registration not found"); return 0; }
    sv_unregister(reg);
    int rid = atoi(rid_str + 1);
    if (rid >= 0 && rid < MAX_REGS) registrations[rid] = NULL;
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"ok\":true}");
    respond_finish(&s);
    return 0;
}

static int handle_shutdown(const char *id, const json_value *params) {
    (void)params;
    sb s; sb_init(&s);
    respond_ok_start(&s, id);
    sb_put(&s, "{\"ok\":true}");
    respond_finish(&s);
    sv_shutdown();
    return 1; /* signal exit */
}

/* ===================================================================
 *  Main loop
 * =================================================================== */

typedef int (*handler_fn)(const char *id, const json_value *params);

static struct { const char *name; handler_fn fn; } handlers[] = {
    { "init",                    handle_init },
    { "manifest_load",           handle_manifest_load },
    { "register_with_manifest",  handle_register },
    { "register",                handle_register },
    { "save",                    handle_save },
    { "pull",                    handle_pull },
    { "pull_select",             handle_pull_select },
    { "list_entries",            handle_list_entries },
    { "list_registrations",      handle_list_registrations },
    { "unregister",              handle_unregister },
    { "shutdown",                handle_shutdown },
    { NULL, NULL }
};

int main(void) {
    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        /* Skip blank lines */
        const char *p = skip_ws(line);
        if (*p == '\0' || *p == '\n' || *p == '\r') continue;

        json_value *msg = json_parse(line);
        if (!msg || msg->type != JV_OBJECT) {
            respond_error("", -100, "parse error");
            json_free(msg);
            continue;
        }

        const char *id = json_str(json_obj_get(msg, "id"), "");
        const char *method = json_str(json_obj_get(msg, "method"), NULL);
        const json_value *params = json_obj_get(msg, "params");

        if (!method) {
            respond_error(id, -101, "missing method");
            json_free(msg);
            continue;
        }

        int found = 0;
        for (int i = 0; handlers[i].name; i++) {
            if (strcmp(handlers[i].name, method) == 0) {
                int rc = handlers[i].fn(id, params);
                json_free(msg);
                if (rc) return 0; /* shutdown */
                found = 1;
                break;
            }
        }
        if (!found) {
            respond_error(id, -101, "unknown method");
            json_free(msg);
        }
    }
    return 0;
}
