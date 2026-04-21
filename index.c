// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration — object_write is implemented in object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── Helper ──────────────────────────────────────────────────────────────────

// Sort comparator: index entries are stored sorted by path so diffs and
// tree construction get deterministic output regardless of add order.
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // Index file doesn't exist yet — treat as an empty staging area (not an error)
        return 0;
    }

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *entry = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 2];
        unsigned int mode;
        unsigned long long mtime;
        unsigned int size;

        // Format: <mode-octal> <64-hex-hash> <mtime> <size> <path>
        int ret = fscanf(f, "%o %64s %llu %u %511s\n",
                         &mode, hex, &mtime, &size, entry->path);
        if (ret == EOF) break;
        if (ret != 5) { fclose(f); return -1; }

        entry->mode      = (uint32_t)mode;
        entry->mtime_sec = (uint64_t)mtime;
        entry->size      = (uint32_t)size;

        if (hex_to_hash(hex, &entry->hash) != 0) { fclose(f); return -1; }

        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    // Heap-allocate the sorted copy: Index is ~5.6 MB, too large for the stack
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, (size_t)sorted->count, sizeof(IndexEntry), compare_index_entries);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                sorted->entries[i].mode,
                hex,
                (unsigned long long)sorted->entries[i].mtime_sec,
                (unsigned int)sorted->entries[i].size,
                sorted->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    // Step 1: Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    void *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return -1; }

    if (file_size > 0 && fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // Step 2: Store contents as a blob object in the object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, buf, (size_t)file_size, &blob_id) != 0) {
        free(buf); return -1;
    }
    free(buf);

    // Step 3: Stat the file to capture metadata for fast change detection
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // Step 4: Update an existing entry or append a new one
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = blob_id;
        existing->mode      = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *entry = &index->entries[index->count++];
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
        entry->hash      = blob_id;
        entry->mode      = mode;
        entry->mtime_sec = (uint64_t)st.st_mtime;
        entry->size      = (uint32_t)st.st_size;
    }

    // Step 5: Persist the updated staging area atomically
    return index_save(index);
}
