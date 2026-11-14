#include "chat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

static sqlite3* g_db;

static const char* const SCHEMA =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id        INTEGER PRIMARY KEY,"
    "  time      INTEGER NOT NULL,"                         // unix seconds, board clock
    "  dir       TEXT    NOT NULL CHECK (dir IN ('in', 'out')),"
    "  sender    INTEGER NOT NULL,"                         // ERL addresses
    "  receiver  INTEGER NOT NULL,"
    "  broadcast INTEGER NOT NULL,"
    "  text      TEXT    NOT NULL,"
    "  status    TEXT    NOT NULL"                          // received | pending | sent | failed: ...
    ");"
    // key/value: last_id, the highest id ever given (chat_clear() empties messages, but new
    // messages must still get higher ids: clients such as the app remember the last one seen)
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);";

static void log_error(const char* what) {
    fprintf(stderr, "chat: %s: %s\n", what, g_db ? sqlite3_errmsg(g_db) : "no database");
}

bool chat_open(const char* path) {
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        log_error(path);
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }
    sqlite3_busy_timeout(g_db, 1000);
    if (sqlite3_exec(g_db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        log_error("schema");
        chat_close();
        return false;
    }
    return true;
}

void chat_close(void) {
    sqlite3_close(g_db);
    g_db = NULL;
}

int64_t chat_add(const char* dir, uint16_t sender, uint16_t receiver, bool broadcast, const uint8_t* text,
                 size_t length, const char* status) {
    if (!g_db)
        return -1;
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(g_db,
                           "INSERT INTO messages (id, time, dir, sender, receiver, broadcast, text, status) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &st, NULL) != SQLITE_OK) {
        log_error("insert");
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(chat_last_id() + 1));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(st, 3, dir, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, sender);
    sqlite3_bind_int(st, 5, receiver);
    sqlite3_bind_int(st, 6, broadcast);
    sqlite3_bind_text(st, 7, (const char*)text, (int)length, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, status, -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        log_error("insert");
        return -1;
    }
    return (int64_t)sqlite3_last_insert_rowid(g_db);
}

void chat_set_status(int64_t id, const char* status) {
    if (!g_db || id < 0)
        return;
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(g_db, "UPDATE messages SET status = ? WHERE id = ?", -1, &st, NULL) != SQLITE_OK) {
        log_error("update");
        return;
    }
    sqlite3_bind_text(st, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
    if (sqlite3_step(st) != SQLITE_DONE)
        log_error("update");
    sqlite3_finalize(st);
}

static int64_t scalar(const char* sql) {
    if (!g_db)
        return 0;
    sqlite3_stmt* st;
    int64_t value = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        value = (int64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return value;
}

int64_t chat_last_id(void) {
    return scalar("SELECT MAX(COALESCE((SELECT MAX(id) FROM messages), 0),"
                  " COALESCE((SELECT value FROM meta WHERE key = 'last_id'), 0))");
}

long chat_clear(void) {
    if (!g_db)
        return -1;
    const int64_t last = chat_last_id();
    const long count = chat_count();
    char sql[160];
    snprintf(sql, sizeof(sql),
             "BEGIN;"
             "INSERT OR REPLACE INTO meta (key, value) VALUES ('last_id', %lld);"
             "DELETE FROM messages;"
             "COMMIT;",
             (long long)last);
    if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        log_error("clear");
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    return count;
}

bool chat_check(void) {
    if (!g_db)
        return false;
    sqlite3_stmt* st;
    bool ok = false;
    if (sqlite3_prepare_v2(g_db, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const char* result = (const char*)sqlite3_column_text(st, 0);
        ok = result && strcmp(result, "ok") == 0;
        if (!ok)
            fprintf(stderr, "chat: integrity check: %s\n", result ? result : "?");
    } else {
        log_error("integrity check");
    }
    sqlite3_finalize(st);
    // CREATE TABLE IF NOT EXISTS keeps an existing table as it is: make sure it has our columns
    if (sqlite3_prepare_v2(g_db, "SELECT id, time, dir, sender, receiver, broadcast, text, status FROM messages",
                           -1, &st, NULL) != SQLITE_OK) {
        log_error("messages table");
        ok = false;
    }
    sqlite3_finalize(st);
    return ok;
}

long chat_count(void) {
    return (long)scalar("SELECT COUNT(*) FROM messages");
}

void chat_json(struct buf* b, int64_t after, int limit) {
    buf_printf(b, "[");
    if (!g_db) {
        buf_printf(b, "]");
        return;
    }
    // newest first in the inner query, so "the last N" works; returned oldest first
    const char* sql = after > 0
        ? "SELECT * FROM (SELECT id, time, dir, sender, receiver, broadcast, text, status FROM messages "
          "WHERE id > ?1 ORDER BY id ASC LIMIT ?2) ORDER BY id ASC"
        : "SELECT * FROM (SELECT id, time, dir, sender, receiver, broadcast, text, status FROM messages "
          "ORDER BY id DESC LIMIT ?2) ORDER BY id ASC";
    sqlite3_stmt* st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        log_error("select");
        buf_printf(b, "]");
        return;
    }
    if (after > 0)
        sqlite3_bind_int64(st, 1, (sqlite3_int64)after);
    sqlite3_bind_int(st, 2, limit);

    bool first = true;
    while (sqlite3_step(st) == SQLITE_ROW) {
        buf_printf(b, "%s{\"id\":%lld,\"time\":%lld,\"dir\":\"%s\",\"from\":\"0x%04X\",\"to\":\"0x%04X\","
                      "\"broadcast\":%s,\"text\":",
                   first ? "" : ",", (long long)sqlite3_column_int64(st, 0), (long long)sqlite3_column_int64(st, 1),
                   (const char*)sqlite3_column_text(st, 2), (unsigned)sqlite3_column_int(st, 3),
                   (unsigned)sqlite3_column_int(st, 4), sqlite3_column_int(st, 5) ? "true" : "false");
        buf_json_string(b, sqlite3_column_text(st, 6), (size_t)sqlite3_column_bytes(st, 6));
        buf_printf(b, ",\"status\":");
        buf_json_string(b, sqlite3_column_text(st, 7), (size_t)sqlite3_column_bytes(st, 7));
        buf_printf(b, "}");
        first = false;
    }
    sqlite3_finalize(st);
    buf_printf(b, "]");
}
