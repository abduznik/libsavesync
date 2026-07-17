#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

#include "test_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;
static int test_count = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_count++; \
    if (!(cond)) { \
        printf("  FAIL [%d] %s\n", test_count, msg); \
        tests_failed++; \
    } else { \
        printf("  PASS [%d] %s\n", test_count, msg); \
        tests_passed++; \
    } \
} while(0)

typedef struct {
    int to_child_w;
    FILE *from_child;
    pid_t pid;
} ipc_conn_t;

static ipc_conn_t ipc_start(const char *binary_path) {
    ipc_conn_t c;
    c.pid = -1;
    c.from_child = NULL;
    int to_pipe[2], from_pipe[2];
    if (pipe(to_pipe) < 0 || pipe(from_pipe) < 0) {
        perror("pipe");
        return c;
    }
    c.pid = fork();
    if (c.pid < 0) {
        perror("fork");
        return c;
    }
    if (c.pid == 0) {
        close(to_pipe[1]);
        close(from_pipe[0]);
        dup2(to_pipe[0], STDIN_FILENO);
        dup2(from_pipe[1], STDOUT_FILENO);
        close(to_pipe[0]);
        close(from_pipe[1]);
        execl(binary_path, binary_path, (char *)NULL);
        perror("execl");
        _exit(127);
    }
    close(to_pipe[0]);
    close(from_pipe[1]);
    c.to_child_w = to_pipe[1];
    c.from_child = fdopen(from_pipe[0], "r");
    return c;
}

static void ipc_send(ipc_conn_t *c, const char *line) {
    FILE *f = fdopen(dup(c->to_child_w), "w");
    if (!f) { perror("fdopen send"); return; }
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

static char *ipc_recv(ipc_conn_t *c) {
    static char buf[65536];
    if (!c->from_child) return NULL;
    char *line = fgets(buf, sizeof(buf), c->from_child);
    if (line) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
    }
    return line;
}

