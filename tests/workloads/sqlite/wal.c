#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(sqlite3 *db, int rc, const char *step)
{
    if (rc == SQLITE_OK) return;
    fprintf(stderr, "SQLite WAL %s: %d/%d %s\n", step, rc,
            db ? sqlite3_extended_errcode(db) : 0,
            db ? sqlite3_errmsg(db) : "no database");
    exit(1);
}

static void execute(sqlite3 *db, const char *sql)
{
    check(db, sqlite3_exec(db, sql, 0, 0, 0), sql);
}

static sqlite3 *open_database(void)
{
    sqlite3 *db = 0;
    int rc = sqlite3_open_v2("/boaros.db", &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
    check(db, rc, "open");
    sqlite3_busy_timeout(db, 0);
    return db;
}

static void expect(sqlite3 *db, const char *sql, const char *answer)
{
    sqlite3_stmt *query = 0;
    check(db, sqlite3_prepare_v2(db, sql, -1, &query, 0), "prepare");
    if (sqlite3_step(query) != SQLITE_ROW ||
        sqlite3_column_text(query, 0) == 0 ||
        strcmp((const char *)sqlite3_column_text(query, 0), answer)) {
        fprintf(stderr, "SQLite WAL unexpected result: %s\n", sql);
        exit(1);
    }
    check(db, sqlite3_finalize(query), "finalize");
}

static void competitor(int ready, int release)
{
    sqlite3 *db = open_database();
    char signal;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0) != SQLITE_BUSY ||
        write(ready, "b", 1) != 1 ||
        read(release, &signal, 1) != 1 || signal != 'r') _exit(11);
    execute(db, "BEGIN IMMEDIATE; INSERT INTO items VALUES(3,'child'); COMMIT");
    check(db, sqlite3_close(db), "child close");
    _exit(0);
}

static void uncommitted_writer(void)
{
    sqlite3 *db = open_database();
    execute(db, "BEGIN IMMEDIATE; INSERT INTO items VALUES(4,'ghost')");
    _exit(0);
}

static void child_status(pid_t child)
{
    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "SQLite WAL child status=%d\n", status);
        exit(1);
    }
}

static int has_items(sqlite3 *db)
{
    sqlite3_stmt *query = 0;
    check(db, sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='items'",
        -1, &query, 0), "table probe");
    if (sqlite3_step(query) != SQLITE_ROW) exit(1);
    int found = sqlite3_column_int(query, 0);
    check(db, sqlite3_finalize(query), "probe finalize");
    return found;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "competitor")) {
        competitor(atoi(argv[2]), atoi(argv[3]));
    }
    if (argc == 2 && !strcmp(argv[1], "uncommitted")) {
        uncommitted_writer();
    }
    sqlite3 *db = open_database();
    if (sqlite3_libversion_number() != 3053004 ||
        !sqlite3_compileoption_used("THREADSAFE=1")) {
        fprintf(stderr, "SQLite WAL wrong build: %s\n", sqlite3_libversion());
        return 1;
    }
    expect(db, "PRAGMA journal_mode=WAL", "wal");
    expect(db, "PRAGMA locking_mode=NORMAL", "normal");
    expect(db, "PRAGMA mmap_size=0", "0");
    execute(db, "PRAGMA synchronous=FULL");
    expect(db, "PRAGMA synchronous", "2");
    if (has_items(db)) {
        expect(db, "PRAGMA integrity_check", "ok");
        expect(db, "SELECT group_concat(value) FROM "
                   "(SELECT value FROM items ORDER BY id)",
               "base,parent,child");
        check(db, sqlite3_close(db), "reboot close");
        puts("BoarOS: SQLite WAL reboot verified");
        return 42;
    }
    execute(db, "CREATE TABLE items(id INTEGER PRIMARY KEY,value TEXT)");
    execute(db, "INSERT INTO items VALUES(1,'base')");
    execute(db, "BEGIN IMMEDIATE; INSERT INTO items VALUES(2,'parent')");
    int ready[2], release[2];
    if (pipe(ready) || pipe(release)) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        char ready_text[16], release_text[16];
        close(ready[0]); close(release[1]);
        snprintf(ready_text, sizeof(ready_text), "%d", ready[1]);
        snprintf(release_text, sizeof(release_text), "%d", release[0]);
        execl("/init", "/init", "competitor", ready_text,
              release_text, (char *)0);
        _exit(12);
    }
    close(ready[1]); close(release[0]);
    char signal;
    if (read(ready[0], &signal, 1) != 1 || signal != 'b') return 1;
    execute(db, "COMMIT");
    if (write(release[1], "r", 1) != 1) return 1;
    child_status(child);
    close(ready[0]); close(release[1]);
    expect(db, "SELECT group_concat(value) FROM "
               "(SELECT value FROM items ORDER BY id)",
           "base,parent,child");
    check(db, sqlite3_close(db), "close");
    child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        execl("/init", "/init", "uncommitted", (char *)0);
        _exit(13);
    }
    child_status(child);
    db = open_database();
    expect(db, "PRAGMA integrity_check", "ok");
    expect(db, "SELECT group_concat(value) FROM "
               "(SELECT value FROM items ORDER BY id)",
           "base,parent,child");
    check(db, sqlite3_close(db), "final close");
    puts("BoarOS: SQLite WAL multiprocess passed");
    return 42;
}
