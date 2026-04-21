// commit.c — Commit creation and history traversal

#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;
    const char *p = (const char *)data;
    char hex[HASH_HEX_SIZE + 1];

    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n') + 1;

    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        commit_out->has_parent = 0;
    }

    char author_buf[256];
    uint64_t ts;
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;

    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;

    ts = (uint64_t)strtoull(last_space + 1, NULL, 10);
    *last_space = '\0';

    snprintf(commit_out->author, sizeof(commit_out->author), "%s", author_buf);
    commit_out->timestamp = ts;

    p = strchr(p, '\n') + 1; // author
    p = strchr(p, '\n') + 1; // committer
    p = strchr(p, '\n') + 1; // blank line

    snprintf(commit_out->message, sizeof(commit_out->message), "%s", p);
    return 0;
}

// 🔥 FIXED SERIALIZATION (MAIN ISSUE FIXED HERE)
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    char parent_hex[HASH_HEX_SIZE + 1];

    hash_to_hex(&commit->tree, tree_hex);

    char buffer[4096];
    int offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "tree %s\n", tree_hex);

    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "parent %s\n", parent_hex);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "author %s %" PRIu64 "\n",
                       commit->author, commit->timestamp);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "committer %s %" PRIu64 "\n\n",
                       commit->author, commit->timestamp);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "%s\n", commit->message);

    *data_out = malloc(offset);
    if (!*data_out) return -1;

    memcpy(*data_out, buffer, offset);
    *len_out = offset;

    return 0;
}

// ─── WALK ────────────────────────────────────────────────────────────────────

int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;

        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;

        Commit c;
        if (commit_parse(raw, raw_len, &c) != 0) {
            free(raw);
            return -1;
        }

        free(raw);
        callback(&id, &c, ctx);

        if (!c.has_parent) break;
        id = c.parent;
    }
    return 0;
}

// ─── HEAD ────────────────────────────────────────────────────────────────────

int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    line[strcspn(line, "\r\n")] = '\0';

    char ref_path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(ref_path, sizeof(ref_path), "%s/%s", PES_DIR, line + 5);
        f = fopen(ref_path, "r");
        if (!f) return -1;

        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return -1;
        }
        fclose(f);

        line[strcspn(line, "\r\n")] = '\0';
    }

    return hex_to_hash(line, id_out);
}

int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    line[strcspn(line, "\r\n")] = '\0';

    char target_path[520];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(target_path, sizeof(target_path), "%s/%s", PES_DIR, line + 5);
    } else {
        snprintf(target_path, sizeof(target_path), "%s", HEAD_FILE);
    }

    char tmp_path[528];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target_path);

    f = fopen(tmp_path, "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);

    fprintf(f, "%s\n", hex);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, target_path);
}

// ─── FINAL FUNCTION ──────────────────────────────────────────────────────────

int commit_create(const char *message, ObjectID *commit_id_out) {
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) return -1;

    Commit commit;
    memset(&commit, 0, sizeof(commit));

    commit.tree = tree_id;

    ObjectID parent_id;
    if (head_read(&parent_id) == 0) {
        commit.parent = parent_id;
        commit.has_parent = 1;
    }

    strncpy(commit.author, pes_author(), sizeof(commit.author) - 1);
    strncpy(commit.message, message, sizeof(commit.message) - 1);
    commit.timestamp = (uint64_t)time(NULL);

    void *data;
    size_t len;

    if (commit_serialize(&commit, &data, &len) != 0) return -1;

    ObjectID commit_id;
    if (object_write(OBJ_COMMIT, data, len, &commit_id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    if (head_update(&commit_id) != 0) return -1;

    if (commit_id_out) *commit_id_out = commit_id;

    return 0;
}