static int ipc_stop(ipc_conn_t *c) {
    if (c->from_child) { fclose(c->from_child); c->from_child = NULL; }
    close(c->to_child_w);
    int status = -1;
    if (c->pid > 0) {
        for (int i = 0; i < 20; i++) {
            int rc = waitpid(c->pid, &status, WNOHANG);
            if (rc > 0) break;
            usleep(100000);
        }
        if (waitpid(c->pid, &status, WNOHANG) <= 0) {
            kill(c->pid, SIGTERM);
            waitpid(c->pid, &status, 0);
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int json_contains(const char *json, const char *key, const char *value) {
    char needle[1024];
    snprintf(needle, sizeof(needle), "\"%s\":\"%s\"", key, value);
    if (strstr(json, needle)) return 1;
    snprintf(needle, sizeof(needle), "\"%s\":%s", key, value);
    if (strstr(json, needle)) return 1;
    return 0;
}

static int json_has_key(const char *json, const char *key) {
    char needle[512];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    return strstr(json, needle) != NULL;
}

static int json_has_error(const char *json) {
    return strstr(json, "\"error\"") != NULL;
}

int main(void) {
    char tmpdir[] = "/tmp/libsavesync_ipc_test_XXXXXX";
    if (!sv_mkdtemp(tmpdir)) {
        printf("FAIL: could not create temp dir\n");
        return 1;
    }

    printf("\n=== libsavesync IPC Integration Test Suite ===\n\n");
    printf("Temp dir: %s\n\n", tmpdir);

    const char *ipc_bin = "build/savesync-ipc";
    if (access(ipc_bin, X_OK) != 0) {
        ipc_bin = "./build/savesync-ipc";
        if (access(ipc_bin, X_OK) != 0) {
            printf("FATAL: cannot find savesync-ipc binary\n");
            return 1;
        }
    }

    /* Create test fixture: synthetic manifest */
    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/test.cfg", tmpdir);
    {
        FILE *f = fopen(manifest_path, "w");
        fprintf(f, "platform=ps2\n");
        fprintf(f, "emulator=testemu\n");
        fprintf(f, "shape=file\n");
        fprintf(f, "identity=none\n");
        fclose(f);
    }

    /* Create a live save directory with a test file */
    char live_path[4096];
    snprintf(live_path, sizeof(live_path), "%s/live_save", tmpdir);
    sv_mkdir(live_path);
    char save_file[4096];
    snprintf(save_file, sizeof(save_file), "%s/data.bin", live_path);
    {
        FILE *f = fopen(save_file, "wb");
        const char *data = "SYNTHETIC_SAVE_V1";
        fwrite(data, 1, strlen(data), f);
        fclose(f);
    }

    /* Start IPC binary */
    ipc_conn_t ipc = ipc_start(ipc_bin);
    TEST_ASSERT(ipc.pid > 0, "IPC binary started");

    /* ---- init ---- */
    {
        char msg[4096];
        snprintf(msg, sizeof(msg),
            "{\"id\":\"t1\",\"method\":\"init\",\"params\":{\"base_path\":\"%s/data\"}}",
            tmpdir);
        ipc_send(&ipc, msg);
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "init: got response");
        TEST_ASSERT(json_contains(resp, "id", "t1"), "init: id echoed");
        TEST_ASSERT(json_contains(resp, "ok", "true"), "init: ok=true");
    }

    /* ---- manifest_load ---- */
    {
        char msg[4096];
        snprintf(msg, sizeof(msg),
            "{\"id\":\"t2\",\"method\":\"manifest_load\",\"params\":{\"path\":\"%s\"}}",
            manifest_path);
        ipc_send(&ipc, msg);
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "manifest_load: got response");
        TEST_ASSERT(json_contains(resp, "id", "t2"), "manifest_load: id echoed");
        TEST_ASSERT(json_has_key(resp, "manifest_id"), "manifest_load: has manifest_id");
    }

    /* ---- register_with_manifest ---- */
    {
        char msg[8192];
        snprintf(msg, sizeof(msg),
            "{\"id\":\"t3\",\"method\":\"register_with_manifest\","
            "\"params\":{\"manifest_id\":\"m0\",\"live_path\":\"%s\",\"shape\":\"file\",\"retention_count\":5}}",
            save_file);
        ipc_send(&ipc, msg);
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "register_with_manifest: got response");
        TEST_ASSERT(json_contains(resp, "id", "t3"), "register_with_manifest: id echoed");
        TEST_ASSERT(json_has_key(resp, "registration_id"), "register_with_manifest: has registration_id");
    }

    /* ---- save ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t4\",\"method\":\"save\",\"params\":{\"registration_id\":\"r0\"}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "save: got response");
        TEST_ASSERT(json_contains(resp, "id", "t4"), "save: id echoed");
        TEST_ASSERT(json_has_key(resp, "entry_id"), "save: has entry_id");
        TEST_ASSERT(json_contains(resp, "entry_created", "true"), "save: entry_created=true");
    }

    /* ---- pull ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t5\",\"method\":\"pull\",\"params\":{\"registration_id\":\"r0\"}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "pull: got response");
        TEST_ASSERT(json_contains(resp, "id", "t5"), "pull: id echoed");
        TEST_ASSERT(json_has_key(resp, "did_pull"), "pull: has did_pull");
    }

    /* ---- list_entries ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t6\",\"method\":\"list_entries\",\"params\":{\"registration_id\":\"r0\"}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "list_entries: got response");
        TEST_ASSERT(json_contains(resp, "id", "t6"), "list_entries: id echoed");
        TEST_ASSERT(json_has_key(resp, "entries"), "list_entries: has entries array");
    }

    /* ---- list_registrations ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t7\",\"method\":\"list_registrations\",\"params\":{}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "list_registrations: got response");
        TEST_ASSERT(json_contains(resp, "id", "t7"), "list_registrations: id echoed");
        TEST_ASSERT(json_has_key(resp, "registrations"), "list_registrations: has registrations array");
    }

    /* ---- malformed JSON error ---- */
    {
        ipc_send(&ipc, "NOT_JSON_AT_ALL");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "malformed JSON: got response");
        TEST_ASSERT(json_has_error(resp), "malformed JSON: error response");
        TEST_ASSERT(json_contains(resp, "code", "-100"), "malformed JSON: error code -100");
    }

    /* ---- unknown method error ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t9\",\"method\":\"bogus_method\",\"params\":{}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "unknown method: got response");
        TEST_ASSERT(json_has_error(resp), "unknown method: error response");
        TEST_ASSERT(json_contains(resp, "code", "-101"), "unknown method: error code -101");
    }

    /* ---- missing required param error ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t10\",\"method\":\"init\",\"params\":{}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "missing param: got response");
        TEST_ASSERT(json_has_error(resp), "missing param: error response");
    }

    /* ---- shutdown ---- */
    {
        ipc_send(&ipc, "{\"id\":\"t11\",\"method\":\"shutdown\",\"params\":{}}");
        char *resp = ipc_recv(&ipc);
        TEST_ASSERT(resp != NULL, "shutdown: got response");
        TEST_ASSERT(json_contains(resp, "id", "t11"), "shutdown: id echoed");
        TEST_ASSERT(json_contains(resp, "ok", "true"), "shutdown: ok=true");
    }

    int exit_code = ipc_stop(&ipc);
    TEST_ASSERT(exit_code == 0, "IPC binary exited cleanly (status 0)");

    /* ---- Cleanup ---- */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmpdir);
    system(cmd);

    printf("\n=== Results: %d passed, %d failed out of %d ===\n\n",
           tests_passed, tests_failed, test_count);

    return tests_failed > 0 ? 1 : 0;
}
