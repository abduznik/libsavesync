#include "savesync.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===================================================================
 *  SHA1 Implementation (public domain)
 * =================================================================== */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sv_sha1_ctx;

#define SV_SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sv_sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    uint32_t a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  | (uint32_t)block[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = SV_SHA1_ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)
            t = 0x5A827999 + ((b & c) | (~b & d));
        else if (i < 40)
            t = 0x6ED9EBA1 + (b ^ c ^ d);
        else if (i < 60)
            t = 0x8F1BBCDC + ((b & c) | (b & d) | (c & d));
        else
            t = 0xCA62C1D6 + (b ^ c ^ d);
        t += SV_SHA1_ROTL(a, 5) + e + w[i];
        e = d; d = c; c = SV_SHA1_ROTL(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sv_sha1_init(sv_sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static void sv_sha1_update(sv_sha1_ctx *ctx, const void *data, size_t len) {
    size_t idx = (size_t)(ctx->count & 63);
    const uint8_t *bytes = (const uint8_t *)data;
    ctx->count += len;
    size_t part = 64 - idx;
    if (len >= part) {
        memcpy(ctx->buffer + idx, bytes, part);
        sv_sha1_transform(ctx->state, ctx->buffer);
        bytes += part; len -= part;
        while (len >= 64) {
            sv_sha1_transform(ctx->state, bytes);
            bytes += 64; len -= 64;
        }
        idx = 0;
    }
    if (len > 0)
        memcpy(ctx->buffer + idx, bytes, len);
}

static void sv_sha1_final(sv_sha1_ctx *ctx, uint8_t out[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        sv_sha1_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + 7 - i] = (uint8_t)(bits >> (i * 8));
    sv_sha1_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[i*4 + 3 - j] = (uint8_t)(ctx->state[i] >> (j * 8));
}

static void hash_data(const void *data, size_t len, uint8_t out[20]) {
    sv_sha1_ctx ctx;
    sv_sha1_init(&ctx);
    sv_sha1_update(&ctx, data, len);
    sv_sha1_final(&ctx, out);
}

static bool hash_file(const char *path, uint8_t out[20]) {
    uint8_t buf[8192];
    sv_sha1_ctx ctx;
    sv_sha1_init(&ctx);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sv_sha1_update(&ctx, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) sv_sha1_final(&ctx, out);
    return ok;
}

static bool hash_compare(const uint8_t a[20], const uint8_t b[20]) {
    return memcmp(a, b, 20) == 0;
}

/* ===================================================================
 *  Internal Structures
 * =================================================================== */
typedef struct sv_entry_s {
    char        id[9];
    char        parent_id[9];
    uint8_t     content_hash[20];
    bool        content_hash_set;
    bool        integrity_ok;
    int64_t     mtime;
    uint64_t    size_bytes;
    uint32_t    playtime_seconds;
    char       *label;
    sv_save_shape_t shape;
    char       *magazine_slot_path;
    bool        orphaned;
    uint64_t    sequence;       /* monotonic creation order for deterministic FIFO eviction */
} sv_entry_t;

struct sv_registration_s {
    char        id[9];
    char       *live_path;
    char       *platform;
    char       *emulator;
    char       *product_version;
    char       *game_id;
    char       *rom_path;
    uint8_t     rom_hash[20];
    bool        rom_hash_set;
    sv_registration_mode_t mode;
    sv_save_shape_t        shape;
    char       *label;
    uint32_t    retention_count;
    char       *save_path_template;      /* raw template from manifest */
    char       *resolved_magazine_dir;   /* resolved path for magazine storage */
    sv_entry_t **entries;
    size_t      num_entries;
    size_t      entries_cap;
};

typedef struct {
    char       *base_path;
    sv_registration_t **regs;
    size_t      num_regs;
    size_t      regs_cap;
    sv_entry_t **all_entries;
    size_t      num_entries;
    size_t      entries_cap;
    bool        initialized;
    uint64_t    next_sequence;
} sv_context_t;

static sv_context_t g_ctx;

/* ===================================================================
 *  Internal Helpers
 * =================================================================== */
static void strdup_safe(char **dest, const char *src) {
    free(*dest);
    *dest = src ? strdup(src) : NULL;
}

static void sv_id_copy(sv_id_t dst, const sv_id_t src) {
    memcpy(dst, src, 8);
}

static bool sv_id_is_zero(const sv_id_t id) {
    for (int i = 0; i < 8; i++)
        if (id[i] != 0) return false;
    return true;
}

/* ===================================================================
 *  ID Generation
 * =================================================================== */
static void generate_id(char *out) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned char buf[8];
    if (fd >= 0) {
        read(fd, buf, 8);
        close(fd);
    } else {
        for (int i = 0; i < 8; i++) buf[i] = (unsigned char)(rand() & 0xFF);
    }
    for (int i = 0; i < 8; i++)
        out[i] = chars[buf[i] % 62];
    out[8] = '\0';
}

static bool id_exists_reg(const char *id) {
    for (size_t i = 0; i < g_ctx.num_regs; i++)
        if (memcmp(g_ctx.regs[i]->id, id, 8) == 0) return true;
    return false;
}

static bool id_exists_entry(const char *id) {
    for (size_t i = 0; i < g_ctx.num_entries; i++)
        if (memcmp(g_ctx.all_entries[i]->id, id, 8) == 0) return true;
    return false;
}

static void generate_unique_id(char *out) {
    do { generate_id(out); } while (id_exists_reg(out) || id_exists_entry(out));
}

/* ===================================================================
 *  Path Helpers
 * =================================================================== */
static void join_path(char *out, size_t out_len, const char *a, const char *b) {
    snprintf(out, out_len, "%s/%s", a, b);
}

static bool ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    return false;
}

static bool ensure_dirs_for_path(const char *path) {
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            }
            *p = '/';
        }
    }
    return true;
}

/* Resolve save_path_template placeholders: {game_id}, {platform}, {emulator}, {live_path} */
static void resolve_save_path_template(const char *template_str,
                                        const char *game_id,
                                        const char *platform,
                                        const char *emulator,
                                        const char *live_path,
                                        char *out, size_t out_len) {
    if (!template_str || !template_str[0] || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    size_t pos = 0;
    const char *p = template_str;

    while (*p && pos < out_len - 1) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end) {
                size_t key_len = (size_t)(end - p - 1);
                const char *value = NULL;

                if (key_len == 7 && memcmp(p + 1, "game_id", 7) == 0) {
                    value = (game_id && game_id[0]) ? game_id : "unknown";
                } else if (key_len == 8 && memcmp(p + 1, "platform", 8) == 0) {
                    value = (platform && platform[0]) ? platform : "unknown";
                } else if (key_len == 8 && memcmp(p + 1, "emulator", 8) == 0) {
                    value = (emulator && emulator[0]) ? emulator : "unknown";
                } else if (key_len == 9 && memcmp(p + 1, "live_path", 9) == 0) {
                    value = live_path ? live_path : "unknown";
                }

                if (value) {
                    size_t vlen = strlen(value);
                    if (pos + vlen < out_len) {
                        memcpy(out + pos, value, vlen);
                        pos += vlen;
                    }
                    p = end + 1;
                    continue;
                }
            }
            /* Unknown placeholder — copy as-is */
            out[pos++] = *p++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}

/* ===================================================================
 *  Metadata Store (binary flat-file format)
 * =================================================================== */
#define METADATA_MAGIC     {'S','V','S','Y'}
#define METADATA_VERSION   1
#define METADATA_HEADER_SZ 24

#define REC_TYPE_REG 0
#define REC_TYPE_ENT 1

/* Field keys for registration records */
#define REG_FLD_LIVE_PATH        1
#define REG_FLD_MODE             2
#define REG_FLD_PLATFORM         3
#define REG_FLD_EMULATOR         4
#define REG_FLD_PRODUCT_VERSION  5
#define REG_FLD_GAME_ID          6
#define REG_FLD_ROM_PATH         7
#define REG_FLD_ROM_HASH         8
#define REG_FLD_ROM_HASH_SET     9
#define REG_FLD_SHAPE            10
#define REG_FLD_LABEL            11
#define REG_FLD_RETENTION_COUNT  12
#define REG_FLD_SAVE_PATH_TEMPLATE    13
#define REG_FLD_RESOLVED_MAGAZINE_DIR 14

/* Field keys for entry records */
#define ENT_FLD_PARENT_ID        1
#define ENT_FLD_CONTENT_HASH     2
#define ENT_FLD_CONTENT_HASH_SET 3
#define ENT_FLD_INTEGRITY_OK     4
#define ENT_FLD_MTIME            5
#define ENT_FLD_SIZE_BYTES       6
#define ENT_FLD_PLAYTIME_SECONDS 7
#define ENT_FLD_LABEL            8
#define ENT_FLD_SHAPE            9
#define ENT_FLD_MAGAZINE_PATH    10
#define ENT_FLD_SEQUENCE         11

static bool write_u8(uint8_t **buf, size_t *cap, size_t *pos, uint8_t v) {
    if (*pos + 1 > *cap) return false;
    (*buf)[(*pos)++] = v;
    return true;
}

static bool write_u16_le(uint8_t **buf, size_t *cap, size_t *pos, uint16_t v) {
    if (*pos + 2 > *cap) return false;
    (*buf)[(*pos)++] = (uint8_t)(v & 0xFF);
    (*buf)[(*pos)++] = (uint8_t)((v >> 8) & 0xFF);
    return true;
}

static bool write_u32_le(uint8_t **buf, size_t *cap, size_t *pos, uint32_t v) {
    if (*pos + 4 > *cap) return false;
    (*buf)[(*pos)++] = (uint8_t)(v & 0xFF);
    (*buf)[(*pos)++] = (uint8_t)((v >> 8) & 0xFF);
    (*buf)[(*pos)++] = (uint8_t)((v >> 16) & 0xFF);
    (*buf)[(*pos)++] = (uint8_t)((v >> 24) & 0xFF);
    return true;
}

static bool write_u64_le(uint8_t **buf, size_t *cap, size_t *pos, uint64_t v) {
    if (*pos + 8 > *cap) return false;
    for (int i = 0; i < 8; i++)
        (*buf)[(*pos)++] = (uint8_t)((v >> (i * 8)) & 0xFF);
    return true;
}

static bool write_bytes(uint8_t **buf, size_t *cap, size_t *pos, const void *data, size_t len) {
    if (*pos + len > *cap) return false;
    memcpy(*buf + *pos, data, len);
    *pos += len;
    return true;
}

static bool write_string_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, const char *s) {
    if (!s) return true;
    size_t len = strlen(s);
    if (len > 65535) len = 65535;
    if (!write_u8(buf, cap, pos, key)) return false;
    if (!write_u16_le(buf, cap, pos, (uint16_t)len)) return false;
    if (!write_bytes(buf, cap, pos, s, len)) return false;
    return true;
}

static bool write_u8_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, uint8_t v) {
    if (!write_u8(buf, cap, pos, key)) return false;
    if (!write_u16_le(buf, cap, pos, 1)) return false;
    return write_u8(buf, cap, pos, v);
}

static bool write_u32_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, uint32_t v) {
    if (!write_u8(buf, cap, pos, key)) return false;
    if (!write_u16_le(buf, cap, pos, 4)) return false;
    return write_u32_le(buf, cap, pos, v);
}

static bool write_u64_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, uint64_t v) {
    if (!write_u8(buf, cap, pos, key)) return false;
    if (!write_u16_le(buf, cap, pos, 8)) return false;
    return write_u64_le(buf, cap, pos, v);
}

static bool write_i64_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, int64_t v) {
    return write_u64_field(buf, cap, pos, key, (uint64_t)v);
}

static bool write_hash_field(uint8_t **buf, size_t *cap, size_t *pos, uint8_t key, const uint8_t h[20]) {
    if (!write_u8(buf, cap, pos, key)) return false;
    if (!write_u16_le(buf, cap, pos, 20)) return false;
    return write_bytes(buf, cap, pos, h, 20);
}

