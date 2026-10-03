/* FAT16/FAT32 filesystem adapted from Reader usb_mass_storage at 3d9bc4f.
 * SD SPI transport is implemented locally, not a firmware filesystem proxy. */
#include "RiscStorageVolumeV1.h"
#include "sd_spi.h"
#define MSC_SECTOR_SIZE 512u
#define FAT_ATTR_READ_ONLY 0x01u
#define FAT_ATTR_VOLUME_ID 0x08u
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_LFN 0x0fu
#define FAT_ENTRY_DELETED 0xe5u
#define FAT_ENTRY_END 0x00u
#define FAT_MAX_LFN_UNITS 260u
#define FAT_MAX_DIR_SCAN 8192u

static bool volume_ready,storage_running;
static char error_text[96];
static uint8_t sector_buffer[MSC_SECTOR_SIZE];
static uint16_t lfn_units[FAT_MAX_LFN_UNITS];
/* Mounted FAT geometry. */
static uint32_t partition_lba;
static uint32_t total_sectors;
static uint32_t fat_start_lba;
static uint32_t fat_sectors;
static uint32_t root_start_lba;
static uint32_t root_dir_sectors;
static uint32_t data_start_lba;
static uint32_t cluster_count;
static uint32_t root_cluster;
static uint32_t allocation_hint;
static uint8_t sectors_per_cluster;
static uint8_t fat_count;
static uint8_t fat_kind; /* 16 or 32 */

static uint32_t handle_sequence;

typedef struct {
    bool root_fixed;
    uint32_t cluster;
} dir_ref;

typedef struct {
    bool active;
    risc_storage_dir_t token;
    dir_ref directory;
    uint32_t entry_index;
} dir_state_t;
static dir_state_t dir_state;

typedef struct {
    char name[RISC_STORAGE_VOLUME_NAME_MAX];
    bool is_directory;
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t short_index;
    uint8_t lfn_count;
} entry_info;

typedef struct {
    bool active;
    bool writable;
    risc_storage_file_t token;
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t cluster_base_position;
    uint32_t size;
    uint32_t position;
    dir_ref parent;
    uint32_t short_index;
    uint8_t lfn_count;
} file_state_t;
static file_state_t file_state;

static void memory_zero(void *destination, size_t size) {
    uint8_t *p = (uint8_t *)destination;
    while (size--) *p++ = 0;
}
static void memory_copy(void *destination, const void *source, size_t size) {
    uint8_t *d = (uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    while (size--) *d++ = *s++;
}
static size_t text_length(const char *text) {
    size_t n = 0;
    if (text) while (text[n]) ++n;
    return n;
}
static bool text_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}
static bool text_equal_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) { ++a; ++b; }
    return *a == *b;
}
static void set_error(const char *message) {
    size_t i = 0;
    if (!message) message = "Storage error";
    while (message[i] && i + 1u < sizeof(error_text)) {
        error_text[i] = message[i]; ++i;
    }
    error_text[i] = 0;
}
static void clear_error(void) { error_text[0] = 0; }
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static inline void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}
static uint32_t next_handle(void) {
    return handle_sequence==UINT32_MAX ? 0 : ++handle_sequence;
}
static bool valid_cluster(uint32_t cluster) {
    return cluster >= 2u && cluster < cluster_count + 2u;
}
static bool end_of_chain(uint32_t value) {
    return fat_kind == 16u ? value >= 0xfff8u : value >= 0x0ffffff8u;
}
static uint32_t eoc_value(void) { return fat_kind == 16u ? 0xffffu : 0x0fffffffu; }

static uint32_t io_count;
static uint64_t io_started;
static bool io_budget(void) {
    if (++io_count>8192 || timer->monotonic_ms(timer->context)-io_started>2000) { set_error("Storage operation budget exhausted"); return false; }
    if ((io_count&7)==0) timer->sleep_ms(timer->context,1);
    return true;
}
static bool read_sector(uint32_t lba,uint8_t *out) { return io_budget() && sd_read(lba,out); }
static bool write_sector(uint32_t lba,uint8_t *bytes) { return io_budget() && sd_write(lba,bytes); }
static bool parse_bpb(uint32_t lba, uint32_t capacity_sectors) {
    if (!read_sector(lba, sector_buffer) || sector_buffer[510] != 0x55u ||
        sector_buffer[511] != 0xaau || read_le16(sector_buffer + 11) != 512u)
        return false;
    const uint8_t spc = sector_buffer[13];
    const uint16_t reserved = read_le16(sector_buffer + 14);
    const uint8_t fats = sector_buffer[16];
    const uint16_t root_entries = read_le16(sector_buffer + 17);
    uint32_t sectors = read_le16(sector_buffer + 19);
    if (!sectors) sectors = read_le32(sector_buffer + 32);
    uint32_t fat_size = read_le16(sector_buffer + 22);
    if (!fat_size) fat_size = read_le32(sector_buffer + 36);
    if (!spc || (spc & (spc - 1u)) || spc > 128u || !reserved || !fats ||
        fats > 2u || !sectors || !fat_size || lba >= capacity_sectors ||
        sectors > capacity_sectors - lba) return false;
    const uint32_t root_sectors = ((uint32_t)root_entries * 32u + 511u) / 512u;
    const uint64_t first_data64 = (uint64_t)reserved + (uint64_t)fats * fat_size + root_sectors;
    if (first_data64 >= sectors) return false;
    const uint32_t first_data = (uint32_t)first_data64;
    const uint32_t clusters = (sectors - first_data) / spc;
    if (clusters < 4085u || clusters >= 0x0ffffff5u) return false;
    uint8_t kind = clusters < 65525u ? 16u : 32u;
    uint32_t root = 0u;
    if (kind == 32u) {
        root = read_le32(sector_buffer + 44) & 0x0fffffffu;
        if (root_entries != 0u || root < 2u || root >= clusters + 2u) return false;
    } else if (root_entries == 0u) return false;

    if ((uint64_t)fat_size*512 < (uint64_t)(clusters+2)*(kind==16?2:4)) return false;
    partition_lba = lba;
    total_sectors = sectors;
    fat_start_lba = lba + reserved;
    fat_sectors = fat_size;
    root_start_lba = lba + reserved + (uint32_t)fats * fat_size;
    root_dir_sectors = root_sectors;
    data_start_lba = lba + first_data;
    cluster_count = clusters;
    root_cluster = root;
    sectors_per_cluster = spc;
    fat_count = fats;
    fat_kind = kind;
    allocation_hint = 2u;
    return true;
}

