#include "abi.h"
static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
void abi_script_probe(const unsigned long *sp)
{
    if (sp[0] < 2) return;
    const char **a = (void *)(sp + 1);
    if (equal(a[1], "/script-empty-arg") ||
        (sp[0] >= 3 && equal(a[2], "/script-empty-arg"))) {
        abi_exit(sp[0] == 4 && equal(a[0], "/init") && !a[1][0] &&
                 equal(a[3], "tail") ? 0 : 90);
    }
    if (!equal(a[1], "script-probe with spaces") &&
        !equal(a[1], "script-probe nested")) return;
    const char **env = a + sp[0] + 1;
    int good = equal(a[0], "/init") && env[0] && equal(env[0], "KEEP=yes") && !env[1];
    if (equal(a[1], "script-probe nested"))
        good = good && sp[0] == 6 && equal(a[2], "/script-inner") &&
            equal(a[3], "nested") && equal(a[4], "/script-outer") && equal(a[5], "tail");
    else good = good && sp[0] == 4 && equal(a[2], "/script-ok") && equal(a[3], "tail");
    abi_exit(good ? 0 : 90);
}
static void put(const char *path, const char *text, long mode)
{
    usize len = 0;
    while (text[len]) len++;
    long fd = SC4(56, -100, path, 0x241, mode);
    abi_require(fd >= 0 && SC3(64, fd, text, len) == (long)len && SC1(57, fd) == 0);
}
void abi_script_cases(void)
{
    put("/script-ok", "#! \t/init\tscript-probe with spaces \t\nignored\n", 0755);
    put("/script-inner", "#!/init script-probe nested\n", 0755);
    put("/script-outer", "#!/script-inner nested\n", 0755);
    const char *env[] = {"KEEP=yes", 0};
    for (int nested = 0; nested < 3; nested++) {
        if (nested == 2) put("/script-ok", "#!/init script-probe with spaces", 0755);
        const char *path = nested == 1 ? "/script-outer" : "/script-ok";
        const char *argv[] = {"discarded-argv-zero", "tail", 0};
        long pid = SC5(220, 17, 0, 0, 0, 0);
        abi_require(pid >= 0);
        if (!pid) { SC3(221, path, argv, env); abi_exit(91); }
        int status;
        abi_require(SC4(260, pid, &status, 0, 0) == pid);
        abi_record(nested == 2 ? "exec.script-no-newline" : nested ? "exec.script-nested" : "exec.script-argv-env", 0, -1, -1, status, 0, 0);
    }
    const char *paths[] = {"/script-missing", "/script-loop", "/script-noexec", "/script-empty", "/script-text", "/script-long"};
    const char *texts[] = {"#!/absent-interpreter\n", "#!/script-loop\n", "#!/init\n", "#! \t\n", "echo text\n"};
    const char *ids[] = {"exec.script-missing", "exec.script-loop", "exec.script-noexec", "exec.script-empty", "exec.text-enoexec", "exec.script-long"};
    char long_line[301];
    long_line[0] = '#'; long_line[1] = '!';
    for (unsigned i = 2; i < 300; i++) long_line[i] = 'x';
    long_line[300] = 0;
    for (int i = 0; i < 6; i++) {
        put(paths[i], i == 5 ? long_line : texts[i], i == 2 ? 0644 : 0755);
        const char *argv[] = {paths[i], 0};
        abi_record(ids[i], SC3(221, paths[i], argv, env), -1, -1, 0, 0, 0);
    }
    for (int i = 1; i <= 4; i++) {
        char path[] = "/depth1", text[] = "#!/depth2\n";
        path[6] = '0' + i; text[8] = '1' + i;
        put(path, text, 0755);
    }
    put("/depth5", "#!/init identity-exec-probe\n", 0755);
    const char *argv[] = {"/depth1", 0};
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) { SC3(221, argv[0], argv, env); abi_exit(91); }
    int status;
    abi_require(SC4(260, child, &status, 0, 0) == child);
    abi_record("exec.script-five", 0, -1, -1, status, 0, 0);
    put("/depth5", "#!/depth6\n", 0755);
    const char *last[] = {"#!/init identity-exec-probe\n", "#! \n", "#!/absent-depth\n"};
    const char *depth_ids[] = {"exec.script-six", "exec.script-depth-format", "exec.script-depth-missing"};
    for (int i = 0; i < 3; i++) {
        put("/depth6", last[i], 0755);
        abi_record(depth_ids[i], SC3(221, argv[0], argv, env), -1, -1, 0, 0, 0);
    }

    for (int embedded = 0; embedded < 2; embedded++) {
        put("/script-empty-arg", "#!/init ", 0755);
        if (embedded) {
            const char text[] = "#!/init \0ignored\n";
            long fd = abi_open("/script-empty-arg", 1 | 512);
            abi_require(fd >= 0 && SC3(64, fd, text, sizeof(text) - 1) == sizeof(text) - 1);
            abi_require(SC1(57, fd) == 0);
        }
        const char *args[] = {"ignored", "tail", 0};
        child = SC5(220, 17, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) { SC3(221, "/script-empty-arg", args, env); abi_exit(91); }
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(embedded ? "exec.script-empty-arg-nul" : "exec.script-empty-arg-eof", 0, -1, -1, status, 0, 0);
    }

}