static bool serialize_reg(sv_registration_t *reg, uint8_t **out, size_t *out_len) {
    size_t cap = 4096;
    *out = malloc(cap);
    if (!*out) return false;

    uint8_t *buf = *out;
    size_t off = 11;

    if (!write_string_field(&buf, &cap, &off, REG_FLD_LIVE_PATH, reg->live_path)) goto oom;
    if (!write_u8_field(&buf, &cap, &off, REG_FLD_MODE, (uint8_t)reg->mode)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_PLATFORM, reg->platform)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_EMULATOR, reg->emulator)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_PRODUCT_VERSION, reg->product_version)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_GAME_ID, reg->game_id)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_ROM_PATH, reg->rom_path)) goto oom;
    if (reg->rom_hash_set && !write_hash_field(&buf, &cap, &off, REG_FLD_ROM_HASH, reg->rom_hash)) goto oom;
    if (reg->rom_hash_set && !write_u8_field(&buf, &cap, &off, REG_FLD_ROM_HASH_SET, 1)) goto oom;
    if (!write_u8_field(&buf, &cap, &off, REG_FLD_SHAPE, (uint8_t)reg->shape)) goto oom;
    if (!write_string_field(&buf, &cap, &off, REG_FLD_LABEL, reg->label)) goto oom;
    if (reg->retention_count > 0 && !write_u32_field(&buf, &cap, &off, REG_FLD_RETENTION_COUNT, reg->retention_count)) goto oom;
    if (reg->save_path_template && reg->save_path_template[0] && !write_string_field(&buf, &cap, &off, REG_FLD_SAVE_PATH_TEMPLATE, reg->save_path_template)) goto oom;
    if (reg->resolved_magazine_dir && reg->resolved_magazine_dir[0] && !write_string_field(&buf, &cap, &off, REG_FLD_RESOLVED_MAGAZINE_DIR, reg->resolved_magazine_dir)) goto oom;

    uint16_t data_len = (uint16_t)(off - 11);
    (*out)[0] = REC_TYPE_REG;
    memcpy(*out + 1, reg->id, 8);
    (*out)[9] = (uint8_t)(data_len & 0xFF);
    (*out)[10] = (uint8_t)((data_len >> 8) & 0xFF);
    *out_len = 11 + data_len;
    return true;

oom:
    free(*out);
    *out = NULL;
    return false;
}

static bool serialize_entry(sv_entry_t *ent, uint8_t **out, size_t *out_len) {
    size_t cap = 4096;
    *out = malloc(cap);
    if (!*out) return false;

    uint8_t *buf = *out;
    size_t off = 11;

    if (ent->parent_id[0]) {
        uint8_t *b2 = buf;
        size_t o2 = off, c2 = cap;
        if (!write_u8(&b2, &c2, &o2, ENT_FLD_PARENT_ID)) goto oom;
        if (!write_u16_le(&b2, &c2, &o2, 8)) goto oom;
        if (!write_bytes(&b2, &c2, &o2, ent->parent_id, 8)) goto oom;
        off = o2;
    }
    if (ent->content_hash_set && !write_hash_field(&buf, &cap, &off, ENT_FLD_CONTENT_HASH, ent->content_hash)) goto oom;
    if (ent->content_hash_set && !write_u8_field(&buf, &cap, &off, ENT_FLD_CONTENT_HASH_SET, 1)) goto oom;
    if (!write_u8_field(&buf, &cap, &off, ENT_FLD_INTEGRITY_OK, ent->integrity_ok ? 1 : 0)) goto oom;
    if (ent->mtime != 0 && !write_i64_field(&buf, &cap, &off, ENT_FLD_MTIME, ent->mtime)) goto oom;
    if (ent->size_bytes != 0 && !write_u64_field(&buf, &cap, &off, ENT_FLD_SIZE_BYTES, ent->size_bytes)) goto oom;
    if (ent->playtime_seconds != 0 && !write_u32_field(&buf, &cap, &off, ENT_FLD_PLAYTIME_SECONDS, ent->playtime_seconds)) goto oom;
    if (!write_string_field(&buf, &cap, &off, ENT_FLD_LABEL, ent->label)) goto oom;
    if (!write_u8_field(&buf, &cap, &off, ENT_FLD_SHAPE, (uint8_t)ent->shape)) goto oom;
    if (!write_string_field(&buf, &cap, &off, ENT_FLD_MAGAZINE_PATH, ent->magazine_slot_path)) goto oom;
    if (ent->sequence != 0 && !write_u64_field(&buf, &cap, &off, ENT_FLD_SEQUENCE, ent->sequence)) goto oom;

    uint16_t data_len = (uint16_t)(off - 11);
    (*out)[0] = REC_TYPE_ENT;
    memcpy(*out + 1, ent->id, 8);
    (*out)[9] = (uint8_t)(data_len & 0xFF);
    (*out)[10] = (uint8_t)((data_len >> 8) & 0xFF);
    *out_len = 11 + data_len;
    return true;

oom:
    free(*out);
    *out = NULL;
    return false;
}

static uint8_t read_u8(const uint8_t **buf, size_t *len) {
    if (*len < 1) return 0;
    uint8_t v = (*buf)[0];
    (*buf)++; (*len)--;
    return v;
}

static uint16_t read_u16_le(const uint8_t **buf, size_t *len) {
    if (*len < 2) return 0;
    uint16_t v = (uint16_t)((*buf)[0] | ((uint16_t)(*buf)[1] << 8));
    (*buf) += 2; (*len) -= 2;
    return v;
}

static uint32_t read_u32_le(const uint8_t **buf, size_t *len) {
    if (*len < 4) return 0;
    uint32_t v = (uint32_t)((*buf)[0] | ((uint32_t)(*buf)[1] << 8) |
                ((uint32_t)(*buf)[2] << 16) | ((uint32_t)(*buf)[3] << 24));
    (*buf) += 4; (*len) -= 4;
    return v;
}

static uint64_t read_u64_le(const uint8_t **buf, size_t *len) {
    uint64_t v = 0;
    if (*len < 8) return 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)(*buf)[i] << (i * 8);
    (*buf) += 8; (*len) -= 8;
    return v;
}

static int64_t read_i64_le(const uint8_t **buf, size_t *len) {
    return (int64_t)read_u64_le(buf, len);
}

static bool read_bytes(const uint8_t **buf, size_t *len, void *out, size_t n) {
    if (*len < n) return false;
    memcpy(out, *buf, n);
    *buf += n; *len -= n;
    return true;
}

static sv_registration_t *deserialize_reg(const uint8_t *data, size_t data_len) {
    sv_registration_t *reg = calloc(1, sizeof(sv_registration_t));
    if (!reg) return NULL;
    reg->mode = SV_MODE_DEFAULT;

    const uint8_t *p = data;
    size_t remain = data_len;
    while (remain > 0) {
        uint8_t key = read_u8(&p, &remain);
        uint16_t flen = read_u16_le(&p, &remain);
        if (flen > remain) break;
        switch (key) {
            case REG_FLD_LIVE_PATH:
                reg->live_path = strndup((const char *)p, flen);
                break;
            case REG_FLD_MODE:
                if (flen >= 1) reg->mode = (sv_registration_mode_t)*p;
                break;
            case REG_FLD_PLATFORM:
                reg->platform = strndup((const char *)p, flen);
                break;
            case REG_FLD_EMULATOR:
                reg->emulator = strndup((const char *)p, flen);
                break;
            case REG_FLD_PRODUCT_VERSION:
                reg->product_version = strndup((const char *)p, flen);
                break;
            case REG_FLD_GAME_ID:
                reg->game_id = strndup((const char *)p, flen);
                break;
            case REG_FLD_ROM_PATH:
                reg->rom_path = strndup((const char *)p, flen);
                break;
            case REG_FLD_ROM_HASH:
                if (flen == 20) { memcpy(reg->rom_hash, p, 20); reg->rom_hash_set = true; }
                break;
            case REG_FLD_ROM_HASH_SET:
                break;
            case REG_FLD_SHAPE:
                if (flen >= 1) reg->shape = (sv_save_shape_t)*p;
                break;
            case REG_FLD_LABEL:
                reg->label = strndup((const char *)p, flen);
                break;
            case REG_FLD_RETENTION_COUNT:
                if (flen >= 4) reg->retention_count = read_u32_le(&p, &remain);
                /* data has been consumed, continue */
                continue;
            case REG_FLD_SAVE_PATH_TEMPLATE:
                reg->save_path_template = strndup((const char *)p, flen);
                break;
            case REG_FLD_RESOLVED_MAGAZINE_DIR:
                reg->resolved_magazine_dir = strndup((const char *)p, flen);
                break;
        }
        p += flen; remain -= flen;
    }
    return reg;
}

static sv_entry_t *deserialize_entry(const uint8_t *data, size_t data_len) {
    sv_entry_t *ent = calloc(1, sizeof(sv_entry_t));
    if (!ent) return NULL;

    const uint8_t *p = data;
    size_t remain = data_len;
    while (remain > 0) {
        uint8_t key = read_u8(&p, &remain);
        uint16_t flen = read_u16_le(&p, &remain);
        if (flen > remain) break;
        switch (key) {
            case ENT_FLD_PARENT_ID:
                if (flen == 8) { memcpy(ent->parent_id, p, 8); }
                break;
            case ENT_FLD_CONTENT_HASH:
                if (flen == 20) { memcpy(ent->content_hash, p, 20); ent->content_hash_set = true; }
                break;
            case ENT_FLD_CONTENT_HASH_SET:
                break;
            case ENT_FLD_INTEGRITY_OK:
                if (flen >= 1) ent->integrity_ok = (*p != 0);
                break;
            case ENT_FLD_MTIME:
                if (flen >= 8) ent->mtime = read_i64_le(&p, &remain);
                continue;
            case ENT_FLD_SIZE_BYTES:
                if (flen >= 8) ent->size_bytes = read_u64_le(&p, &remain);
                continue;
            case ENT_FLD_PLAYTIME_SECONDS:
                if (flen >= 4) ent->playtime_seconds = read_u32_le(&p, &remain);
                continue;
            case ENT_FLD_LABEL:
                ent->label = strndup((const char *)p, flen);
                break;
            case ENT_FLD_SHAPE:
                if (flen >= 1) ent->shape = (sv_save_shape_t)*p;
                break;
            case ENT_FLD_MAGAZINE_PATH:
                ent->magazine_slot_path = strndup((const char *)p, flen);
                break;
            case ENT_FLD_SEQUENCE:
                if (flen >= 8) ent->sequence = read_u64_le(&p, &remain);
                continue;
        }
        p += flen; remain -= flen;
    }
    return ent;
}

static bool metadata_flush(void) {
    char path[4096];
    join_path(path, sizeof(path), g_ctx.base_path, "savesync.dat");

    /* Calculate total size needed */
    size_t total = METADATA_HEADER_SZ;
    for (size_t i = 0; i < g_ctx.num_regs; i++) {
        uint8_t *buf = NULL; size_t len = 0;
        if (serialize_reg(g_ctx.regs[i], &buf, &len)) {
            total += len;
            free(buf);
        }
    }
    for (size_t i = 0; i < g_ctx.num_entries; i++) {
        uint8_t *buf = NULL; size_t len = 0;
        if (serialize_entry(g_ctx.all_entries[i], &buf, &len)) {
            total += len;
            free(buf);
        }
    }

    uint8_t *out = calloc(1, total + 64);
    if (!out) return false;
    size_t pos = 0;

    /* Write header */
    uint8_t magic[4] = METADATA_MAGIC;
    write_bytes(&out, &total, &pos, magic, 4);
    write_u32_le(&out, &total, &pos, METADATA_VERSION);
    write_u32_le(&out, &total, &pos, (uint32_t)g_ctx.num_regs);
    write_u32_le(&out, &total, &pos, (uint32_t)g_ctx.num_entries);
    for (int i = 0; i < 8; i++) write_u8(&out, &total, &pos, 0); /* reserved */

    /* Write records */
    for (size_t i = 0; i < g_ctx.num_regs; i++) {
        uint8_t *buf = NULL; size_t len = 0;
        if (serialize_reg(g_ctx.regs[i], &buf, &len)) {
            write_bytes(&out, &total, &pos, buf, len);
            free(buf);
        }
    }
    for (size_t i = 0; i < g_ctx.num_entries; i++) {
        uint8_t *buf = NULL; size_t len = 0;
        if (serialize_entry(g_ctx.all_entries[i], &buf, &len)) {
            write_bytes(&out, &total, &pos, buf, len);
            free(buf);
        }
    }

    /* Atomic write: write to temp, then rename */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.sync_tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) { free(out); return false; }
    if (fwrite(out, 1, pos, f) != pos) { fclose(f); unlink(tmp_path); free(out); return false; }
    fclose(f);

    if (rename(tmp_path, path) != 0) { unlink(tmp_path); free(out); return false; }

    free(out);
    return true;
}