static bool mount_fat(uint32_t last_lba) {
    const uint32_t capacity = last_lba + 1u;
    partition_lba = 0u;
    total_sectors = capacity;
    cluster_count = 0u;
    fat_kind = 0u;
    if (parse_bpb(0u, capacity)) return true;
    if (!read_sector(0u, sector_buffer) || sector_buffer[510] != 0x55u ||
        sector_buffer[511] != 0xaau) return false;
    uint8_t partitions[64]; memory_copy(partitions,sector_buffer+446,64);
    for (unsigned p = 0; p < 4u; ++p) {
        const uint8_t *entry = partitions + p * 16u;
        const uint32_t start = read_le32(entry + 8);
        const uint32_t count = read_le32(entry + 12);
        if (entry[4] && start && count && start < capacity && count <= capacity - start &&
            parse_bpb(start, capacity)) return true;
    }
    return false;
}

static bool fat_get(uint32_t cluster, uint32_t *value) {
    if (!value || !valid_cluster(cluster)) return false;
    const uint32_t offset = fat_kind == 16u ? cluster * 2u : cluster * 4u;
    const uint32_t lba = fat_start_lba + offset / 512u;
    const uint32_t within = offset % 512u;
    if (!read_sector(lba, sector_buffer)) return false;
    *value = fat_kind == 16u ? read_le16(sector_buffer + within) :
                              (read_le32(sector_buffer + within) & 0x0fffffffu);
    return true;
}
static bool fat_set(uint32_t cluster, uint32_t value) {
    if (!valid_cluster(cluster)) return false;
    const uint32_t offset = fat_kind == 16u ? cluster * 2u : cluster * 4u;
    for (uint8_t copy = 0; copy < fat_count; ++copy) {
        const uint32_t lba = fat_start_lba + (uint32_t)copy * fat_sectors + offset / 512u;
        const uint32_t within = offset % 512u;
        if (!read_sector(lba, sector_buffer)) return false;
        if (fat_kind == 16u) write_le16(sector_buffer + within, (uint16_t)value);
        else {
            uint32_t current = read_le32(sector_buffer + within);
            write_le32(sector_buffer + within, (current & 0xf0000000u) | (value & 0x0fffffffu));
        }
        if (!write_sector(lba, sector_buffer)) return false;
    }
    return true;
}
static bool zero_cluster(uint32_t cluster) {
    memory_zero(sector_buffer, sizeof(sector_buffer));
    const uint32_t first = data_start_lba + (cluster - 2u) * sectors_per_cluster;
    for (uint8_t i = 0; i < sectors_per_cluster; ++i)
        if (!write_sector(first + i, sector_buffer)) return false;
    return true;
}
static bool allocate_cluster(uint32_t *out) {
    if (!out || !cluster_count) return false;
    uint32_t start = allocation_hint;
    if (!valid_cluster(start)) start = 2u;
    uint32_t cluster = start;
    do {
        uint32_t value = 0;
        if (!fat_get(cluster, &value)) return false;
        if (value == 0u) {
            if (!fat_set(cluster, eoc_value()) || !zero_cluster(cluster)) {
                (void)fat_set(cluster, 0u);
                return false;
            }
            allocation_hint = cluster + 1u;
            if (!valid_cluster(allocation_hint)) allocation_hint = 2u;
            *out = cluster;
            return true;
        }
        ++cluster;
        if (!valid_cluster(cluster)) cluster = 2u;
    } while (cluster != start);
    set_error("USB storage is full");
    return false;
}
static bool free_chain(uint32_t first) {
    uint32_t cluster = first;
    uint32_t guard = 0;
    while (valid_cluster(cluster) && guard++ <= cluster_count) {
        uint32_t next = 0;
        if (!fat_get(cluster, &next) || !fat_set(cluster, 0u)) return false;
        if (end_of_chain(next) || !valid_cluster(next)) return true;
        cluster = next;
    }
    return first == 0u;
}

