#include <sqlite3.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VALUE_SIZE = 3000 };

static void fail(sqlite3 *db, int result, const char *step)
{
    fprintf(stderr, "SQLite recovery %s: %d/%d %s\n", step, result,
            db ? sqlite3_extended_errcode(db) : 0,
            db ? sqlite3_errmsg(db) : "no database");
    exit(1);
}

static void require(sqlite3 *db, int result, const char *step)
{
    if (result != SQLITE_OK) fail(db, result, step);
}

static void statement(sqlite3 *db, const char *sql)
{
    require(db, sqlite3_exec(db, sql, 0, 0, 0), sql);
}

static void answer(sqlite3 *db, const char *sql, const char *expected)
{
    sqlite3_stmt *query = 0;
    require(db, sqlite3_prepare_v2(db, sql, -1, &query, 0), "prepare");
    if (sqlite3_step(query) != SQLITE_ROW ||
        !sqlite3_column_text(query, 0) ||
        strcmp((const char *)sqlite3_column_text(query, 0), expected))
        fail(db, SQLITE_MISMATCH, sql);
    require(db, sqlite3_finalize(query), "finalize");
}

static char control(const char *path)
{
    char value;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || read(fd, &value, 1) != 1 || close(fd)) {
        perror(path);
        exit(1);
    }
    return value;
}

static void setup(sqlite3 *db, int rows)
{
    statement(db, "PRAGMA page_size=4096");
    statement(db, "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)");
    statement(db, "BEGIN IMMEDIATE");
    sqlite3_stmt *insert = 0;
    require(db, sqlite3_prepare_v2(db,
            "INSERT INTO items VALUES(?1,?2)", -1, &insert, 0), "insert prepare");
    char value[VALUE_SIZE + 1];
    memset(value, 'a', VALUE_SIZE);
    value[VALUE_SIZE] = 0;
    for (int id = 1; id <= rows; ++id) {
        require(db, sqlite3_bind_int(insert, 1, id), "bind id");
        require(db, sqlite3_bind_text(insert, 2, value, VALUE_SIZE,
                                       SQLITE_STATIC), "bind value");
        if (sqlite3_step(insert) != SQLITE_DONE)
            fail(db, sqlite3_errcode(db), "insert");
        require(db, sqlite3_reset(insert), "reset");
        require(db, sqlite3_clear_bindings(insert), "clear bindings");
    }
    require(db, sqlite3_finalize(insert), "insert finalize");
    statement(db, "COMMIT");
    puts("BoarOS: SQLite recovery setup complete");
}

static void mutate(sqlite3 *db, int pause_at)
{
    statement(db, "PRAGMA cache_size=2");
    statement(db, "PRAGMA cache_spill=ON");
    puts("BoarOS: SQLite mutation begin");
    fflush(stdout);
    statement(db, "BEGIN IMMEDIATE");
    statement(db, "UPDATE items SET value=replace(value,'a','b')");
    if (pause_at == 1) {
        puts("BoarOS: SQLite hot transaction ready");
        fflush(stdout);
        for (;;) pause();
    }
    statement(db, "COMMIT");
    puts("BoarOS: SQLite commit confirmed");
    fflush(stdout);
    if (pause_at == 2) for (;;) pause();
}

static void verify(sqlite3 *db, int rows)
{
    answer(db, "PRAGMA integrity_check", "ok");
    sqlite3_stmt *query = 0;
    require(db, sqlite3_prepare_v2(db,
        "SELECT value FROM items ORDER BY id", -1, &query, 0),
        "verify prepare");
    int old = 0, fresh = 0;
    for (int row = 0; row < rows; ++row) {
        if (sqlite3_step(query) != SQLITE_ROW ||
            sqlite3_column_bytes(query, 0) != VALUE_SIZE)
            fail(db, SQLITE_CORRUPT, "row shape");
        const unsigned char *value = sqlite3_column_text(query, 0);
        if (!value || (value[0] != 'a' && value[0] != 'b'))
            fail(db, SQLITE_CORRUPT, "row state");
        for (int byte = 0; byte < VALUE_SIZE; ++byte)
            if (value[byte] != value[0])
                fail(db, SQLITE_CORRUPT, "torn value");
        if (value[0] == 'a') old++;
        else fresh++;
    }
    if (sqlite3_step(query) != SQLITE_DONE ||
        !((old == rows && fresh == 0) ||
          (old == 0 && fresh == rows))) fail(db, SQLITE_CORRUPT, "atomic rows");
    require(db, sqlite3_finalize(query), "verify finalize");
    printf("BoarOS: SQLite recovery state=%s\n", old ? "old" : "new");
}

int main(void)
{
    char phase = control("/phase"), sync = control("/sync");
    char journal = control("/journal");
    int rows = control("/size") == '1' ? 1 : 24;
    sqlite3 *db = 0;
    if (sqlite3_libversion_number() != 3053004 ||
        !sqlite3_compileoption_used("THREADSAFE=1") ||
        !sqlite3_vfs_find(0) ||
        strcmp(sqlite3_vfs_find(0)->zName, "unix")) {
        fprintf(stderr, "SQLite recovery binary identity mismatch\n");
        return 1;
    }
    require(db, sqlite3_open_v2("/recovery.db", &db,
              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0), "open");
    if (journal == 'W') {
        answer(db, "PRAGMA journal_mode=WAL", "wal");
        puts("BoarOS: SQLite journal=wal");
        fflush(stdout);
    } else if (journal == 'D') {
        answer(db, "PRAGMA journal_mode=DELETE", "delete");
    } else return 1;
    answer(db, "PRAGMA locking_mode=NORMAL", "normal");
    answer(db, "PRAGMA mmap_size=0", "0");
    statement(db, sync == 'F' ? "PRAGMA synchronous=FULL" :
                                 "PRAGMA synchronous=EXTRA");
    answer(db, "PRAGMA synchronous", sync == 'F' ? "2" : "3");
    if (phase == 'S') setup(db, rows);
    else if (phase == 'M' || phase == 'H' || phase == 'C' || phase == 'G') {
        if (phase == 'G') {
            char acknowledged;
            /* Isolate the armed mutation from asynchronous preparation
             * metadata (including loader and control-file access times). */
            int directory = open("/", O_RDONLY | O_DIRECTORY);
            if (directory < 0 || fsync(directory) || close(directory)) {
                perror("pre-arm namespace sync");
                return 1;
            }
            puts("BoarOS: SQLite mutation armed");
            fflush(stdout);
            if (read(0, &acknowledged, 1) != 1 || acknowledged != 'g')
                return 1;
        }
        mutate(db, phase == 'H' ? 1 :
                    phase == 'C' || phase == 'G' ? 2 : 0);
    }
    else if (phase == 'R') verify(db, rows);
    else return 1;
    require(db, sqlite3_close(db), "close");
    return 42;
}