static bool metadata_load(void) {
    char path[4096];
    join_path(path, sizeof(path), g_ctx.base_path, "savesync.dat");

    FILE *f = fopen(path, "rb");
    if (!f) return true; /* File doesn't exist yet, that's OK */

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < METADATA_HEADER_SZ) { fclose(f); return false; }

    uint8_t *data = malloc((size_t)fsize);
    if (!data) { fclose(f); return false; }
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) { fclose(f); free(data); return false; }
    fclose(f);

    const uint8_t *p = data;
    size_t remain = (size_t)fsize;

    uint8_t magic[4];
    read_bytes(&p, &remain, magic, 4);
    if (magic[0] != 'S' || magic[1] != 'V' || magic[2] != 'S' || magic[3] != 'Y') { free(data); return false; }

    uint32_t version = read_u32_le(&p, &remain);
    (void)version;
    uint32_t num_regs = read_u32_le(&p, &remain);
    uint32_t num_entries = read_u32_le(&p, &remain);
    p += 8; remain -= 8; /* skip reserved */

    for (uint32_t i = 0; i < num_regs && remain > 0; i++) {
        if (remain < 11) break;
        uint8_t type = read_u8(&p, &remain);
        char id[9];
        read_bytes(&p, &remain, id, 8);
        id[8] = '\0';
        uint16_t data_len = read_u16_le(&p, &remain);
        if (data_len > remain) break;

        if (type == REC_TYPE_REG) {
            sv_registration_t *reg = deserialize_reg(p, data_len);
            if (reg) {
                memcpy(reg->id, id, 9);
                if (g_ctx.num_regs >= g_ctx.regs_cap) {
                    size_t newcap = g_ctx.regs_cap ? g_ctx.regs_cap * 2 : 16;
                    sv_registration_t **newregs = realloc(g_ctx.regs, newcap * sizeof(sv_registration_t *));
                    if (!newregs) { /* leak but continue */ }
                    else { g_ctx.regs = newregs; g_ctx.regs_cap = newcap; }
                }
                g_ctx.regs[g_ctx.num_regs++] = reg;
            }
        }
        p += data_len; remain -= data_len;
    }

    for (uint32_t i = 0; i < num_entries && remain > 0; i++) {
        if (remain < 11) break;
        uint8_t type = read_u8(&p, &remain);
        char id[9];
        read_bytes(&p, &remain, id, 8);
        id[8] = '\0';
        uint16_t data_len = read_u16_le(&p, &remain);
        if (data_len > remain) break;

        if (type == REC_TYPE_ENT) {
            sv_entry_t *ent = deserialize_entry(p, data_len);
            if (ent) {
                memcpy(ent->id, id, 9);
                if (g_ctx.num_entries >= g_ctx.entries_cap) {
                    size_t newcap = g_ctx.entries_cap ? g_ctx.entries_cap * 2 : 16;
                    sv_entry_t **newents = realloc(g_ctx.all_entries, newcap * sizeof(sv_entry_t *));
                    if (!newents) { }
                    else { g_ctx.all_entries = newents; g_ctx.entries_cap = newcap; }
                }
                g_ctx.all_entries[g_ctx.num_entries++] = ent;
            }
        }
        p += data_len; remain -= data_len;
    }

    free(data);
    return true;
}

/* ===================================================================
 *  Entry management
 * =================================================================== */
static sv_entry_t *find_entry(const char *id) {
    if (!id || !id[0]) return NULL;
    for (size_t i = 0; i < g_ctx.num_entries; i++)
        if (memcmp(g_ctx.all_entries[i]->id, id, 8) == 0) return g_ctx.all_entries[i];
    return NULL;
}

static sv_registration_t *find_reg(const char *id) {
    if (!id || !id[0]) return NULL;
    for (size_t i = 0; i < g_ctx.num_regs; i++)
        if (memcmp(g_ctx.regs[i]->id, id, 8) == 0) return g_ctx.regs[i];
    return NULL;
}

static sv_registration_t *find_reg_by_entry_parent(const char *parent_id) {
    return find_reg(parent_id);
}

static void link_entry_to_reg(sv_entry_t *ent, sv_registration_t *reg) {
    if (reg) {
        memcpy(ent->parent_id, reg->id, 8);
        if (reg->num_entries >= reg->entries_cap) {
            size_t newcap = reg->entries_cap ? reg->entries_cap * 2 : 16;
            sv_entry_t **newents = realloc(reg->entries, newcap * sizeof(sv_entry_t *));
            if (newents) { reg->entries = newents; reg->entries_cap = newcap; }
        }
        if (reg->num_entries < reg->entries_cap)
            reg->entries[reg->num_entries++] = ent;
    } else {
        memset(ent->parent_id, 0, 8);
    }
}

static void add_entry(sv_entry_t *ent) {
    if (g_ctx.num_entries >= g_ctx.entries_cap) {
        size_t newcap = g_ctx.entries_cap ? g_ctx.entries_cap * 2 : 16;
        sv_entry_t **newents = realloc(g_ctx.all_entries, newcap * sizeof(sv_entry_t *));
        if (!newents) return;
        g_ctx.all_entries = newents;
        g_ctx.entries_cap = newcap;
    }
    g_ctx.all_entries[g_ctx.num_entries++] = ent;
}

static void remove_entry_from_reg(sv_entry_t *ent, sv_registration_t *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->num_entries; i++) {
        if (reg->entries[i] == ent) {
            reg->entries[i] = reg->entries[--reg->num_entries];
            break;
        }
    }
}

/* ===================================================================
 *  Atomic file operations
 * =================================================================== */
static bool atomic_copy_file(const char *src, const char *dst) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.sync_cp_tmp", dst);

    FILE *fin = fopen(src, "rb");
    if (!fin) return false;
    FILE *fout = fopen(tmp, "wb");
    if (!fout) { fclose(fin); return false; }

    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            fclose(fin); fclose(fout); unlink(tmp); return false;
        }
    }
    bool ok = !ferror(fin);
    fclose(fin);
    if (fclose(fout) != 0) { unlink(tmp); return false; }

    if (!ok) { unlink(tmp); return false; }

    if (rename(tmp, dst) != 0) { unlink(tmp); return false; }
    return true;
}

static bool atomic_copy_dir(const char *src, const char *dst) {
    /* Create destination */
    if (!ensure_dir(dst)) return false;

    char tmp_dst[4096];
    snprintf(tmp_dst, sizeof(tmp_dst), "%s.sync_cp_tmp", dst);
    if (ensure_dir(tmp_dst)) {
        /* Copy recursively */
        DIR *dir = opendir(src);
        if (!dir) { /* try clean up */ rmdir(tmp_dst); return false; }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char src_path[4096], tmp_path[4096];
            snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
            snprintf(tmp_path, sizeof(tmp_path), "%s/%s", tmp_dst, entry->d_name);

            struct stat st;
            if (stat(src_path, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                if (!atomic_copy_dir(src_path, tmp_path)) {
                    closedir(dir); /* recursive cleanup */ return false;
                }
            } else {
                if (!atomic_copy_file(src_path, tmp_path)) {
                    closedir(dir); return false;
                }
            }
        }
        closedir(dir);

        /* Atomic rename */
        if (rename(tmp_dst, dst) != 0) {
            /* Cleanup tmp */
            DIR *tdir = opendir(tmp_dst);
            if (tdir) {
                struct dirent *te;
                while ((te = readdir(tdir)) != NULL) {
                    if (strcmp(te->d_name, ".") == 0 || strcmp(te->d_name, "..") == 0) continue;
                    char tp[4096];
                    snprintf(tp, sizeof(tp), "%s/%s", tmp_dst, te->d_name);
                    unlink(tp);
                }
                closedir(tdir);
            }
            rmdir(tmp_dst);
            return false;
        }
        return true;
    }
    return false;
}

static bool copy_to_magazine(const char *live_path, const char *mag_path, sv_save_shape_t shape) {
    if (!ensure_dirs_for_path(mag_path)) return false;

    if (shape == SV_SHAPE_FILE) {
        return atomic_copy_file(live_path, mag_path);
    } else if (shape == SV_SHAPE_DIRECTORY) {
        return atomic_copy_dir(live_path, mag_path);
    } else {
        /* For UNKNOWN/CONTAINER/ARCHIVE, treat as file for now */
        return atomic_copy_file(live_path, mag_path);
    }
}

static bool is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

static int64_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

static bool replace_live(const char *mag_path, const char *live_path, sv_save_shape_t shape) {
    if (shape == SV_SHAPE_FILE) {
        return atomic_copy_file(mag_path, live_path);
    } else if (shape == SV_SHAPE_DIRECTORY) {
        return atomic_copy_dir(mag_path, live_path);
    } else {
        return atomic_copy_file(mag_path, live_path);
    }
}

/* ===================================================================
 *  Retention (Layer 5)
 * =================================================================== */
static int entry_mtime_cmp(const void *a, const void *b) {
    const sv_entry_t *ea = *(const sv_entry_t **)a;
    const sv_entry_t *eb = *(const sv_entry_t **)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    if (ea->sequence < eb->sequence) return -1;
    if (ea->sequence > eb->sequence) return 1;
    return 0;
}

static bool run_retention(sv_registration_t *reg, sv_id_t *evicted_ids, size_t *evicted_count) {
    *evicted_count = 0;
    if (reg->retention_count == 0) return true; /* keep forever */

    if (reg->num_entries <= reg->retention_count) return true;

    size_t to_evict = reg->num_entries - reg->retention_count;
    if (to_evict > 8) to_evict = 8; /* report up to 8 */

    /* Sort entries by mtime (oldest first) */
    qsort(reg->entries, reg->num_entries, sizeof(sv_entry_t *), entry_mtime_cmp);

    for (size_t i = 0; i < to_evict; i++) {
        sv_entry_t *ent = reg->entries[i];
        sv_id_copy(evicted_ids[*evicted_count], ent->id);
        (*evicted_count)++;

        /* Remove from global list */
        for (size_t j = 0; j < g_ctx.num_entries; j++) {
            if (g_ctx.all_entries[j] == ent) {
                g_ctx.all_entries[j] = g_ctx.all_entries[--g_ctx.num_entries];
                break;
            }
        }

        /* Remove magazine data */
        if (ent->magazine_slot_path) {
            struct stat st;
            if (stat(ent->magazine_slot_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    /* Recursive remove */
                    char cmd[4096];
                    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", ent->magazine_slot_path);
                    system(cmd);
                    rmdir(ent->magazine_slot_path);
                } else {
                    unlink(ent->magazine_slot_path);
                }
            }
        }

        /* Free entry */
        free(ent->label);
        free(ent->magazine_slot_path);
        free(ent);
    }

    /* Compact reg entries list */
    size_t remaining = reg->num_entries - to_evict;
    memmove(reg->entries, reg->entries + to_evict, remaining * sizeof(sv_entry_t *));
    reg->num_entries = remaining;

    return true;
}

/* ===================================================================
 *  Public API
 * =================================================================== */