static dir_ref root_directory(void) {
    dir_ref result;
    result.root_fixed = fat_kind == 16u;
    result.cluster = fat_kind == 32u ? root_cluster : 0u;
    return result;
}
static bool directory_sector(dir_ref directory, uint32_t sector_index,
                             bool extend, uint32_t *lba) {
    if (!lba) return false;
    if (directory.root_fixed) {
        if (sector_index >= root_dir_sectors) return false;
        *lba = root_start_lba + sector_index;
        return true;
    }
    if (!valid_cluster(directory.cluster)) return false;
    uint32_t cluster = directory.cluster;
    uint32_t skip = sector_index / sectors_per_cluster;
    for (uint32_t i = 0; i < skip; ++i) {
        uint32_t next = 0;
        if (!fat_get(cluster, &next)) return false;
        if (end_of_chain(next)) {
            if (!extend) return false;
            uint32_t added = 0;
            if (!allocate_cluster(&added) || !fat_set(cluster, added)) {
                if (added) (void)fat_set(added, 0u);
                return false;
            }
            next = added;
        }
        if (!valid_cluster(next)) return false;
        cluster = next;
    }
    *lba = data_start_lba + (cluster - 2u) * sectors_per_cluster +
           sector_index % sectors_per_cluster;
    return true;
}
static bool directory_entry_location(dir_ref directory, uint32_t index,
                                     bool extend, uint32_t *lba, uint16_t *offset) {
    const uint32_t byte_offset = index * 32u;
    if (!directory_sector(directory, byte_offset / 512u, extend, lba)) return false;
    if (offset) *offset = (uint16_t)(byte_offset % 512u);
    return true;
}
static bool read_directory_entry(dir_ref directory, uint32_t index, uint8_t out[32]) {
    uint32_t lba = 0; uint16_t offset = 0;
    if (!out || !directory_entry_location(directory, index, false, &lba, &offset) ||
        !read_sector(lba, sector_buffer)) return false;
    memory_copy(out, sector_buffer + offset, 32u);
    return true;
}
static bool write_directory_entry(dir_ref directory, uint32_t index, const uint8_t in[32]) {
    uint32_t lba = 0; uint16_t offset = 0;
    if (!in || !directory_entry_location(directory, index, true, &lba, &offset) ||
        !read_sector(lba, sector_buffer)) return false;
    memory_copy(sector_buffer + offset, in, 32u);
    return write_sector(lba, sector_buffer);
}

static uint8_t short_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11u; ++i)
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + short_name[i]);
    return sum;
}
static void clear_lfn(void) {
    for (unsigned i = 0; i < FAT_MAX_LFN_UNITS; ++i) lfn_units[i] = 0xffffu;
}
static void capture_lfn_piece(const uint8_t raw[32], uint8_t ordinal) {
    static const uint8_t offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    const size_t base = (size_t)(ordinal - 1u) * 13u;
    for (unsigned i = 0; i < 13u && base + i < FAT_MAX_LFN_UNITS; ++i)
        lfn_units[base + i] = read_le16(raw + offsets[i]);
}
static size_t append_utf8(char *out, size_t capacity, size_t used, uint32_t code) {
    if (code < 0x80u) {
        if (used + 1u < capacity) out[used++] = (char)code;
    } else if (code < 0x800u) {
        if (used + 2u < capacity) {
            out[used++] = (char)(0xc0u | (code >> 6));
            out[used++] = (char)(0x80u | (code & 0x3fu));
        }
    } else if (code < 0x10000u) {
        if (used + 3u < capacity) {
            out[used++] = (char)(0xe0u | (code >> 12));
            out[used++] = (char)(0x80u | ((code >> 6) & 0x3fu));
            out[used++] = (char)(0x80u | (code & 0x3fu));
        }
    } else if (used + 4u < capacity) {
        out[used++] = (char)(0xf0u | (code >> 18));
        out[used++] = (char)(0x80u | ((code >> 12) & 0x3fu));
        out[used++] = (char)(0x80u | ((code >> 6) & 0x3fu));
        out[used++] = (char)(0x80u | (code & 0x3fu));
    }
    return used;
}
static bool lfn_to_utf8(char *out, size_t capacity) {
    if (!out || capacity < 2u) return false;
    size_t used = 0;
    for (size_t i = 0; i < FAT_MAX_LFN_UNITS; ++i) {
        uint32_t code = lfn_units[i];
        if (code == 0u || code == 0xffffu) break;
        if (code >= 0xd800u && code <= 0xdbffu && i + 1u < FAT_MAX_LFN_UNITS) {
            uint16_t low = lfn_units[i + 1u];
            if (low >= 0xdc00u && low <= 0xdfffu) {
                code = 0x10000u + (((code - 0xd800u) << 10) | (low - 0xdc00u));
                ++i;
            } else code = '?';
        } else if (code >= 0xdc00u && code <= 0xdfffu) code = '?';
        const size_t prior = used;
        used = append_utf8(out, capacity, used, code);
        if (used == prior && code != 0u) return false;
    }
    out[used] = 0;
    return used != 0u;
}
static void short_to_name(const uint8_t raw[32], char *out, size_t capacity) {
    size_t used = 0;
    for (unsigned i = 0; i < 8u && raw[i] != ' '; ++i) {
        uint8_t c = raw[i];
        if (i == 0u && c == 0x05u) c = 0xe5u;
        if (used + 1u < capacity) out[used++] = (char)c;
    }
    bool has_ext = false;
    for (unsigned i = 8u; i < 11u; ++i) if (raw[i] != ' ') has_ext = true;
    if (has_ext && used + 1u < capacity) out[used++] = '.';
    for (unsigned i = 8u; i < 11u && raw[i] != ' '; ++i)
        if (used + 1u < capacity) out[used++] = (char)raw[i];
    out[used] = 0;
}
static uint32_t entry_cluster(const uint8_t raw[32]) {
    const uint32_t low = read_le16(raw + 26);
    const uint32_t high = fat_kind == 32u ? read_le16(raw + 20) : 0u;
    return (high << 16) | low;
}
static void set_entry_cluster(uint8_t raw[32], uint32_t cluster) {
    write_le16(raw + 26, (uint16_t)cluster);
    if (fat_kind == 32u) write_le16(raw + 20, (uint16_t)(cluster >> 16));
}

