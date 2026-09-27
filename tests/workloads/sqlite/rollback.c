#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int result, sqlite3 *db, const char *step)
{
    if (result == SQLITE_OK) return;
    fprintf(stderr, "SQLite %s: %d %s\n", step, result,
            db ? sqlite3_errmsg(db) : "no database");
    exit(1);
}

static void statement(sqlite3 *db, const char *sql)
{
    check(sqlite3_exec(db, sql, 0, 0, 0), db, sql);
}

static void expect_text(sqlite3 *db, const char *sql, const char *expected)
{
    sqlite3_stmt *query = 0;
    check(sqlite3_prepare_v2(db, sql, -1, &query, 0), db, "prepare");
    if (sqlite3_step(query) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(query, 0), expected)) {
        fprintf(stderr, "SQLite unexpected answer: %s\n", sql);
        exit(1);
    }
    check(sqlite3_finalize(query), db, "finalize");
}

static void competing_connection(int ready_fd, int released_fd)
{
    sqlite3 *other = 0;
    char marker;
    if (sqlite3_open("/boaros.db", &other) != SQLITE_OK) _exit(2);
    sqlite3_busy_timeout(other, 0);
    int busy = sqlite3_exec(other, "BEGIN IMMEDIATE", 0, 0, 0);
    if (busy != SQLITE_BUSY || write(ready_fd, "b", 1) != 1 ||
        read(released_fd, &marker, 1) != 1 || marker != 'r') _exit(3);
    int retry = sqlite3_exec(other, "BEGIN IMMEDIATE;"
                                    "INSERT INTO items VALUES(4, 'child');"
                                    "COMMIT", 0, 0, 0);
    if (retry != SQLITE_OK) {
        fprintf(stderr, "SQLite child retry: %d/%d %s\n", retry,
                sqlite3_extended_errcode(other), sqlite3_errmsg(other));
        fflush(stderr);
        _exit(4);
    }
    if (sqlite3_close(other) != SQLITE_OK) _exit(5);
    _exit(0);
}

static void crash_writer(void)
{
    sqlite3 *db = 0;
    if (sqlite3_open("/boaros.db", &db) != SQLITE_OK) _exit(7);
    statement(db, "PRAGMA journal_mode=DELETE");
    statement(db, "PRAGMA locking_mode=NORMAL");
    statement(db, "PRAGMA mmap_size=0");
    statement(db, "PRAGMA synchronous=EXTRA");
    statement(db, "PRAGMA cache_size=2");
    statement(db, "BEGIN IMMEDIATE");
    statement(db, "UPDATE spill SET value=replace(value,'0','1')");
    puts("BoarOS: SQLite process hot journal ready");
    fflush(stdout);
    _exit(0); /* No COMMIT, ROLLBACK, SQLite close, or application cleanup. */
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "competitor")) {
        competing_connection(atoi(argv[2]), atoi(argv[3]));
    }
    if (argc == 2 && !strcmp(argv[1], "crashwriter")) crash_writer();
    sqlite3 *db = 0;
    int rc = sqlite3_open_v2("/boaros.db", &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
    check(rc, db, "open");
    if (sqlite3_libversion_number() != 3053004) {
        fprintf(stderr, "SQLite unexpected version: %s\n", sqlite3_libversion());
        return 1;
    }
    expect_text(db, "PRAGMA journal_mode=DELETE", "delete");
    expect_text(db, "PRAGMA locking_mode=NORMAL", "normal");
    expect_text(db, "PRAGMA mmap_size=0", "0");
    statement(db, "PRAGMA synchronous=EXTRA");
    expect_text(db, "PRAGMA synchronous", "3");
    statement(db, "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY, value TEXT)");
    statement(db, "DELETE FROM items");
    statement(db, "BEGIN IMMEDIATE");
    statement(db, "INSERT INTO items VALUES(1, 'committed')");
    statement(db, "COMMIT");
    statement(db, "BEGIN IMMEDIATE");
    statement(db, "INSERT INTO items VALUES(2, 'rolled back')");
    statement(db, "ROLLBACK");
    check(sqlite3_close(db), db, "close");

    db = 0;
    check(sqlite3_open("/boaros.db", &db), db, "reopen");
    expect_text(db, "PRAGMA integrity_check", "ok");
    expect_text(db, "SELECT group_concat(value) FROM items", "committed");

    statement(db, "BEGIN IMMEDIATE");
    statement(db, "INSERT INTO items VALUES(3, 'parent')");
    int ready[2], released[2];
    if (pipe(ready) || pipe(released)) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        char ready_text[16], released_text[16];
        close(ready[0]);
        close(released[1]);
        snprintf(ready_text, sizeof(ready_text), "%d", ready[1]);
        snprintf(released_text, sizeof(released_text), "%d", released[0]);
        execl("/init", "/init", "competitor", ready_text,
              released_text, (char *)0);
        _exit(6);
    }
    close(ready[1]);
    close(released[0]);
    char marker;
    if (read(ready[0], &marker, 1) != 1 || marker != 'b') return 1;
    statement(db, "COMMIT");
    if (write(released[1], "r", 1) != 1) return 1;
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "SQLite child status: %d\n", child_status);
        return 1;
    }
    close(ready[0]);
    close(released[1]);
    expect_text(db, "SELECT group_concat(value) FROM items", "committed,parent,child");
    statement(db, "CREATE TABLE spill(id INTEGER PRIMARY KEY, value TEXT)");
    statement(db, "WITH RECURSIVE nums(x) AS (SELECT 1 UNION ALL "
                  "SELECT x+1 FROM nums WHERE x<24) "
                  "INSERT INTO spill SELECT x, printf('%03000d',0) FROM nums");
    check(sqlite3_close(db), db, "close reopened");
    child = fork();
    if (child < 0) return 1;
    if (!child) {
        execl("/init", "/init", "crashwriter", (char *)0);
        _exit(8);
    }
    child_status = 0;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) return 1;
    db = 0;
    check(sqlite3_open("/boaros.db", &db), db, "post-crash reopen");
    expect_text(db, "PRAGMA integrity_check", "ok");
    expect_text(db, "SELECT count(*) FROM spill", "24");
    expect_text(db, "SELECT count(*) FROM spill WHERE value LIKE '1%'", "0");
    check(sqlite3_close(db), db, "post-crash close");
    puts("BoarOS: SQLite rollback smoke passed");
    return 42;
}