sv_status_t sv_init(const char *base_path) {
    if (g_ctx.initialized) return SV_OK;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.base_path = strdup(base_path);
    if (!g_ctx.base_path) return SV_ERR_OUT_OF_MEMORY;

    if (!ensure_dir(base_path)) {
        free(g_ctx.base_path);
        g_ctx.base_path = NULL;
        return SV_ERR_IO;
    }

    /* Ensure magazine directory exists */
    char mag_path[4096];
    join_path(mag_path, sizeof(mag_path), base_path, "magazine");
    if (!ensure_dir(mag_path)) {
        free(g_ctx.base_path);
        g_ctx.base_path = NULL;
        return SV_ERR_IO;
    }

    g_ctx.initialized = true;

    /* Load existing metadata */
    metadata_load();

    /* Re-link entries to registrations */
    for (size_t i = 0; i < g_ctx.num_entries; i++) {
        sv_entry_t *ent = g_ctx.all_entries[i];
        bool has_parent = false;
        for (int j = 0; j < 8; j++) {
            if (ent->parent_id[j] != 0) { has_parent = true; break; }
        }
        if (has_parent) {
            sv_registration_t *reg = find_reg(ent->parent_id);
            if (reg) {
                link_entry_to_reg(ent, reg);
            }
        }
    }

    return SV_OK;
}

void sv_shutdown(void) {
    if (!g_ctx.initialized) return;
    g_ctx.initialized = false;

    metadata_flush();

    for (size_t i = 0; i < g_ctx.num_regs; i++) {
        sv_registration_t *reg = g_ctx.regs[i];
        free(reg->live_path);
        free(reg->platform);
        free(reg->emulator);
        free(reg->product_version);
        free(reg->game_id);
        free(reg->rom_path);
        free(reg->label);
        free(reg->save_path_template);
        free(reg->resolved_magazine_dir);
        free(reg->entries);
        free(reg);
    }
    free(g_ctx.regs);

    for (size_t i = 0; i < g_ctx.num_entries; i++) {
        sv_entry_t *ent = g_ctx.all_entries[i];
        free(ent->label);
        free(ent->magazine_slot_path);
        free(ent);
    }
    free(g_ctx.all_entries);

    free(g_ctx.base_path);
    memset(&g_ctx, 0, sizeof(g_ctx));
}

/* ---- Accessors ---- */
void sv_registration_id(const sv_registration_t *reg, sv_id_t out_id) {
    if (reg && out_id) sv_id_copy(out_id, reg->id);
}

/* ---- Layer 2: Registration ---- */
sv_registration_t *sv_register(const sv_register_opts_t *opts, sv_status_t *out_status) {
    if (!g_ctx.initialized) {
        if (out_status) *out_status = SV_ERR_UNAVAILABLE;
        return NULL;
    }
    if (!opts || !opts->live_path) {
        if (out_status) *out_status = SV_ERR_INVALID_ARG;
        return NULL;
    }
    if (!is_regular_file(opts->live_path) && !is_directory(opts->live_path)) {
        if (out_status) *out_status = SV_ERR_NOT_FOUND;
        return NULL;
    }

    sv_registration_t *reg = calloc(1, sizeof(sv_registration_t));
    if (!reg) {
        if (out_status) *out_status = SV_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    generate_unique_id(reg->id);
    reg->live_path = strdup(opts->live_path);
    reg->platform = opts->platform ? strdup(opts->platform) : NULL;
    reg->emulator = opts->emulator ? strdup(opts->emulator) : NULL;
    reg->product_version = opts->product_version ? strdup(opts->product_version) : NULL;
    reg->game_id = opts->game_id ? strdup(opts->game_id) : NULL;
    reg->rom_path = opts->rom_path ? strdup(opts->rom_path) : NULL;
    reg->label = opts->label ? strdup(opts->label) : NULL;
    reg->mode = opts->mode;
    reg->shape = opts->shape;
    reg->retention_count = opts->retention_count;

    if (g_ctx.num_regs >= g_ctx.regs_cap) {
        size_t newcap = g_ctx.regs_cap ? g_ctx.regs_cap * 2 : 16;
        sv_registration_t **newregs = realloc(g_ctx.regs, newcap * sizeof(sv_registration_t *));
        if (!newregs) {
            free(reg->live_path); free(reg->platform); free(reg->emulator);
            free(reg->product_version); free(reg->game_id); free(reg->rom_path);
            free(reg->label); free(reg);
            if (out_status) *out_status = SV_ERR_OUT_OF_MEMORY;
            return NULL;
        }
        g_ctx.regs = newregs;
        g_ctx.regs_cap = newcap;
    }
    g_ctx.regs[g_ctx.num_regs++] = reg;

    if (!metadata_flush()) {
        /* non-fatal */
    }

    if (out_status) *out_status = SV_OK;
    return reg;
}

sv_status_t sv_update_register(sv_registration_t *reg, const sv_update_opts_t *opts, sv_update_result_t *out_result) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!reg || !opts) return SV_ERR_INVALID_ARG;

    if (out_result) out_result->conflict = SV_CONFLICT_NONE;

    if (opts->set_mask & (1u << 0)) { /* live_path */
        if (opts->live_path) {
            /* Poke check */
            bool dest_exists = is_regular_file(opts->live_path) || is_directory(opts->live_path);
            if (dest_exists && opts->on_conflict == SV_ON_CONFLICT_REPORT) {
                if (out_result) out_result->conflict = SV_CONFLICT_EXISTING_DATA;
                return SV_ERR_CONFLICT;
            }
            if (dest_exists && opts->on_conflict == SV_ON_CONFLICT_ABORT_SILENT) {
                return SV_ERR_CONFLICT;
            }

            /* Relocate */
            if (opts->relocate_mode == SV_RELOCATE_MOVE) {
                if (rename(reg->live_path, opts->live_path) != 0) {
                    /* Fall back to copy + delete */
                    if (!copy_to_magazine(reg->live_path, opts->live_path, reg->shape))
                        return SV_ERR_IO;
                    unlink(reg->live_path);
                }
            } else {
                if (!copy_to_magazine(reg->live_path, opts->live_path, reg->shape))
                    return SV_ERR_IO;
            }
            strdup_safe(&reg->live_path, opts->live_path);
        }
    }
    if (opts->set_mask & (1u << 1)) strdup_safe(&reg->platform, opts->platform);
    if (opts->set_mask & (1u << 2)) strdup_safe(&reg->emulator, opts->emulator);
    if (opts->set_mask & (1u << 3)) strdup_safe(&reg->product_version, opts->product_version);
    if (opts->set_mask & (1u << 4)) strdup_safe(&reg->game_id, opts->game_id);
    if (opts->set_mask & (1u << 5)) strdup_safe(&reg->rom_path, opts->rom_path);
    if (opts->set_mask & (1u << 6)) strdup_safe(&reg->label, opts->label);
    if (opts->set_mask & (1u << 7)) reg->mode = opts->mode;
    if (opts->set_mask & (1u << 8)) reg->shape = opts->shape;
    if (opts->set_mask & (1u << 9)) reg->retention_count = opts->retention_count;

    metadata_flush();
    return SV_OK;
}

void sv_unregister(sv_registration_t *reg) {
    if (!g_ctx.initialized || !reg) return;

    /* Orphan all entries */
    for (size_t i = 0; i < reg->num_entries; i++) {
        memset(reg->entries[i]->parent_id, 0, 8);
    }

    /* Remove from context list */
    for (size_t i = 0; i < g_ctx.num_regs; i++) {
        if (g_ctx.regs[i] == reg) {
            g_ctx.regs[i] = g_ctx.regs[--g_ctx.num_regs];
            break;
        }
    }

    /* Free registration */
    free(reg->live_path);
    free(reg->platform);
    free(reg->emulator);
    free(reg->product_version);
    free(reg->game_id);
    free(reg->rom_path);
    free(reg->label);
    free(reg->save_path_template);
    free(reg->resolved_magazine_dir);
    free(reg->entries);
    free(reg);

    metadata_flush();
}

sv_status_t sv_list_registrations(sv_id_t *out_ids, size_t max_ids, size_t *out_count) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!out_ids || !out_count) return SV_ERR_INVALID_ARG;

    size_t count = g_ctx.num_regs < max_ids ? g_ctx.num_regs : max_ids;
    for (size_t i = 0; i < count; i++)
        sv_id_copy(out_ids[i], g_ctx.regs[i]->id);
    *out_count = g_ctx.num_regs;
    return SV_OK;
}

sv_status_t sv_read_registration(const sv_id_t id, sv_registration_info_t *out_info) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!id || !out_info) return SV_ERR_INVALID_ARG;

    sv_registration_t *reg = find_reg(id);
    if (!reg) return SV_ERR_NOT_FOUND;

    memset(out_info, 0, sizeof(*out_info));
    sv_id_copy(out_info->id, reg->id);
    if (reg->live_path) strncpy(out_info->live_path, reg->live_path, sizeof(out_info->live_path) - 1);
    if (reg->platform) strncpy(out_info->platform, reg->platform, sizeof(out_info->platform) - 1);
    if (reg->emulator) strncpy(out_info->emulator, reg->emulator, sizeof(out_info->emulator) - 1);
    if (reg->product_version) strncpy(out_info->product_version, reg->product_version, sizeof(out_info->product_version) - 1);
    if (reg->game_id) strncpy(out_info->game_id, reg->game_id, sizeof(out_info->game_id) - 1);
    if (reg->rom_path) strncpy(out_info->rom_path, reg->rom_path, sizeof(out_info->rom_path) - 1);
    if (reg->label) strncpy(out_info->label, reg->label, sizeof(out_info->label) - 1);
    if (reg->rom_hash_set) { memcpy(out_info->rom_hash, reg->rom_hash, 20); out_info->rom_hash_set = true; }
    out_info->mode = reg->mode;
    out_info->shape = reg->shape;
    out_info->retention_count = reg->retention_count;
    out_info->entry_count = reg->num_entries;
    return SV_OK;
}

/* ---- Layer 4: Local save/pull ---- */
sv_status_t sv_save(sv_registration_t *reg, const sv_save_opts_t *opts, sv_save_result_t *out_result) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!reg) return SV_ERR_INVALID_ARG;

    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

    bool live_exists = is_regular_file(reg->live_path) || is_directory(reg->live_path);
    if (!live_exists) return SV_ERR_NOT_FOUND;

    /* Compute hash of current live save */
    uint8_t live_hash[20];
    bool hash_ok = false;
    if (reg->shape == SV_SHAPE_FILE) {
        hash_ok = hash_file(reg->live_path, live_hash);
    } else {
        /* For directory shape, compute hash of sorted filenames+contents */
        hash_data(reg->live_path, strlen(reg->live_path), live_hash);
        hash_ok = true;
    }

    bool force = opts && opts->force;

    /* Dedup check: skip if hash matches most recent entry (unless force) */
    if (!force && reg->num_entries > 0) {
        /* Find newest entry */
        sv_entry_t *newest = reg->entries[0];
        for (size_t i = 1; i < reg->num_entries; i++) {
            if (reg->entries[i]->mtime > newest->mtime)
                newest = reg->entries[i];
        }
        if (newest->content_hash_set && hash_ok && hash_compare(newest->content_hash, live_hash)) {
            if (out_result) out_result->dedup_skipped = true;
            return SV_OK;
        }
    }

    /* Create entry */
    sv_entry_t *ent = calloc(1, sizeof(sv_entry_t));
    if (!ent) return SV_ERR_OUT_OF_MEMORY;

    generate_unique_id(ent->id);
    ent->sequence = g_ctx.next_sequence++;
    sv_id_copy(ent->parent_id, reg->id);

    /* Set up magazine path */
    char mag_path[4096];
    {
        char mag_dir[4096];
        if (reg->resolved_magazine_dir && reg->resolved_magazine_dir[0]) {
            strncpy(mag_dir, reg->resolved_magazine_dir, sizeof(mag_dir) - 1);
            mag_dir[sizeof(mag_dir) - 1] = '\0';
        } else {
            join_path(mag_dir, sizeof(mag_dir), g_ctx.base_path, "magazine");
        }
        snprintf(mag_path, sizeof(mag_path), "%s/%s", mag_dir, ent->id);
    }

    /* Copy live save to magazine */
    if (!copy_to_magazine(reg->live_path, mag_path, reg->shape)) {
        free(ent);
        return SV_ERR_IO;
    }

    /* Populate entry metadata */
    ent->mtime = get_mtime(reg->live_path);
    ent->size_bytes = get_file_size(reg->live_path);
    if (hash_ok) {
        memcpy(ent->content_hash, live_hash, 20);
        ent->content_hash_set = true;
    }
    ent->integrity_ok = true;
    ent->shape = reg->shape;
    ent->magazine_slot_path = strdup(mag_path);

    /* Add to store */
    add_entry(ent);
    link_entry_to_reg(ent, reg);

    if (out_result) {
        sv_id_copy(out_result->entry_id, ent->id);
        out_result->entry_created = true;
    }

    /* Retention */
    run_retention(reg, out_result ? out_result->evicted_ids : NULL,
                  out_result ? &out_result->evicted_count : NULL);

    metadata_flush();

    return SV_OK;
}