static bool scan_next(dir_ref directory, uint32_t *index, entry_info *out) {
    uint8_t raw[32];
    bool lfn_active = false;
    uint8_t lfn_checksum = 0;
    uint8_t expected = 0;
    uint8_t lfn_count = 0;
    if (!index || !out) return false;
    clear_lfn();
    for (; *index < FAT_MAX_DIR_SCAN; ++*index) {
        if (!read_directory_entry(directory, *index, raw)) return false;
        if (raw[0] == FAT_ENTRY_END) return false;
        if (raw[0] == FAT_ENTRY_DELETED) { lfn_active = false; continue; }
        if (raw[11] == FAT_ATTR_LFN) {
            const uint8_t ordinal = raw[0] & 0x1fu;
            if (raw[0] & 0x40u) {
                clear_lfn(); lfn_active = ordinal > 0u && ordinal <= 20u;
                expected = ordinal; lfn_count = ordinal; lfn_checksum = raw[13];
            }
            if (!lfn_active || ordinal != expected || raw[13] != lfn_checksum) {
                lfn_active = false; continue;
            }
            capture_lfn_piece(raw, ordinal);
            --expected;
            continue;
        }
        if (raw[11] & FAT_ATTR_VOLUME_ID) { lfn_active = false; continue; }
        memory_zero(out, sizeof(*out));
        const bool valid_lfn = lfn_active && expected == 0u &&
                               lfn_checksum == short_checksum(raw) &&
                               lfn_to_utf8(out->name, sizeof(out->name));
        if (!valid_lfn) short_to_name(raw, out->name, sizeof(out->name));
        out->attributes = raw[11];
        out->is_directory = (raw[11] & FAT_ATTR_DIRECTORY) != 0u;
        out->first_cluster = entry_cluster(raw);
        out->size = read_le32(raw + 28);
        out->short_index = *index;
        out->lfn_count = valid_lfn ? lfn_count : 0u;
        ++*index;
        if (text_equal(out->name, ".") || text_equal(out->name, "..")) {
            lfn_active = false; clear_lfn(); continue;
        }
        return out->name[0] != 0;
    }
    return false;
}
static bool lookup_in_directory(dir_ref directory, const char *name, entry_info *out) {
    uint32_t index = 0;
    entry_info candidate;
    while (scan_next(directory, &index, &candidate)) {
        if (text_equal_ci(candidate.name, name)) {
            if (out) *out = candidate;
            return true;
        }
    }
    return false;
}

static bool next_component(const char **cursor, char *out, size_t capacity, bool *last) {
    const char *p = *cursor;
    while (*p == '/') ++p;
    if (!*p) return false;
    size_t used = 0;
    while (*p && *p != '/') {
        if (used + 1u >= capacity) return false;
        out[used++] = *p++;
    }
    out[used] = 0;
    const char *q = p;
    while (*q == '/') ++q;
    *last = *q == 0;
    *cursor = p;
    return used != 0u;
}
static bool bounded_path(const char *p) {
    if (!p) return false;
    for (size_t i=0;i<512;i++) if (!p[i]) return true;
    return false;
}
static bool resolve_path(const char *path, entry_info *out) {
    if (!bounded_path(path)) return false;
    if (!path || path[0] != '/') return false;
    const char *cursor = path;
    dir_ref directory = root_directory();
    char component[RISC_STORAGE_VOLUME_NAME_MAX];
    bool last = false;
    if (text_equal(path, "/")) {
        if (out) {
            memory_zero(out, sizeof(*out));
            out->is_directory = true;
            out->first_cluster = directory.cluster;
            out->name[0] = '/'; out->name[1] = 0;
        }
        return true;
    }
    while (next_component(&cursor, component, sizeof(component), &last)) {
        entry_info found;
        if (!lookup_in_directory(directory, component, &found)) return false;
        if (last) { if (out) *out = found; return true; }
        if (!found.is_directory || !valid_cluster(found.first_cluster)) return false;
        directory.root_fixed = false;
        directory.cluster = found.first_cluster;
        while (*cursor == '/') ++cursor;
    }
    return false;
}
static bool resolve_directory(const char *path, dir_ref *out) {
    if (!path || !out) return false;
    if (text_equal(path, "/")) { *out = root_directory(); return true; }
    entry_info found;
    if (!resolve_path(path, &found) || !found.is_directory || !valid_cluster(found.first_cluster))
        return false;
    out->root_fixed = false; out->cluster = found.first_cluster;
    return true;
}
static bool resolve_parent(const char *path, dir_ref *parent,
                           char *name, size_t capacity) {
    if (!path || path[0] != '/' || !parent || !name || capacity < 2u) return false;
    const char *last = path;
    for (const char *p = path; *p; ++p) if (*p == '/') last = p;
    if (!last[1]) return false;
    size_t length = text_length(last + 1);
    if (!length || length >= capacity) return false;
    memory_copy(name, last + 1, length + 1u);
    if (last == path) return resolve_directory("/", parent);
    char parent_path[512];
    const size_t parent_length = (size_t)(last - path);
    if (parent_length >= sizeof(parent_path)) return false;
    memory_copy(parent_path, path, parent_length);
    parent_path[parent_length] = 0;
    return resolve_directory(parent_path, parent);
}

