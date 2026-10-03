#pragma once
/* Generic removable filesystem volume capability. RiscRTE core treats this
 * interface as opaque; the provider owns the media transport and filesystem. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RISC_STORAGE_VOLUME_API_V1 1u
#define RISC_STORAGE_VOLUME_NAME_MAX 128u

typedef uint32_t risc_storage_dir_t;
typedef uint32_t risc_storage_file_t;
#define RISC_STORAGE_DIR_INVALID 0u
#define RISC_STORAGE_FILE_INVALID 0u

typedef struct {
    char name[RISC_STORAGE_VOLUME_NAME_MAX];
    uint64_t size;
    uint8_t is_directory;
} risc_storage_dirent_v1;

typedef struct {
    uint32_t api_version;
    uint32_t struct_size;
    void *context;
    /* Poll/remount media state. True means the provider serviced the request;
     * call ready() separately to distinguish "no media" from provider error. */
    bool (*refresh)(void *context);
    bool (*ready)(void *context);
    bool (*label)(void *context, char *out, size_t capacity);
    bool (*stat)(void *context, const char *path, uint64_t *size_out,
                 bool *is_directory_out);
    risc_storage_dir_t (*dir_open)(void *context, const char *path);
    bool (*dir_next)(void *context, risc_storage_dir_t directory,
                     risc_storage_dirent_v1 *entry);
    void (*dir_close)(void *context, risc_storage_dir_t directory);
    risc_storage_file_t (*file_open_read)(void *context, const char *path,
                                          uint64_t *size_out);
    size_t (*file_read)(void *context, risc_storage_file_t file,
                        void *buffer, size_t capacity);
    /* Creation is exclusive: existing destinations are rejected. */
    risc_storage_file_t (*file_open_write)(void *context, const char *path);
    size_t (*file_write)(void *context, risc_storage_file_t file,
                         const void *buffer, size_t size);
    /* commit=false removes the incomplete destination and releases clusters. */
    bool (*file_close)(void *context, risc_storage_file_t file, bool commit);
    bool (*remove)(void *context, const char *path);
    bool (*last_error)(void *context, char *out, size_t capacity);
} risc_storage_volume_api_v1;

#ifdef __cplusplus
}
#endif