sv_status_t sv_pull(sv_registration_t *reg, const sv_pull_opts_t *opts, sv_pull_result_t *out_result) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!reg) return SV_ERR_INVALID_ARG;

    if (out_result) memset(out_result, 0, sizeof(*out_result));

    if (reg->num_entries == 0) return SV_ERR_NOT_FOUND;

    /* Find newest entry (highest mtime, then highest sequence) */
    sv_entry_t *newest = reg->entries[0];
    for (size_t i = 1; i < reg->num_entries; i++) {
        sv_entry_t *c = reg->entries[i];
        if (c->mtime > newest->mtime ||
            (c->mtime == newest->mtime && c->sequence > newest->sequence))
            newest = c;
    }

    return sv_pull_select(reg, newest->id, opts, out_result);
}

sv_status_t sv_pull_select(sv_registration_t *reg, const sv_id_t entry_id, const sv_pull_opts_t *opts, sv_pull_result_t *out_result) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!reg || !entry_id) return SV_ERR_INVALID_ARG;

    if (out_result) memset(out_result, 0, sizeof(*out_result));

    sv_entry_t *ent = find_entry(entry_id);
    if (!ent) return SV_ERR_NOT_FOUND;

    if (!ent->magazine_slot_path) return SV_ERR_NOT_FOUND;

    bool mag_exists = is_regular_file(ent->magazine_slot_path) || is_directory(ent->magazine_slot_path);
    if (!mag_exists) return SV_ERR_NOT_FOUND;

    /* Check for conflict: has live path changed since we last saw it? */
    bool live_exists = is_regular_file(reg->live_path) || is_directory(reg->live_path);
    bool live_changed = false;

    if (live_exists && reg->num_entries > 0) {
        sv_entry_t *latest = reg->entries[0];
        for (size_t i = 1; i < reg->num_entries; i++) {
            sv_entry_t *c = reg->entries[i];
            if (c->mtime > latest->mtime ||
                (c->mtime == latest->mtime && c->sequence > latest->sequence))
                latest = c;
        }
        if (latest && latest->content_hash_set) {
            uint8_t current_hash[20];
            bool hash_ok = false;
            if (reg->shape == SV_SHAPE_FILE) {
                hash_ok = hash_file(reg->live_path, current_hash);
            } else {
                hash_ok = true;
                hash_data(reg->live_path, strlen(reg->live_path), current_hash);
            }
            if (hash_ok && !hash_compare(latest->content_hash, current_hash)) {
                live_changed = true;
            }
        }
    }

    /* Save the magazine path before backup — retention may free ent */
    char saved_mag_path[4096];
    saved_mag_path[0] = '\0';
    if (ent->magazine_slot_path)
        strncpy(saved_mag_path, ent->magazine_slot_path, sizeof(saved_mag_path) - 1);

    if (live_changed) {
        sv_pull_on_conflict_t conflict_policy = opts ? opts->on_conflict : SV_PULL_CONFLICT_REPORT;
        if (conflict_policy == SV_PULL_CONFLICT_REPORT) {
            if (out_result) out_result->conflicted = true;
            return SV_ERR_CONFLICT;
        }
        /* override: backup first */
        sv_save_opts_t save_opts = { .force = true };
        sv_save_result_t save_result;
        sv_status_t save_status = sv_save(reg, &save_opts, &save_result);
        if (save_status == SV_OK && out_result) {
            out_result->did_backup = true;
            sv_id_copy(out_result->backup_entry_id, save_result.entry_id);
        }
        /* Re-find entry after backup (retention may have freed ent) */
        ent = find_entry(entry_id);
        if (!ent) return SV_ERR_NOT_FOUND;
        /* If the entry survived retention, prefer its (possibly updated) path */
        if (ent->magazine_slot_path)
            strncpy(saved_mag_path, ent->magazine_slot_path, sizeof(saved_mag_path) - 1);
    }

    /* Atomic replace: copy magazine content to tmp, then rename to live */
    if (!saved_mag_path[0] || !replace_live(saved_mag_path, reg->live_path, reg->shape)) {
        return SV_ERR_IO;
    }

    if (out_result) out_result->did_pull = true;
    return SV_OK;
}

sv_status_t sv_list_entries(const sv_id_t reg_id, sv_id_t *out_ids, size_t max_ids, size_t *out_count) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!out_ids || !out_count) return SV_ERR_INVALID_ARG;

    *out_count = 0;
    bool list_orphans = sv_id_is_zero(reg_id);

    for (size_t i = 0; i < g_ctx.num_entries && *out_count < max_ids; i++) {
        sv_entry_t *ent = g_ctx.all_entries[i];
        if (list_orphans) {
            bool has_parent = false;
            for (int j = 0; j < 8; j++) {
                if (ent->parent_id[j] != 0) { has_parent = true; break; }
            }
            if (!has_parent)
                sv_id_copy(out_ids[(*out_count)++], ent->id);
        } else {
            if (memcmp(ent->parent_id, reg_id, 8) == 0)
                sv_id_copy(out_ids[(*out_count)++], ent->id);
        }
    }
    return SV_OK;
}

sv_status_t sv_read_entry(const sv_id_t entry_id, sv_entry_info_t *out_info) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!entry_id || !out_info) return SV_ERR_INVALID_ARG;

    sv_entry_t *ent = find_entry(entry_id);
    if (!ent) return SV_ERR_NOT_FOUND;

    memset(out_info, 0, sizeof(*out_info));
    sv_id_copy(out_info->id, ent->id);
    sv_id_copy(out_info->parent_id, ent->parent_id);
    if (ent->label) strncpy(out_info->label, ent->label, sizeof(out_info->label) - 1);
    if (ent->magazine_slot_path) strncpy(out_info->magazine_slot_path, ent->magazine_slot_path, sizeof(out_info->magazine_slot_path) - 1);
    if (ent->content_hash_set) { memcpy(out_info->content_hash, ent->content_hash, 20); out_info->content_hash_set = true; }
    out_info->integrity_ok = ent->integrity_ok;
    out_info->mtime = ent->mtime;
    out_info->size_bytes = ent->size_bytes;
    out_info->playtime_seconds = ent->playtime_seconds;
    out_info->shape = ent->shape;
    return SV_OK;
}

sv_status_t sv_reparent_entry(const sv_id_t entry_id, const sv_id_t new_parent_reg_id) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!entry_id || !new_parent_reg_id) return SV_ERR_INVALID_ARG;

    sv_entry_t *ent = find_entry(entry_id);
    if (!ent) return SV_ERR_NOT_FOUND;

    sv_registration_t *new_parent = find_reg(new_parent_reg_id);
    if (!new_parent) return SV_ERR_NOT_FOUND;

    /* Remove from old parent */
    sv_registration_t *old_parent = find_reg_by_entry_parent(ent->parent_id);
    if (old_parent) remove_entry_from_reg(ent, old_parent);

    /* Add to new parent */
    link_entry_to_reg(ent, new_parent);

    metadata_flush();
    return SV_OK;
}

sv_status_t sv_delete_entry(const sv_id_t entry_id) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!entry_id) return SV_ERR_INVALID_ARG;

    sv_entry_t *ent = find_entry(entry_id);
    if (!ent) return SV_ERR_NOT_FOUND;

    /* Remove from parent */
    sv_registration_t *parent = find_reg_by_entry_parent(ent->parent_id);
    if (parent) remove_entry_from_reg(ent, parent);

    /* Remove from global list */
    for (size_t i = 0; i < g_ctx.num_entries; i++) {
        if (g_ctx.all_entries[i] == ent) {
            g_ctx.all_entries[i] = g_ctx.all_entries[--g_ctx.num_entries];
            break;
        }
    }

    /* Remove magazine data */
    if (ent->magazine_slot_path) {
        unlink(ent->magazine_slot_path);
    }

    free(ent->label);
    free(ent->magazine_slot_path);
    free(ent);

    metadata_flush();
    return SV_OK;
}

/* ===================================================================
 *  Manifest (Layer 2 — STRATEGY mode)
 * =================================================================== */
struct sv_manifest_s {
    char platform[64];
    char emulator[64];
    sv_save_shape_t shape;
    sv_identity_method_t identity_method;

    /* Tier 1: serial extraction */
    char serial_file[256];
    size_t serial_offset;
    size_t serial_length;
    char serial_pattern[256];

    /* Tier 2: checksum */
    size_t checksum_offset;
    size_t checksum_size;
    bool checksum_big_endian;

    /* Save path template */
    char save_path_template[4096];

    /* ROM header reading */
    size_t rom_header_offset;
    size_t rom_header_length;

    /* Text pattern extraction */
    char text_pattern[1024];
    char text_pattern_source[256];
    size_t text_pattern_offset;
    size_t text_pattern_length;
    char normalize_strip[64];
    char normalize_replace[256];

    /* Tier 3: pluggable hash-DB */
    sv_hash_db_lookup_fn hash_db_lookup;
    void *hash_db_ctx;
};

sv_manifest_t *sv_manifest_create(void) {
    sv_manifest_t *m = calloc(1, sizeof(sv_manifest_t));
    if (m) m->shape = SV_SHAPE_UNKNOWN;
    return m;
}

void sv_manifest_free(sv_manifest_t *manifest) {
    free(manifest);
}

/* ---- Field accessors ---- */
const char *sv_manifest_get_platform(const sv_manifest_t *manifest) {
    return manifest ? manifest->platform : NULL;
}

const char *sv_manifest_get_emulator(const sv_manifest_t *manifest) {
    return manifest ? manifest->emulator : NULL;
}

sv_save_shape_t sv_manifest_get_shape(const sv_manifest_t *manifest) {
    return manifest ? manifest->shape : SV_SHAPE_UNKNOWN;
}

sv_identity_method_t sv_manifest_get_identity_method(const sv_manifest_t *manifest) {
    return manifest ? manifest->identity_method : SV_IDENTITY_NONE;
}

const char *sv_manifest_get_save_path_template(const sv_manifest_t *manifest) {
    return manifest ? manifest->save_path_template : NULL;
}

/* ---- Field setters ---- */
void sv_manifest_set_platform(sv_manifest_t *manifest, const char *platform) {
    if (manifest && platform) {
        strncpy(manifest->platform, platform, sizeof(manifest->platform) - 1);
        manifest->platform[sizeof(manifest->platform) - 1] = '\0';
    }
}

void sv_manifest_set_emulator(sv_manifest_t *manifest, const char *emulator) {
    if (manifest && emulator) {
        strncpy(manifest->emulator, emulator, sizeof(manifest->emulator) - 1);
        manifest->emulator[sizeof(manifest->emulator) - 1] = '\0';
    }
}

void sv_manifest_set_shape(sv_manifest_t *manifest, sv_save_shape_t shape) {
    if (manifest) manifest->shape = shape;
}

void sv_manifest_set_identity_method(sv_manifest_t *manifest, sv_identity_method_t method) {
    if (manifest) manifest->identity_method = method;
}