static bool utf8_to_utf16(const char *name, uint16_t *units, size_t capacity,
                          size_t *count) {
    if (!name || !units || !count) return false;
    size_t used = 0;
    const uint8_t *p = (const uint8_t *)name;
    while (*p) {
        uint32_t code = 0; size_t bytes = 0;
        if (*p < 0x80u) { code = *p; bytes = 1; }
        else if ((*p & 0xe0u) == 0xc0u && (p[1] & 0xc0u) == 0x80u) {
            code = ((uint32_t)(p[0] & 0x1fu) << 6) | (p[1] & 0x3fu); bytes = 2;
            if (code < 0x80u) return false;
        } else if ((*p & 0xf0u) == 0xe0u && (p[1] & 0xc0u) == 0x80u &&
                   (p[2] & 0xc0u) == 0x80u) {
            code = ((uint32_t)(p[0] & 0x0fu) << 12) | ((uint32_t)(p[1] & 0x3fu) << 6) |
                   (p[2] & 0x3fu); bytes = 3;
            if (code < 0x800u || (code >= 0xd800u && code <= 0xdfffu)) return false;
        } else if ((*p & 0xf8u) == 0xf0u && (p[1] & 0xc0u) == 0x80u &&
                   (p[2] & 0xc0u) == 0x80u && (p[3] & 0xc0u) == 0x80u) {
            code = ((uint32_t)(p[0] & 7u) << 18) | ((uint32_t)(p[1] & 0x3fu) << 12) |
                   ((uint32_t)(p[2] & 0x3fu) << 6) | (p[3] & 0x3fu); bytes = 4;
            if (code < 0x10000u || code > 0x10ffffu) return false;
        } else return false;
        if (code < 0x20u || code == '"' || code == '*' || code == '/' || code == ':' ||
            code == '<' || code == '>' || code == '?' || code == '\\' || code == '|') return false;
        if (code <= 0xffffu) {
            if (used >= capacity) return false;
            units[used++] = (uint16_t)code;
        } else {
            if (used + 2u > capacity) return false;
            code -= 0x10000u;
            units[used++] = (uint16_t)(0xd800u | (code >> 10));
            units[used++] = (uint16_t)(0xdc00u | (code & 0x3ffu));
        }
        p += bytes;
    }
    if (!used || name[text_length(name) - 1u] == ' ' || name[text_length(name) - 1u] == '.') return false;
    *count = used;
    return true;
}
static uint32_t name_hash(const char *name) {
    uint32_t hash = 2166136261u;
    while (name && *name) { hash ^= (uint8_t)*name++; hash *= 16777619u; }
    return hash;
}
static char hex_digit(uint8_t value) { return value < 10u ? (char)('0' + value) : (char)('A' + value - 10u); }
static bool short_name_exists(dir_ref directory, const uint8_t name[11]) {
    uint8_t raw[32];
    for (uint32_t index = 0; index < FAT_MAX_DIR_SCAN; ++index) {
        if (!read_directory_entry(directory, index, raw)) return false;
        if (raw[0] == FAT_ENTRY_END) return false;
        if (raw[0] == FAT_ENTRY_DELETED || raw[11] == FAT_ATTR_LFN) continue;
        bool same = true;
        for (unsigned i = 0; i < 11u; ++i) if (raw[i] != name[i]) { same = false; break; }
        if (same) return true;
    }
    return false;
}
static void make_short_name(dir_ref directory, const char *name, uint8_t out[11]) {
    uint32_t hash = name_hash(name);
    const char *dot = 0;
    for (const char *p = name; *p; ++p) if (*p == '.') dot = p;
    for (unsigned attempt = 0; attempt < 36u; ++attempt) {
        for (unsigned i = 0; i < 11u; ++i) out[i] = ' ';
        uint32_t value = hash + attempt * 0x9e3779b9u;
        for (unsigned i = 0; i < 6u; ++i) out[i] = hex_digit((uint8_t)(value >> ((5u - i) * 4u)) & 0x0fu);
        out[6] = '~';
        out[7] = attempt < 10u ? (char)('0' + attempt) : (char)('A' + attempt - 10u);
        if (dot && dot[1]) {
            for (unsigned i = 0; i < 3u && dot[1 + i]; ++i) {
                char c = dot[1 + i];
                out[8 + i] = c >= 'a' && c <= 'z' ? (uint8_t)(c - ('a' - 'A')) : (uint8_t)c;
            }
        }
        if (!short_name_exists(directory, out)) return;
    }
}
static bool find_free_directory_slots(dir_ref directory, uint8_t needed, uint32_t *start) {
    uint8_t raw[32];
    uint32_t run_start = 0, run = 0;
    if (!needed || !start) return false;
    for (uint32_t index = 0; index < FAT_MAX_DIR_SCAN; ++index) {
        uint32_t lba = 0; uint16_t offset = 0;
        if (!directory_entry_location(directory, index, true, &lba, &offset) ||
            !read_sector(lba, sector_buffer)) return false;
        memory_copy(raw, sector_buffer + offset, 32u);
        const bool free = raw[0] == FAT_ENTRY_END || raw[0] == FAT_ENTRY_DELETED;
        if (free) {
            if (!run) run_start = index;
            if (++run >= needed) { *start = run_start; return true; }
        } else run = 0;
    }
    return false;
}
static void fill_lfn_entry(uint8_t raw[32], uint8_t ordinal, uint8_t count,
                           uint8_t checksum, const uint16_t *units, size_t unit_count) {
    static const uint8_t offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    memory_zero(raw, 32u);
    raw[0] = ordinal | (ordinal == count ? 0x40u : 0u);
    raw[11] = FAT_ATTR_LFN; raw[12] = 0u; raw[13] = checksum;
    write_le16(raw + 26, 0u);
    const size_t base = (size_t)(ordinal - 1u) * 13u;
    for (unsigned i = 0; i < 13u; ++i) {
        uint16_t value = 0xffffu;
        const size_t index = base + i;
        if (index < unit_count) value = units[index];
        else if (index == unit_count) value = 0u;
        write_le16(raw + offsets[i], value);
    }
}
static bool create_file_entry(dir_ref parent, const char *name,
                              uint32_t *short_index, uint8_t *lfn_count) {
    uint16_t units[FAT_MAX_LFN_UNITS];
    size_t unit_count = 0;
    uint8_t short_name[11];
    if (!utf8_to_utf16(name, units, FAT_MAX_LFN_UNITS, &unit_count)) {
        set_error("Unsupported FAT filename"); return false;
    }
    uint8_t count = (uint8_t)((unit_count + 12u) / 13u);
    if (!count || count > 20u) { set_error("Filename is too long"); return false; }
    make_short_name(parent, name, short_name);
    if (short_name_exists(parent, short_name)) { set_error("Could not allocate FAT alias"); return false; }
    uint32_t start = 0;
    if (!find_free_directory_slots(parent, (uint8_t)(count + 1u), &start)) {
        set_error("Directory has no free entries"); return false;
    }
    const uint8_t checksum = short_checksum(short_name);
    uint8_t raw[32];
    for (uint8_t disk_pos = 0; disk_pos < count; ++disk_pos) {
        const uint8_t ordinal = (uint8_t)(count - disk_pos);
        fill_lfn_entry(raw, ordinal, count, checksum, units, unit_count);
        if (!write_directory_entry(parent, start + disk_pos, raw)) return false;
    }
    memory_zero(raw, sizeof(raw));
    memory_copy(raw, short_name, 11u);
    raw[11] = 0x20u;
    set_entry_cluster(raw, 0u);
    write_le32(raw + 28, 0u);
    if (!write_directory_entry(parent, start + count, raw)) return false;
    *short_index = start + count;
    *lfn_count = count;
    return true;
}
static bool update_file_entry(dir_ref parent, uint32_t short_index,
                              uint32_t first_cluster, uint32_t size) {
    uint8_t raw[32];
    if (!read_directory_entry(parent, short_index, raw) || raw[0] == FAT_ENTRY_DELETED ||
        raw[0] == FAT_ENTRY_END || raw[11] == FAT_ATTR_LFN) return false;
    set_entry_cluster(raw, first_cluster);
    write_le32(raw + 28, size);
    return write_directory_entry(parent, short_index, raw);
}
static bool mark_entries_deleted(dir_ref parent, uint32_t short_index, uint8_t lfn_count) {
    uint8_t raw[32];
    const uint32_t first = short_index >= lfn_count ? short_index - lfn_count : short_index;
    for (uint32_t index = first; index <= short_index; ++index) {
        if (!read_directory_entry(parent, index, raw)) return false;
        raw[0] = FAT_ENTRY_DELETED;
        if (!write_directory_entry(parent, index, raw)) return false;
    }
    return true;
}

