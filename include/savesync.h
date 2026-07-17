#ifndef SAVESYNC_H
#define SAVESYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ---- */
#define SAVESYNC_MAJOR 0
#define SAVESYNC_MINOR 1
#define SAVESYNC_PATCH 0

/* ---- Core typedef ---- */
typedef char sv_id_t[9];

/* ---- Opaque handle ---- */
typedef struct sv_registration_s sv_registration_t;

/* ---- Enums ---- */
typedef enum {
    SV_MODE_DEFAULT = 0,
    SV_MODE_STRATEGY
} sv_registration_mode_t;

typedef enum {
    SV_IDENTITY_NONE = 0,
    SV_IDENTITY_SERIAL_CNF,
    SV_IDENTITY_SERIAL_SFO,
    SV_IDENTITY_SERIAL_IPBIN,
    SV_IDENTITY_BOOT_HEADER,
    SV_IDENTITY_CHECKSUM,
    SV_IDENTITY_PLUGGABLE,
    SV_IDENTITY_ROM_HEADER,
    SV_IDENTITY_TEXT_PATTERN,
} sv_identity_method_t;

typedef enum {
    SV_SHAPE_UNKNOWN = 0,
    SV_SHAPE_FILE,
    SV_SHAPE_DIRECTORY,
    SV_SHAPE_CONTAINER,
    SV_SHAPE_ARCHIVE
} sv_save_shape_t;

typedef enum {
    SV_OK = 0,
    SV_ERR_GENERIC,
    SV_ERR_INVALID_ARG,
    SV_ERR_NOT_FOUND,
    SV_ERR_IO,
    SV_ERR_OUT_OF_MEMORY,
    SV_ERR_CONFLICT,
    SV_ERR_UNAVAILABLE
} sv_status_t;

typedef enum {
    SV_CONFLICT_NONE = 0,
    SV_CONFLICT_EXISTING_DATA
} sv_conflict_t;

typedef enum {
    SV_ON_CONFLICT_REPORT = 0,
    SV_ON_CONFLICT_OVERRIDE,
    SV_ON_CONFLICT_ABORT_SILENT
} sv_on_conflict_t;

typedef enum {
    SV_RELOCATE_COPY = 0,
    SV_RELOCATE_MOVE
} sv_relocate_mode_t;

typedef enum {
    SV_PULL_CONFLICT_REPORT = 0,
    SV_PULL_CONFLICT_OVERRIDE
} sv_pull_on_conflict_t;

typedef enum {
    SV_EVICT_OLDEST = 0,
    SV_EVICT_NONE
} sv_eviction_policy_t;

typedef enum {
    SV_XPORT_OK = 0,
    SV_XPORT_ERR_IO,
    SV_XPORT_ERR_AUTH,
    SV_XPORT_ERR_NOT_FOUND,
    SV_XPORT_ERR_CONFLICT,
    SV_XPORT_ERR_OTHER
} sv_xport_status_t;

/* ---- Option structs ---- */
typedef struct {
    uint32_t set_mask;
    const char          *live_path;
    const char          *platform;
    const char          *emulator;
    const char          *product_version;
    const char          *game_id;
    const char          *rom_path;
    const char          *label;
    sv_registration_mode_t mode;
    sv_save_shape_t        shape;
    uint32_t               retention_count;
    sv_on_conflict_t       on_conflict;
    sv_relocate_mode_t     relocate_mode;
} sv_update_opts_t;

typedef struct {
    sv_conflict_t conflict;
} sv_update_result_t;

typedef struct {
    const char          *live_path;
    const char          *platform;
    const char          *emulator;
    const char          *product_version;
    const char          *game_id;
    const char          *rom_path;
    const char          *label;
    sv_registration_mode_t mode;
    sv_save_shape_t        shape;
    uint32_t               retention_count;
} sv_register_opts_t;

typedef struct {
    bool force;
} sv_save_opts_t;

typedef struct {
    sv_id_t entry_id;
    bool    entry_created;
    bool    dedup_skipped;
    sv_id_t evicted_ids[8];
    size_t  evicted_count;
} sv_save_result_t;

typedef struct {
    sv_pull_on_conflict_t  on_conflict;
} sv_pull_opts_t;

typedef struct {
    bool    conflicted;
    bool    did_pull;
    bool    did_backup;
    sv_id_t backup_entry_id;
} sv_pull_result_t;

/* ---- Read-only info structs ---- */
typedef struct {
    char  id[9];
    char  live_path[4096];
    char  platform[256];
    char  emulator[256];
    char  product_version[256];
    char  game_id[256];
    char  rom_path[4096];
    char  label[256];
    uint8_t     rom_hash[20];
    bool        rom_hash_set;
    sv_registration_mode_t mode;
    sv_save_shape_t        shape;
    uint32_t               retention_count;
    size_t                 entry_count;
} sv_registration_info_t;

typedef struct {
    char  id[9];
    char  parent_id[9];
    char  label[256];
    char  magazine_slot_path[4096];
    uint8_t     content_hash[20];
    bool        content_hash_set;
    bool        integrity_ok;
    int64_t     mtime;
    uint64_t    size_bytes;
    uint32_t    playtime_seconds;
    sv_save_shape_t shape;
} sv_entry_info_t;

