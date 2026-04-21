// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int index_load(Index *index);

// PROVIDED
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// PROVIDED
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

// PROVIDED
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// PROVIDED
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// TODO implemented
static int write_tree_recursive(IndexEntry *entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    int i = 0;
    while (i < count) {
        IndexEntry *e = &entries[i];
        char *path = e->path;
        int slashes = 0;
        char *p = path;
        while (slashes < depth && *p) { if (*p == '/') slashes++; p++; }
        char *slash = strchr(p, '/');
        if (slash == NULL) {
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = e->mode;
            snprintf(te->name, sizeof(te->name), "%s", p);
            te->hash = e->hash;
            i++;
        } else {
            size_t dir_len = slash - p;
            char dir_name[256];
            snprintf(dir_name, sizeof(dir_name), "%.*s", (int)dir_len, p);
            int j = i;
            while (j < count) {
                char *pj = entries[j].path;
                int s = 0;
                char *pp = pj;
                while (s < depth && *pp) { if (*pp == '/') s++; pp++; }
                char *sl = strchr(pp, '/');
                if (!sl) break;
                size_t dl = sl - pp;
                if (dl != dir_len || strncmp(pp, dir_name, dir_len) != 0) break;
                j++;
            }
            ObjectID sub_id;
            if (write_tree_recursive(entries + i, j - i, depth + 1, &sub_id) != 0)
                return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000;
            snprintf(te->name, sizeof(te->name), "%s", dir_name);
            te->hash = sub_id;
            i = j;
        }
    }
    void *tdata;
    size_t tlen;
    if (tree_serialize(&tree, &tdata, &tlen) != 0) return -1;
    int rc = object_write(OBJ_TREE, tdata, tlen, id_out);
    free(tdata);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) return -1;
    return write_tree_recursive(index.entries, index.count, 0, id_out);
}