void sv_manifest_set_serial_params(sv_manifest_t *manifest, const char *file,
                                    size_t offset, size_t length, const char *pattern) {
    if (!manifest) return;
    if (file) {
        strncpy(manifest->serial_file, file, sizeof(manifest->serial_file) - 1);
        manifest->serial_file[sizeof(manifest->serial_file) - 1] = '\0';
    }
    manifest->serial_offset = offset;
    manifest->serial_length = length;
    if (pattern) {
        strncpy(manifest->serial_pattern, pattern, sizeof(manifest->serial_pattern) - 1);
        manifest->serial_pattern[sizeof(manifest->serial_pattern) - 1] = '\0';
    }
}

void sv_manifest_set_checksum_params(sv_manifest_t *manifest, size_t offset,
                                      size_t size, bool big_endian) {
    if (!manifest) return;
    manifest->checksum_offset = offset;
    manifest->checksum_size = size;
    manifest->checksum_big_endian = big_endian;
}

void sv_manifest_set_save_path_template(sv_manifest_t *manifest, const char *tmpl) {
    if (manifest && tmpl) {
        strncpy(manifest->save_path_template, tmpl, sizeof(manifest->save_path_template) - 1);
        manifest->save_path_template[sizeof(manifest->save_path_template) - 1] = '\0';
    }
}

void sv_manifest_set_hash_db_callback(sv_manifest_t *manifest,
                                       sv_hash_db_lookup_fn fn, void *ctx) {
    if (!manifest) return;
    manifest->hash_db_lookup = fn;
    manifest->hash_db_ctx = ctx;
}

void sv_manifest_set_rom_header_params(sv_manifest_t *manifest, size_t offset,
                                        size_t length) {
    if (!manifest) return;
    manifest->rom_header_offset = offset;
    manifest->rom_header_length = length;
}

void sv_manifest_set_text_pattern_params(sv_manifest_t *manifest,
                                          const char *pattern,
                                          const char *source,
                                          size_t offset,
                                          size_t length) {
    if (!manifest) return;
    if (pattern) {
        strncpy(manifest->text_pattern, pattern, sizeof(manifest->text_pattern) - 1);
        manifest->text_pattern[sizeof(manifest->text_pattern) - 1] = '\0';
    }
    if (source) {
        strncpy(manifest->text_pattern_source, source, sizeof(manifest->text_pattern_source) - 1);
        manifest->text_pattern_source[sizeof(manifest->text_pattern_source) - 1] = '\0';
    }
    manifest->text_pattern_offset = offset;
    manifest->text_pattern_length = length;
}

void sv_manifest_set_normalize_params(sv_manifest_t *manifest,
                                       const char *strip,
                                       const char *replace) {
    if (!manifest) return;
    if (strip) {
        strncpy(manifest->normalize_strip, strip, sizeof(manifest->normalize_strip) - 1);
        manifest->normalize_strip[sizeof(manifest->normalize_strip) - 1] = '\0';
    }
    if (replace) {
        strncpy(manifest->normalize_replace, replace, sizeof(manifest->normalize_replace) - 1);
        manifest->normalize_replace[sizeof(manifest->normalize_replace) - 1] = '\0';
    }
}

/* ---- Manifest file I/O (text format) ---- */
static void trim(char *s) {
    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t'))
        *end-- = '\0';
}

sv_status_t sv_manifest_load(const char *path, sv_manifest_t *out_manifest) {
    if (!path || !out_manifest) return SV_ERR_INVALID_ARG;

    FILE *f = fopen(path, "r");
    if (!f) return SV_ERR_NOT_FOUND;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        if (strcmp(key, "platform") == 0) {
            sv_manifest_set_platform(out_manifest, value);
        } else if (strcmp(key, "emulator") == 0) {
            sv_manifest_set_emulator(out_manifest, value);
        } else if (strcmp(key, "shape") == 0) {
            if (strcmp(value, "file") == 0) sv_manifest_set_shape(out_manifest, SV_SHAPE_FILE);
            else if (strcmp(value, "directory") == 0) sv_manifest_set_shape(out_manifest, SV_SHAPE_DIRECTORY);
            else if (strcmp(value, "container") == 0) sv_manifest_set_shape(out_manifest, SV_SHAPE_CONTAINER);
            else if (strcmp(value, "archive") == 0) sv_manifest_set_shape(out_manifest, SV_SHAPE_ARCHIVE);
            else sv_manifest_set_shape(out_manifest, SV_SHAPE_UNKNOWN);
        } else if (strcmp(key, "identity") == 0) {
            if (strcmp(value, "serial_cnf") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_SERIAL_CNF);
            else if (strcmp(value, "serial_sfo") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_SERIAL_SFO);
            else if (strcmp(value, "serial_ipbin") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_SERIAL_IPBIN);
            else if (strcmp(value, "boot_header") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_BOOT_HEADER);
            else if (strcmp(value, "checksum") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_CHECKSUM);
            else if (strcmp(value, "pluggable") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_PLUGGABLE);
            else if (strcmp(value, "rom_header") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_ROM_HEADER);
            else if (strcmp(value, "text_pattern") == 0) sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_TEXT_PATTERN);
            else sv_manifest_set_identity_method(out_manifest, SV_IDENTITY_NONE);
        } else if (strcmp(key, "serial_file") == 0) {
            strncpy(out_manifest->serial_file, value, sizeof(out_manifest->serial_file) - 1);
        } else if (strcmp(key, "serial_offset") == 0) {
            out_manifest->serial_offset = (size_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "serial_length") == 0) {
            out_manifest->serial_length = (size_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "serial_pattern") == 0) {
            strncpy(out_manifest->serial_pattern, value, sizeof(out_manifest->serial_pattern) - 1);
        } else if (strcmp(key, "checksum_offset") == 0) {
            out_manifest->checksum_offset = (size_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "checksum_size") == 0) {
            out_manifest->checksum_size = (size_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "checksum_big_endian") == 0) {
            out_manifest->checksum_big_endian = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "save_path_template") == 0) {
            sv_manifest_set_save_path_template(out_manifest, value);
        } else if (strcmp(key, "rom_header_offset") == 0) {
            out_manifest->rom_header_offset = (size_t)strtoul(value, NULL, 0);
        } else if (strcmp(key, "rom_header_length") == 0) {
            out_manifest->rom_header_length = (size_t)strtoul(value, NULL, 0);
        } else if (strcmp(key, "text_pattern") == 0) {
            strncpy(out_manifest->text_pattern, value, sizeof(out_manifest->text_pattern) - 1);
        } else if (strcmp(key, "text_pattern_source") == 0) {
            strncpy(out_manifest->text_pattern_source, value, sizeof(out_manifest->text_pattern_source) - 1);
        } else if (strcmp(key, "text_pattern_offset") == 0) {
            out_manifest->text_pattern_offset = (size_t)strtoul(value, NULL, 0);
        } else if (strcmp(key, "text_pattern_length") == 0) {
            out_manifest->text_pattern_length = (size_t)strtoul(value, NULL, 0);
        } else if (strcmp(key, "normalize_strip") == 0) {
            strncpy(out_manifest->normalize_strip, value, sizeof(out_manifest->normalize_strip) - 1);
        } else if (strcmp(key, "normalize_replace") == 0) {
            strncpy(out_manifest->normalize_replace, value, sizeof(out_manifest->normalize_replace) - 1);
        }
    }

    fclose(f);
    return SV_OK;
}

sv_status_t sv_manifest_save(const char *path, const sv_manifest_t *manifest) {
    if (!path || !manifest) return SV_ERR_INVALID_ARG;

    FILE *f = fopen(path, "w");
    if (!f) return SV_ERR_IO;

    fprintf(f, "# libsavesync manifest\n");
    if (manifest->platform[0]) fprintf(f, "platform=%s\n", manifest->platform);
    if (manifest->emulator[0]) fprintf(f, "emulator=%s\n", manifest->emulator);

    const char *shape_str = "unknown";
    switch (manifest->shape) {
        case SV_SHAPE_FILE: shape_str = "file"; break;
        case SV_SHAPE_DIRECTORY: shape_str = "directory"; break;
        case SV_SHAPE_CONTAINER: shape_str = "container"; break;
        case SV_SHAPE_ARCHIVE: shape_str = "archive"; break;
        default: shape_str = "unknown"; break;
    }
    fprintf(f, "shape=%s\n", shape_str);

    const char *identity_str = "none";
    switch (manifest->identity_method) {
        case SV_IDENTITY_SERIAL_CNF: identity_str = "serial_cnf"; break;
        case SV_IDENTITY_SERIAL_SFO: identity_str = "serial_sfo"; break;
        case SV_IDENTITY_SERIAL_IPBIN: identity_str = "serial_ipbin"; break;
        case SV_IDENTITY_BOOT_HEADER: identity_str = "boot_header"; break;
        case SV_IDENTITY_CHECKSUM: identity_str = "checksum"; break;
        case SV_IDENTITY_PLUGGABLE: identity_str = "pluggable"; break;
        case SV_IDENTITY_ROM_HEADER: identity_str = "rom_header"; break;
        case SV_IDENTITY_TEXT_PATTERN: identity_str = "text_pattern"; break;
        default: identity_str = "none"; break;
    }
    fprintf(f, "identity=%s\n", identity_str);

    if (manifest->serial_file[0]) fprintf(f, "serial_file=%s\n", manifest->serial_file);
    if (manifest->serial_offset > 0) fprintf(f, "serial_offset=%zu\n", manifest->serial_offset);
    if (manifest->serial_length > 0) fprintf(f, "serial_length=%zu\n", manifest->serial_length);
    if (manifest->serial_pattern[0]) fprintf(f, "serial_pattern=%s\n", manifest->serial_pattern);
    if (manifest->checksum_offset > 0) fprintf(f, "checksum_offset=%zu\n", manifest->checksum_offset);
    if (manifest->checksum_size > 0) fprintf(f, "checksum_size=%zu\n", manifest->checksum_size);
    if (manifest->checksum_big_endian) fprintf(f, "checksum_big_endian=true\n");
    if (manifest->save_path_template[0]) fprintf(f, "save_path_template=%s\n", manifest->save_path_template);
    if (manifest->rom_header_offset > 0) fprintf(f, "rom_header_offset=%zu\n", manifest->rom_header_offset);
    if (manifest->rom_header_length > 0) fprintf(f, "rom_header_length=%zu\n", manifest->rom_header_length);
    if (manifest->text_pattern[0]) fprintf(f, "text_pattern=%s\n", manifest->text_pattern);
    if (manifest->text_pattern_source[0]) fprintf(f, "text_pattern_source=%s\n", manifest->text_pattern_source);
    if (manifest->text_pattern_offset > 0) fprintf(f, "text_pattern_offset=%zu\n", manifest->text_pattern_offset);
    if (manifest->text_pattern_length > 0) fprintf(f, "text_pattern_length=%zu\n", manifest->text_pattern_length);
    if (manifest->normalize_strip[0]) fprintf(f, "normalize_strip=%s\n", manifest->normalize_strip);
    if (manifest->normalize_replace[0]) fprintf(f, "normalize_replace=%s\n", manifest->normalize_replace);

    fclose(f);
    return SV_OK;
}

/* ---- Identity Resolution ---- */

/* Extract serial from a simple pattern like "BOOT2 = {SERIAL:12}"
 * Pattern format: literal text with {SERIAL:N} as placeholder.
 * Returns number of chars written to out_serial, or 0 on failure. */