/* ---- Transport ---- */
typedef sv_xport_status_t (*sv_xport_push_fn)(
    const sv_entry_info_t *entry,
    const char *magazine_slot_path,
    void *user_ctx
);

typedef sv_xport_status_t (*sv_xport_pull_fn)(
    const sv_id_t reg_id,
    sv_entry_info_t *out_entry,
    char *out_dest_buf, size_t out_dest_buf_len,
    void *user_ctx
);

typedef struct {
    sv_xport_push_fn push;
    sv_xport_pull_fn pull;
    void *user_ctx;
} sv_transport_t;

/* ---- Manifest (STRATEGY mode) ---- */
typedef struct sv_manifest_s sv_manifest_t;

/* Hash-DB callback for Tier 3 identity resolution */
typedef sv_status_t (*sv_hash_db_lookup_fn)(
    const uint8_t *hash, size_t hash_len,
    const char *platform,
    char *out_game_id, size_t out_game_id_len,
    void *user_ctx
);

/* ---- Lifetime ---- */
sv_status_t sv_init(const char *base_path);
void sv_shutdown(void);

/* ---- Accessors ---- */
void sv_registration_id(const sv_registration_t *reg, sv_id_t out_id);

/* ---- Layer 2: Registration ---- */
sv_registration_t *sv_register(const sv_register_opts_t *opts, sv_status_t *out_status);
sv_status_t         sv_update_register(sv_registration_t *reg, const sv_update_opts_t *opts, sv_update_result_t *out_result);
void                sv_unregister(sv_registration_t *reg);
sv_status_t         sv_list_registrations(sv_id_t *out_ids, size_t max_ids, size_t *out_count);
sv_status_t         sv_read_registration(const sv_id_t id, sv_registration_info_t *out_info);

/* ---- Layer 4: Local save/pull ---- */
sv_status_t sv_save(sv_registration_t *reg, const sv_save_opts_t *opts, sv_save_result_t *out_result);
sv_status_t sv_pull(sv_registration_t *reg, const sv_pull_opts_t *opts, sv_pull_result_t *out_result);
sv_status_t sv_pull_select(sv_registration_t *reg, const sv_id_t entry_id, const sv_pull_opts_t *opts, sv_pull_result_t *out_result);
sv_status_t sv_list_entries(const sv_id_t reg_id, sv_id_t *out_ids, size_t max_ids, size_t *out_count);
sv_status_t sv_read_entry(const sv_id_t entry_id, sv_entry_info_t *out_info);
sv_status_t sv_reparent_entry(const sv_id_t entry_id, const sv_id_t new_parent_reg_id);
sv_status_t sv_delete_entry(const sv_id_t entry_id);

/* ---- Layer 6: External transport ---- */
sv_status_t sv_push_external(const sv_id_t entry_id, const sv_transport_t *transport);
sv_status_t sv_pull_external(const sv_id_t reg_id, const sv_transport_t *transport, sv_pull_opts_t *pull_opts);

/* ---- Manifest (STRATEGY mode) ---- */
sv_manifest_t *sv_manifest_create(void);
void           sv_manifest_free(sv_manifest_t *manifest);
sv_status_t    sv_manifest_load(const char *path, sv_manifest_t *out_manifest);
sv_status_t    sv_manifest_save(const char *path, const sv_manifest_t *manifest);

/* Manifest field accessors */
const char          *sv_manifest_get_platform(const sv_manifest_t *manifest);
const char          *sv_manifest_get_emulator(const sv_manifest_t *manifest);
sv_save_shape_t      sv_manifest_get_shape(const sv_manifest_t *manifest);
sv_identity_method_t sv_manifest_get_identity_method(const sv_manifest_t *manifest);
const char          *sv_manifest_get_save_path_template(const sv_manifest_t *manifest);

/* Manifest field setters */
void sv_manifest_set_platform(sv_manifest_t *manifest, const char *platform);
void sv_manifest_set_emulator(sv_manifest_t *manifest, const char *emulator);
void sv_manifest_set_shape(sv_manifest_t *manifest, sv_save_shape_t shape);
void sv_manifest_set_identity_method(sv_manifest_t *manifest, sv_identity_method_t method);
void sv_manifest_set_serial_params(sv_manifest_t *manifest, const char *file,
                                    size_t offset, size_t length, const char *pattern);
void sv_manifest_set_checksum_params(sv_manifest_t *manifest, size_t offset,
                                      size_t size, bool big_endian);
void sv_manifest_set_save_path_template(sv_manifest_t *manifest, const char *tmpl);
void sv_manifest_set_hash_db_callback(sv_manifest_t *manifest,
                                       sv_hash_db_lookup_fn fn, void *ctx);
void sv_manifest_set_rom_header_params(sv_manifest_t *manifest, size_t offset,
                                        size_t length);
void sv_manifest_set_text_pattern_params(sv_manifest_t *manifest,
                                          const char *pattern,
                                          const char *source,
                                          size_t offset,
                                          size_t length);
void sv_manifest_set_normalize_params(sv_manifest_t *manifest,
                                       const char *strip,
                                       const char *replace);

/* Register with manifest (STRATEGY mode) */
sv_registration_t *sv_register_with_manifest(
    const sv_register_opts_t *opts,
    const sv_manifest_t *manifest,
    sv_status_t *out_status
);

#ifdef __cplusplus
}
#endif

#endif /* SAVESYNC_H */