static bool volume_refresh(void *context) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!storage_running || file_state.active || dir_state.active) return false;
    volume_ready=false;
    if (!sd_initialize()) { set_error("SD unavailable or initialization failed"); return true; }
    volume_ready=mount_fat(sd_sectors-1);
    if (!volume_ready) set_error("SD is not supported FAT16/FAT32"); else clear_error();
    return true;
}
static bool volume_is_ready(void *context) { (void)context; return volume_ready; }
static bool volume_label(void *context, char *out, size_t capacity) {
    (void)context;
    static const char label[] = "Garden SD";
    if (!out || capacity < sizeof(label)) return false;
    memory_copy(out, label, sizeof(label)); return true;
}
static bool volume_stat(void *context, const char *path, uint64_t *size_out,
                        bool *is_directory_out) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || !path || !size_out || !is_directory_out) return false;
    entry_info found;
    if (!resolve_path(path, &found)) return false;
    *size_out = found.size; *is_directory_out = found.is_directory; return true;
}
static risc_storage_dir_t volume_dir_open(void *context, const char *path) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || handle_sequence==UINT32_MAX || dir_state.active || !path) return RISC_STORAGE_DIR_INVALID;
    dir_ref directory;
    if (!resolve_directory(path, &directory)) return RISC_STORAGE_DIR_INVALID;
    dir_state.active = true; dir_state.token = next_handle();
    dir_state.directory = directory; dir_state.entry_index = 0;
    return dir_state.token;
}
static bool volume_dir_next(void *context, risc_storage_dir_t token,
                            risc_storage_dirent_v1 *entry) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || !dir_state.active || token != dir_state.token || !entry) return false;
    entry_info found;
    if (!scan_next(dir_state.directory, &dir_state.entry_index, &found)) return false;
    memory_zero(entry, sizeof(*entry));
    size_t length = text_length(found.name);
    if (length >= sizeof(entry->name)) length = sizeof(entry->name) - 1u;
    memory_copy(entry->name, found.name, length); entry->name[length] = 0;
    entry->size = found.size; entry->is_directory = found.is_directory ? 1u : 0u;
    return true;
}
static void volume_dir_close(void *context, risc_storage_dir_t token) {
    (void)context;
    if (dir_state.active && token == dir_state.token) dir_state.active = false;
}
static risc_storage_file_t volume_file_open_read(void *context, const char *path,
                                                 uint64_t *size_out) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || file_state.active || !path || !size_out) return RISC_STORAGE_FILE_INVALID;
    entry_info found;
    if (!resolve_path(path, &found) || found.is_directory) return RISC_STORAGE_FILE_INVALID;
    if (found.size && !valid_cluster(found.first_cluster)) return RISC_STORAGE_FILE_INVALID;
    memory_zero(&file_state, sizeof(file_state));
    file_state.active = true; file_state.writable = false; file_state.token = next_handle();
    file_state.first_cluster = found.first_cluster; file_state.current_cluster = found.first_cluster;
    file_state.size = found.size; *size_out = found.size;
    return file_state.token;
}
static bool advance_file_cluster(void) {
    uint32_t next = 0;
    if (!valid_cluster(file_state.current_cluster) || !fat_get(file_state.current_cluster, &next) ||
        end_of_chain(next) || !valid_cluster(next)) return false;
    file_state.current_cluster = next;
    file_state.cluster_base_position += (uint32_t)sectors_per_cluster * 512u;
    return true;
}
static size_t volume_file_read(void *context, risc_storage_file_t token,
                               void *buffer, size_t capacity) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || !file_state.active || file_state.writable || token != file_state.token ||
        (!buffer && capacity)) return 0;
    if (file_state.position >= file_state.size || !capacity) return 0;
    if (capacity>4096) capacity=4096;
    uint8_t *out = (uint8_t *)buffer;
    size_t total = 0;
    const uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512u;
    while (total < capacity && file_state.position < file_state.size) {
        while (file_state.position >= file_state.cluster_base_position + cluster_bytes)
            if (!advance_file_cluster()) { set_error("Broken FAT file chain"); return total; }
        if (!valid_cluster(file_state.current_cluster)) return total;
        const uint32_t within_cluster = file_state.position - file_state.cluster_base_position;
        const uint32_t sector_index = within_cluster / 512u;
        const uint32_t within_sector = within_cluster % 512u;
        const uint32_t lba = data_start_lba + (file_state.current_cluster - 2u) * sectors_per_cluster + sector_index;
        if (!read_sector(lba, sector_buffer)) { set_error("SD read failed"); return total; }
        size_t amount = 512u - within_sector;
        if (amount > capacity - total) amount = capacity - total;
        if (amount > file_state.size - file_state.position) amount = file_state.size - file_state.position;
        memory_copy(out + total, sector_buffer + within_sector, amount);
        total += amount; file_state.position += (uint32_t)amount;
    }
    return total;
}
static risc_storage_file_t volume_file_open_write(void *context, const char *path) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || handle_sequence==UINT32_MAX || file_state.active || !path) return RISC_STORAGE_FILE_INVALID;
    entry_info existing;
    if (resolve_path(path, &existing)) { set_error("Destination already exists"); return RISC_STORAGE_FILE_INVALID; }
    dir_ref parent; char name[RISC_STORAGE_VOLUME_NAME_MAX];
    if (!resolve_parent(path, &parent, name, sizeof(name))) { set_error("Destination folder not found"); return RISC_STORAGE_FILE_INVALID; }
    uint32_t short_index = 0; uint8_t lfn_count = 0;
    if (!create_file_entry(parent, name, &short_index, &lfn_count)) return RISC_STORAGE_FILE_INVALID;
    memory_zero(&file_state, sizeof(file_state));
    file_state.active = true; file_state.writable = true; file_state.token = next_handle();
    file_state.parent = parent; file_state.short_index = short_index; file_state.lfn_count = lfn_count;
    clear_error(); return file_state.token;
}
static bool ensure_write_cluster(void) {
    const uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * 512u;
    if (!file_state.current_cluster) {
        uint32_t cluster = 0;
        if (!allocate_cluster(&cluster)) return false;
        file_state.first_cluster = file_state.current_cluster = cluster;
        file_state.cluster_base_position = 0;
        return true;
    }
    if (file_state.position < file_state.cluster_base_position + cluster_bytes) return true;
    uint32_t next = 0;
    if (!allocate_cluster(&next)) return false;
    if (!fat_set(file_state.current_cluster, next)) { (void)fat_set(next, 0u); return false; }
    file_state.current_cluster = next;
    file_state.cluster_base_position += cluster_bytes;
    return true;
}
static size_t volume_file_write(void *context, risc_storage_file_t token,
                                const void *buffer, size_t size) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || !file_state.active || !file_state.writable || token != file_state.token ||
        (!buffer && size)) return 0;
    if (size>4096) size=4096;
    const uint8_t *source = (const uint8_t *)buffer;
    size_t total = 0;
    while (total < size) {
        if (file_state.position > 0xffffffffu - (uint32_t)(size - total)) {
            set_error("FAT file exceeds 4 GiB"); return total;
        }
        if (!ensure_write_cluster()) return total;
        const uint32_t within_cluster = file_state.position - file_state.cluster_base_position;
        const uint32_t sector_index = within_cluster / 512u;
        const uint32_t within_sector = within_cluster % 512u;
        const uint32_t lba = data_start_lba + (file_state.current_cluster - 2u) * sectors_per_cluster + sector_index;
        size_t amount = 512u - within_sector;
        if (amount > size - total) amount = size - total;
        if (within_sector || amount < 512u) {
            if (!read_sector(lba, sector_buffer)) return total;
        }
        if (!within_sector && amount == 512u) memory_copy(sector_buffer, source + total, 512u);
        else memory_copy(sector_buffer + within_sector, source + total, amount);
        if (!write_sector(lba, sector_buffer)) { set_error("SD write failed"); return total; }
        total += amount; file_state.position += (uint32_t)amount;
        if (file_state.position > file_state.size) file_state.size = file_state.position;
    }
    return total;
}
static bool volume_file_close(void *context, risc_storage_file_t token, bool commit) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!file_state.active || token != file_state.token) return false;
    bool okay = true;
    if (file_state.writable) {
        if (commit) {
            okay = volume_ready && update_file_entry(file_state.parent, file_state.short_index,
                                                     file_state.first_cluster, file_state.size);
            if (!okay) commit = false;
        }
        if (!commit) {
            /* Delete the namespace entry before releasing its clusters. If FAT
             * cleanup fails after that point the worst outcome is leaked space,
             * never a live directory entry pointing at reallocated clusters. */
            if (!mark_entries_deleted(file_state.parent, file_state.short_index,
                                      file_state.lfn_count)) okay = false;
            else if (file_state.first_cluster && !free_chain(file_state.first_cluster)) okay = false;
        }
    }
    memory_zero(&file_state, sizeof(file_state));
    return okay;
}
static bool volume_remove(void *context, const char *path) {
    io_count=0; io_started=timer ? timer->monotonic_ms(timer->context) : 0;
    (void)context;
    if (!volume_ready || file_state.active || dir_state.active || !path) return false;
    entry_info found;
    if (!resolve_path(path, &found) || found.is_directory || (found.attributes & FAT_ATTR_READ_ONLY)) return false;
    dir_ref parent; char name[RISC_STORAGE_VOLUME_NAME_MAX];
    if (!resolve_parent(path, &parent, name, sizeof(name))) return false;
    if (!mark_entries_deleted(parent, found.short_index, found.lfn_count)) return false;
    return !found.first_cluster || free_chain(found.first_cluster);
}
static bool volume_last_error(void *context, char *out, size_t capacity) {
    (void)context;
    if (!out || !capacity || !error_text[0]) return false;
    size_t i = 0;
    while (error_text[i] && i + 1u < capacity) { out[i] = error_text[i]; ++i; }
    out[i] = 0; return true;
}