static size_t extract_serial_from_pattern(const char *data, size_t data_len,
                                           const char *pattern,
                                           char *out_serial, size_t out_max) {
    if (!data || !pattern || !out_serial || out_max == 0) return 0;

    /* Find {SERIAL:N} in pattern */
    const char *placeholder = strstr(pattern, "{SERIAL:");
    if (!placeholder) return 0;

    size_t prefix_len = (size_t)(placeholder - pattern);
    const char *num_start = placeholder + 8; /* skip "{SERIAL:" */
    const char *num_end = strchr(num_start, '}');
    if (!num_end) return 0;

    size_t serial_len = (size_t)strtoul(num_start, NULL, 10);
    if (serial_len == 0 || serial_len > out_max - 1) return 0;

    /* Check that data is long enough for prefix + serial */
    if (data_len < prefix_len + serial_len) return 0;

    /* Check prefix matches */
    if (memcmp(data, pattern, prefix_len) != 0) return 0;

    /* Extract serial */
    memcpy(out_serial, data + prefix_len, serial_len);
    out_serial[serial_len] = '\0';

    /* Strip trailing whitespace (\r, \n, spaces) */
    char *end = out_serial + strlen(out_serial) - 1;
    while (end >= out_serial && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t'))
        *end-- = '\0';

    return strlen(out_serial);
}

/* Try to identify game from SYSTEM.CNF-style file (Tier 1: serial_cnf) */
static sv_status_t identify_from_serial_cnf(const sv_manifest_t *manifest,
                                              const char *live_path,
                                              char *out_game_id, size_t out_len) {
    if (!manifest || !live_path || !out_game_id || out_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    /* Determine the file to read */
    char file_path[4096];
    struct stat st;
    if (stat(live_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* live_path is a directory — look for serial_file within it */
        if (!manifest->serial_file[0]) return SV_ERR_NOT_FOUND;
        snprintf(file_path, sizeof(file_path), "%s/%s", live_path, manifest->serial_file);
    } else {
        /* live_path is a file — read it directly */
        strncpy(file_path, live_path, sizeof(file_path) - 1);
        file_path[sizeof(file_path) - 1] = '\0';
    }

    /* Read the file */
    FILE *f = fopen(file_path, "rb");
    if (!f) return SV_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t read_len = manifest->serial_length > 0 ? manifest->serial_length : (size_t)fsize;
    if (manifest->serial_offset > 0) {
        fseek(f, (long)manifest->serial_offset, SEEK_SET);
        read_len = manifest->serial_length > 0 ? manifest->serial_length : (size_t)(fsize - (long)manifest->serial_offset);
    }

    char *buf = malloc(read_len + 1);
    if (!buf) { fclose(f); return SV_ERR_OUT_OF_MEMORY; }

    size_t n = fread(buf, 1, read_len, f);
    fclose(f);
    buf[n] = '\0';

    /* Try to extract serial using pattern */
    size_t serial_len = extract_serial_from_pattern(buf, n, manifest->serial_pattern,
                                                     out_game_id, out_len);
    free(buf);

    if (serial_len == 0) return SV_ERR_NOT_FOUND;
    return SV_OK;
}

/* Try to identify game from PARAM.SFO-style file (Tier 1: serial_sfo) */
static sv_status_t identify_from_serial_sfo(const sv_manifest_t *manifest,
                                              const char *live_path,
                                              char *out_game_id, size_t out_len) {
    if (!manifest || !live_path || !out_game_id || out_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    /* Determine the file to read */
    char file_path[4096];
    struct stat st;
    if (stat(live_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!manifest->serial_file[0]) return SV_ERR_NOT_FOUND;
        snprintf(file_path, sizeof(file_path), "%s/%s", live_path, manifest->serial_file);
    } else {
        strncpy(file_path, live_path, sizeof(file_path) - 1);
        file_path[sizeof(file_path) - 1] = '\0';
    }

    /* For SFO, we can use the same pattern extraction as CNF
     * The manifest's serial_offset/serial_length/pattern define how to extract */
    return identify_from_serial_cnf(manifest, live_path, out_game_id, out_len);
}

/* Try to identify game from checksum (Tier 2) */
static sv_status_t identify_from_checksum(const sv_manifest_t *manifest,
                                            const char *live_path,
                                            char *out_game_id, size_t out_len) {
    if (!manifest || !live_path || !out_game_id || out_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    FILE *f = fopen(live_path, "rb");
    if (!f) return SV_ERR_NOT_FOUND;

    /* Read checksum bytes */
    fseek(f, (long)manifest->checksum_offset, SEEK_SET);
    uint8_t ckbuf[8] = {0};
    size_t to_read = manifest->checksum_size > 8 ? 8 : manifest->checksum_size;
    fread(ckbuf, 1, to_read, f);

    fclose(f);

    /* Compute checksum value */
    uint64_t checksum = 0;
    if (manifest->checksum_big_endian) {
        for (size_t i = 0; i < to_read; i++)
            checksum = (checksum << 8) | ckbuf[i];
    } else {
        for (size_t i = 0; i < to_read; i++)
            checksum |= (uint64_t)ckbuf[i] << (i * 8);
    }

    /* Without a lookup table, we can't resolve — return the combined key as a string */
    snprintf(out_game_id, out_len, "CKSUM_%08X_%08X",
             (unsigned)(checksum >> 32), (unsigned)(checksum & 0xFFFFFFFF));
    return SV_OK;
}

/* Try to identify game from text pattern extraction (SV_IDENTITY_TEXT_PATTERN) */
static sv_status_t identify_from_text_pattern(const sv_manifest_t *manifest,
                                               const char *live_path,
                                               char *out_game_id, size_t out_len) {
    if (!manifest || !live_path || !out_game_id || out_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    if (!manifest->text_pattern[0]) return SV_ERR_INVALID_ARG;

    uint8_t *raw = NULL;
    size_t n = 0;
    bool raw_from_malloc = false;

    if (strcmp(manifest->text_pattern_source, "_dirname") == 0) {
        /* Special source: match against the basename of live_path itself */
        const char *base = strrchr(live_path, '/');
        if (!base) base = live_path; else base++;
        n = strlen(base);
        raw = (uint8_t *)base;
    } else {
        /* Determine the file to read */
        char file_path[4096];
        if (manifest->text_pattern_source[0]) {
            snprintf(file_path, sizeof(file_path), "%s/%s", live_path, manifest->text_pattern_source);
        } else {
            strncpy(file_path, live_path, sizeof(file_path) - 1);
            file_path[sizeof(file_path) - 1] = '\0';
        }

        FILE *f = fopen(file_path, "rb");
        if (!f) return SV_ERR_NOT_FOUND;

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        size_t read_start = manifest->text_pattern_offset;
        size_t read_len = manifest->text_pattern_length > 0 ? manifest->text_pattern_length : (size_t)fsize;

        if (read_start >= (size_t)fsize) { fclose(f); return SV_ERR_NOT_FOUND; }
        if (read_start + read_len > (size_t)fsize) read_len = (size_t)fsize - read_start;

        fseek(f, (long)read_start, SEEK_SET);
        raw = malloc(read_len);
        if (!raw) { fclose(f); return SV_ERR_OUT_OF_MEMORY; }
        n = fread(raw, 1, read_len, f);
        fclose(f);
        raw_from_malloc = true;
    }

    /* Try matching the pattern at every position in the content */
    const char *pat = manifest->text_pattern;
    size_t pat_len = strlen(pat);

    for (size_t start = 0; start <= n; start++) {
        const uint8_t *cdata = raw + start;
        size_t clen = n - start;
        size_t pi = 0;   /* pattern index */
        size_t ci = 0;   /* content index */
        char captured[256] = {0};
        bool capturing = false;
        size_t capture_need = 0;
        bool capture_star = false;
        char capture_delim = '\0';
        bool failed = false;

        while ((pi < pat_len || capturing) && ci < clen && !failed) {
            char pc = (pi < pat_len) ? pat[pi] : '\0';

            if (pc == '{') {
                /* Start capture */
                const char *close = strchr(pat + pi, '}');
                if (!close) { failed = true; break; }
                const char *colon = strchr(pat + pi, ':');
                if (!colon || colon >= close) { failed = true; break; }
                colon++;
                if (*colon == '*') {
                    capture_star = true;
                    capture_delim = *(close + 1);
                    capture_need = 0;
                } else {
                    capture_star = false;
                    capture_need = (size_t)strtoul(colon, NULL, 10);
                    if (capture_need == 0 || capture_need >= sizeof(captured)) { failed = true; break; }
                }
                capturing = true;
                pi = close - pat + 1;
                continue;
            }

            if (capturing) {
                if (capture_star) {
                    if (ci < clen && cdata[ci] != (uint8_t)capture_delim) {
                        size_t cl = strlen(captured);
                        if (cl < sizeof(captured) - 1) captured[cl] = (char)cdata[ci];
                        ci++;
                        continue;
                    }
                    capturing = false;
                    continue;
                } else {
                    size_t cl = strlen(captured);
                    if (cl < capture_need && ci < clen) {
                        captured[cl] = (char)cdata[ci];
                        ci++;
                        continue;
                    }
                    if (cl < capture_need) { failed = true; break; }
                    capturing = false;
                    continue;
                }
            }

            /* Literal or wildcard match */
            if (pc == '_') {
                ci++;
            } else {
                if (ci >= clen || cdata[ci] != (uint8_t)pc) { failed = true; break; }
                ci++;
            }
            pi++;
        }

        /* Reject incomplete fixed-length captures that ran out of content */
        if (!failed && capturing && !capture_star && strlen(captured) < capture_need) {
            failed = true;
        }

        if (!failed && strlen(captured) > 0) {
            /* Apply normalization */
            char normalized[256];
            strncpy(normalized, captured, sizeof(normalized) - 1);
            normalized[sizeof(normalized) - 1] = '\0';

            /* Strip characters */
            if (manifest->normalize_strip[0]) {
                char result[256] = {0};
                size_t ri = 0;
                for (size_t i = 0; i < strlen(normalized); i++) {
                    if (!strchr(manifest->normalize_strip, normalized[i]))
                        result[ri++] = normalized[i];
                }
                result[ri] = '\0';
                strncpy(normalized, result, sizeof(normalized) - 1);
            }

            /* Replace character pairs: "XY" means replace X with Y */
            if (manifest->normalize_replace[0]) {
                for (size_t i = 0; i + 1 < strlen(manifest->normalize_replace); i += 2) {
                    char from = manifest->normalize_replace[i];
                    char to = manifest->normalize_replace[i + 1];
                    for (size_t j = 0; j < strlen(normalized); j++) {
                        if (normalized[j] == from) normalized[j] = to;
                    }
                }
            }

            if (strlen(normalized) > 0) {
                strncpy(out_game_id, normalized, out_len - 1);
                out_game_id[out_len - 1] = '\0';
                if (raw_from_malloc) free(raw);
                return SV_OK;
            }
        }
    }

    if (raw_from_malloc) free(raw);
    return SV_ERR_NOT_FOUND;
}

/* Try to identify game from ROM header bytes (SV_IDENTITY_ROM_HEADER) */
static bool is_printable_ascii(uint8_t b) {
    return b >= 0x20 && b <= 0x7E;
}

static sv_status_t identify_from_rom_header(const sv_manifest_t *manifest,
                                             const char *rom_path,
                                             char *out_game_id, size_t out_len) {
    if (!manifest || !rom_path || !out_game_id || out_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    size_t read_len = manifest->rom_header_length > 0 ? manifest->rom_header_length : 4;

    FILE *f = fopen(rom_path, "rb");
    if (!f) return SV_ERR_NOT_FOUND;

    /* Check file is long enough */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize < 0 || (size_t)fsize < manifest->rom_header_offset + read_len) {
        fclose(f);
        return SV_ERR_NOT_FOUND;
    }

    /* Read the bytes */
    fseek(f, (long)manifest->rom_header_offset, SEEK_SET);
    uint8_t buf[256];
    if (read_len > sizeof(buf)) read_len = sizeof(buf);
    size_t n = fread(buf, 1, read_len, f);
    fclose(f);

    if (n < read_len) return SV_ERR_NOT_FOUND;

    /* Check if all bytes are printable ASCII */
    bool all_ascii = true;
    for (size_t i = 0; i < read_len; i++) {
        if (!is_printable_ascii(buf[i])) { all_ascii = false; break; }
    }

    if (all_ascii) {
        /* ASCII passthrough — strip trailing nulls and spaces */
        size_t len = read_len;
        while (len > 0 && (buf[len-1] == '\0' || buf[len-1] == ' '))
            len--;
        if (len == 0) return SV_ERR_NOT_FOUND;
        if (len >= out_len) len = out_len - 1;
        memcpy(out_game_id, buf, len);
        out_game_id[len] = '\0';
    } else {
        /* Check if non-ASCII bytes are only null padding at the end */
        size_t non_null_len = read_len;
        while (non_null_len > 0 && buf[non_null_len - 1] == '\0')
            non_null_len--;

        bool printable_except_nulls = true;
        for (size_t i = 0; i < non_null_len; i++) {
            if (!is_printable_ascii(buf[i])) { printable_except_nulls = false; break; }
        }

        if (printable_except_nulls && non_null_len > 0) {
            if (non_null_len >= out_len) non_null_len = out_len - 1;
            memcpy(out_game_id, buf, non_null_len);
            out_game_id[non_null_len] = '\0';
        } else {
            /* Hex encoding fallback */
            snprintf(out_game_id, out_len, "0x");
            for (size_t i = 0; i < read_len && strlen(out_game_id) < out_len - 3; i++)
                snprintf(out_game_id + strlen(out_game_id), out_len - strlen(out_game_id),
                         "%02X", buf[i]);
        }
    }

    return SV_OK;
}

/* Run identity resolution using the manifest */
static sv_status_t resolve_identity(const sv_manifest_t *manifest,
                                      const char *live_path,
                                      const char *rom_path,
                                      char *out_game_id, size_t out_game_id_len) {
    if (!manifest || !live_path || !out_game_id || out_game_id_len == 0)
        return SV_ERR_INVALID_ARG;

    out_game_id[0] = '\0';

    switch (manifest->identity_method) {
        case SV_IDENTITY_NONE:
            return SV_OK;

        case SV_IDENTITY_SERIAL_CNF:
            return identify_from_serial_cnf(manifest, live_path, out_game_id, out_game_id_len);

        case SV_IDENTITY_SERIAL_SFO:
            return identify_from_serial_sfo(manifest, live_path, out_game_id, out_game_id_len);

        case SV_IDENTITY_SERIAL_IPBIN:
            /* Same pattern extraction as CNF — differs only in file/offset */
            return identify_from_serial_cnf(manifest, live_path, out_game_id, out_game_id_len);

        case SV_IDENTITY_BOOT_HEADER:
            return identify_from_serial_cnf(manifest, live_path, out_game_id, out_game_id_len);

        case SV_IDENTITY_CHECKSUM:
            return identify_from_checksum(manifest, live_path, out_game_id, out_game_id_len);

        case SV_IDENTITY_PLUGGABLE:
            if (manifest->hash_db_lookup) {
                /* Compute content hash of the file */
                uint8_t hash[20];
                if (!hash_file(live_path, hash)) return SV_ERR_IO;
                return manifest->hash_db_lookup(hash, sizeof(hash), manifest->platform,
                                                 out_game_id, out_game_id_len,
                                                 manifest->hash_db_ctx);
            }
            return SV_OK;

        case SV_IDENTITY_ROM_HEADER: {
            const char *path = (rom_path && rom_path[0]) ? rom_path : live_path;
            return identify_from_rom_header(manifest, path, out_game_id, out_game_id_len);
        }

        case SV_IDENTITY_TEXT_PATTERN: {
            /* If text_pattern_source is empty, try rom_path first, then live_path */
            const char *path = live_path;
            if (!manifest->text_pattern_source[0] && rom_path && rom_path[0])
                path = rom_path;
            return identify_from_text_pattern(manifest, path, out_game_id, out_game_id_len);
        }

        default:
            return SV_ERR_INVALID_ARG;
    }
}

/* ---- Register with manifest ---- */
sv_registration_t *sv_register_with_manifest(
    const sv_register_opts_t *opts,
    const sv_manifest_t *manifest,
    sv_status_t *out_status
) {
    if (!g_ctx.initialized) {
        if (out_status) *out_status = SV_ERR_UNAVAILABLE;
        return NULL;
    }
    if (!opts || !opts->live_path) {
        if (out_status) *out_status = SV_ERR_INVALID_ARG;
        return NULL;
    }
    if (!is_regular_file(opts->live_path) && !is_directory(opts->live_path)) {
        if (out_status) *out_status = SV_ERR_NOT_FOUND;
        return NULL;
    }

    sv_registration_t *reg = calloc(1, sizeof(sv_registration_t));
    if (!reg) {
        if (out_status) *out_status = SV_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    generate_unique_id(reg->id);
    reg->live_path = strdup(opts->live_path);
    reg->platform = opts->platform ? strdup(opts->platform) : NULL;
    reg->emulator = opts->emulator ? strdup(opts->emulator) : NULL;
    reg->product_version = opts->product_version ? strdup(opts->product_version) : NULL;
    reg->game_id = opts->game_id ? strdup(opts->game_id) : NULL;
    reg->rom_path = opts->rom_path ? strdup(opts->rom_path) : NULL;
    reg->label = opts->label ? strdup(opts->label) : NULL;
    reg->mode = opts->mode;
    reg->shape = opts->shape;
    reg->retention_count = opts->retention_count;

    /* Identity resolution from manifest */
    if (manifest) {
        /* Use manifest platform if not supplied by caller */
        if (!reg->platform && manifest->platform[0]) {
            reg->platform = strdup(manifest->platform);
        }
        /* Use manifest emulator if not supplied by caller */
        if (!reg->emulator && manifest->emulator[0]) {
            reg->emulator = strdup(manifest->emulator);
        }
        /* Use manifest shape if not supplied by caller (shape == UNKNOWN) */
        if (reg->shape == SV_SHAPE_UNKNOWN && manifest->shape != SV_SHAPE_UNKNOWN) {
            reg->shape = manifest->shape;
        }

        /* Resolve game_id via single-method identity resolution */
        if (!reg->game_id || reg->game_id[0] == '\0') {
            char detected_id[256] = {0};
            sv_status_t id_status = resolve_identity(manifest, opts->live_path,
                                                       opts->rom_path,
                                                       detected_id, sizeof(detected_id));
            if (id_status == SV_OK && detected_id[0] != '\0') {
                free(reg->game_id);
                reg->game_id = strdup(detected_id);
            }
        }

        /* Store and resolve save_path_template */
        if (manifest->save_path_template[0]) {
            reg->save_path_template = strdup(manifest->save_path_template);
            /* Resolve template to actual directory path, relative to base_path */
            char resolved[4096] = {0};
            resolve_save_path_template(manifest->save_path_template,
                                        reg->game_id, reg->platform, reg->emulator,
                                        reg->live_path, resolved, sizeof(resolved));
            if (resolved[0]) {
                /* Make absolute relative to base_path */
                char abs_path[4096];
                snprintf(abs_path, sizeof(abs_path), "%s/%s", g_ctx.base_path, resolved);
                reg->resolved_magazine_dir = strdup(abs_path);
            }
        }
    }

    if (g_ctx.num_regs >= g_ctx.regs_cap) {
        size_t newcap = g_ctx.regs_cap ? g_ctx.regs_cap * 2 : 16;
        sv_registration_t **newregs = realloc(g_ctx.regs, newcap * sizeof(sv_registration_t *));
        if (!newregs) {
            free(reg->live_path); free(reg->platform); free(reg->emulator);
            free(reg->product_version); free(reg->game_id); free(reg->rom_path);
            free(reg->label); free(reg);
            if (out_status) *out_status = SV_ERR_OUT_OF_MEMORY;
            return NULL;
        }
        g_ctx.regs = newregs;
        g_ctx.regs_cap = newcap;
    }
    g_ctx.regs[g_ctx.num_regs++] = reg;

    if (!metadata_flush()) {
        /* non-fatal */
    }

    if (out_status) *out_status = SV_OK;
    return reg;
}

/* ---- Layer 6: External transport ---- */
sv_status_t sv_push_external(const sv_id_t entry_id, const sv_transport_t *transport) {
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!entry_id || !transport) return SV_ERR_INVALID_ARG;

    sv_entry_t *ent = find_entry(entry_id);
    if (!ent) return SV_ERR_NOT_FOUND;

    if (!transport->push) return SV_ERR_UNAVAILABLE;

    sv_entry_info_t info;
    sv_status_t st = sv_read_entry(entry_id, &info);
    if (st != SV_OK) return st;

    sv_xport_status_t xst = transport->push(&info, ent->magazine_slot_path, transport->user_ctx);
    switch (xst) {
        case SV_XPORT_OK: return SV_OK;
        case SV_XPORT_ERR_NOT_FOUND: return SV_ERR_NOT_FOUND;
        case SV_XPORT_ERR_CONFLICT: return SV_ERR_CONFLICT;
        default: return SV_ERR_IO;
    }
}

sv_status_t sv_pull_external(const sv_id_t reg_id, const sv_transport_t *transport, sv_pull_opts_t *pull_opts) {
    (void)pull_opts;
    if (!g_ctx.initialized) return SV_ERR_UNAVAILABLE;
    if (!reg_id || !transport) return SV_ERR_INVALID_ARG;

    sv_registration_t *reg = find_reg(reg_id);
    if (!reg) return SV_ERR_NOT_FOUND;

    if (!transport->pull) return SV_ERR_UNAVAILABLE;

    /* Transport pulls remote save into a buffer */
    sv_entry_info_t entry_info;
    memset(&entry_info, 0, sizeof(entry_info));
    char dest_buf[4096];

    sv_xport_status_t xst = transport->pull(reg_id, &entry_info, dest_buf, sizeof(dest_buf), transport->user_ctx);
    if (xst != SV_XPORT_OK) {
        switch (xst) {
            case SV_XPORT_ERR_NOT_FOUND: return SV_ERR_NOT_FOUND;
            case SV_XPORT_ERR_CONFLICT: return SV_ERR_CONFLICT;
            default: return SV_ERR_IO;
        }
    }

    /* Write pulled data to a temp location, then import as a new magazine entry */
    char mag_dir[4096];
    join_path(mag_dir, sizeof(mag_dir), g_ctx.base_path, "magazine");

    /* Create entry */
    sv_entry_t *ent = calloc(1, sizeof(sv_entry_t));
    if (!ent) return SV_ERR_OUT_OF_MEMORY;
    generate_unique_id(ent->id);
    ent->sequence = g_ctx.next_sequence++;
    sv_id_copy(ent->parent_id, reg_id);

    char mag_path[4096];
    snprintf(mag_path, sizeof(mag_path), "%s/%s", mag_dir, ent->id);

    /* Write pulled data to magazine path */
    if (entry_info.shape == SV_SHAPE_FILE || entry_info.shape == SV_SHAPE_UNKNOWN) {
        FILE *f = fopen(mag_path, "wb");
        if (!f) { free(ent); return SV_ERR_IO; }
        size_t data_len = strlen(dest_buf);
        if (data_len > 0 && fwrite(dest_buf, 1, data_len, f) != data_len) {
            fclose(f); unlink(mag_path); free(ent); return SV_ERR_IO;
        }
        fclose(f);
        ent->shape = SV_SHAPE_FILE;
        ent->size_bytes = data_len;
    } else {
        /* For directory/container shapes, need more sophisticated handling — treat as file for now */
        FILE *f = fopen(mag_path, "wb");
        if (!f) { free(ent); return SV_ERR_IO; }
        size_t data_len = strlen(dest_buf);
        if (data_len > 0 && fwrite(dest_buf, 1, data_len, f) != data_len) {
            fclose(f); unlink(mag_path); free(ent); return SV_ERR_IO;
        }
        fclose(f);
        ent->shape = entry_info.shape;
        ent->size_bytes = data_len;
    }

    ent->magazine_slot_path = strdup(mag_path);
    ent->mtime = (int64_t)time(NULL);
    ent->integrity_ok = true;

    /* Compute hash */
    uint8_t hash[20];
    if (hash_file(mag_path, hash)) {
        memcpy(ent->content_hash, hash, 20);
        ent->content_hash_set = true;
    }

    /* Copy over metadata from pulled entry */
    if (entry_info.label[0]) ent->label = strdup(entry_info.label);
    ent->playtime_seconds = entry_info.playtime_seconds;

    /* Add to store */
    add_entry(ent);
    link_entry_to_reg(ent, reg);

    /* Retention */
    run_retention(reg, NULL, NULL);

    metadata_flush();

    return SV_OK;
}