static const risc_storage_volume_api_v1 volume_api = {
    RISC_STORAGE_VOLUME_API_V1, sizeof(risc_storage_volume_api_v1), 0,
    volume_refresh, volume_is_ready, volume_label, volume_stat,
    volume_dir_open, volume_dir_next, volume_dir_close,
    volume_file_open_read, volume_file_read, volume_file_open_write,
    volume_file_write, volume_file_close, volume_remove, volume_last_error
};

static bool driver_start(const risc_provider_dependency_v1 *d,size_t n) {
    if(storage_running) return false;
    const risc_hw_sd_spi_v1 *next=hardware_config(d,n,"sd-association,sd-spi","storage.sd-spi",sizeof(*next));
    if(!next || !hw_bus(&next->bus,RISC_HW_BUS_SPI) || !hw_pin(next->bus.miso) || !hw_pin(next->cs) || next->detect_active_high>1 || next->write_protect_active_high>1) return false;
    int16_t used[]={next->bus.sclk,next->bus.mosi,next->bus.miso,next->cs,next->detect,next->write_protect};if(!hw_unique(used,6)) return false;
    if(!spi_dependencies(d,n)) return false;
    config=*next;have_config=true;spi_max_hz=config.bus.frequency_hz;
    if(config.detect>=0 && !gpio_input(config.detect)) return false;
    if(config.write_protect>=0 && !gpio_input(config.write_protect)) return false;
    if (!spi->claim(spi->context,config.bus.sclk,config.bus.mosi,config.bus.miso,config.cs,&spi_claim) || !spi_claim) return false;
    storage_running=true; clear_error(); return true;
}
static bool driver_quiesce(void) {
    if (file_state.active || dir_state.active) return false;
    if (!spi_release()) return false;
    if(have_config) { if(config.detect>=0) gpio_release(config.detect);if(config.write_protect>=0) gpio_release(config.write_protect); }
    if(!gpio_clean()) return false;
    have_config=false;storage_running=volume_ready=false; return true;
}
static void driver_stop(void) { (void)driver_quiesce(); }
static const risc_driver_v2 driver={2,sizeof(driver),"storage","storage.volume",1,&volume_api,driver_start,driver_stop,driver_quiesce};
__attribute__((visibility("default"))) const risc_driver_v2 *t5_driver_get(uint32_t abi) { return abi==2?&driver:NULL; }
