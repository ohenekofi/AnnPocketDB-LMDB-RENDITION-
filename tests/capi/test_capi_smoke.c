/*
 * tests/capi/test_capi_smoke.c
 *
 * Plain C99 smoke test for the annpocketdb_c shared library.
 * No test framework — every assertion uses the ASSERT macro below,
 * which prints the failing expression + line number and exits 1.
 *
 * Exercises:
 *   1. Open / close environment.
 *   2. Default-keyspace single-op put / get / del.
 *   3. Explicit read and write transactions.
 *   4. Read cursor — full forward iteration.
 *   5. Write cursor — put via cursor, del via cursor.
 *   6. apdb_get returns APDB_NOT_FOUND for missing key.
 *   7. Named sub-database create / put / get / list / drop.
 *   8. apdb_bytes_copy / apdb_bytes_free (durable copy).
 *   9. apdb_env_stat / apdb_env_info.
 *  10. apdb_version_check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
/* kDbPath is a plain *file* path (Environment::open creates it directly),
   not a directory -- clean up the file itself plus its .lock and .wal
   sidecars. Each command is best-effort/silently ignored (2>nul) since not
   every test leaves every sidecar behind (e.g. no WAL, no .lock in
   single-process mode). */
static void rm_rf(const char* path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "del /q \"%s\" 2>nul & rd /s /q \"%s\" 2>nul & del /q \"%s.lock\" 2>nul & rd /s /q \"%s.wal\" 2>nul",
             path, path, path, path);
    system(cmd);
}
#else
#  include <sys/stat.h>
#  include <sys/types.h>
static void rm_rf(const char* path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s.lock' '%s.wal'", path, path, path);
    system(cmd);
}
#endif

#include "annpocketdb_c/annpocketdb.h"

/* -------------------------------------------------------------------------- */
/* Assertion helpers                                                           */
/* -------------------------------------------------------------------------- */

static int g_failures = 0;

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        g_failures++; \
    } \
} while (0)

#define ASSERT_RC(call) do { \
    APDB_rc _rc = (call); \
    if (_rc != APDB_OK) { \
        fprintf(stderr, "FAIL  %s:%d  %s  =>  %s (%d)\n", \
                __FILE__, __LINE__, #call, apdb_errmsg(_rc), _rc); \
        g_failures++; \
    } \
} while (0)

#define ASSERT_RC_EQ(call, expected) do { \
    APDB_rc _rc = (call); \
    if (_rc != (expected)) { \
        fprintf(stderr, "FAIL  %s:%d  %s  =>  %s (%d), expected %d\n", \
                __FILE__, __LINE__, #call, apdb_errmsg(_rc), _rc, (expected)); \
        g_failures++; \
    } \
} while (0)

/* -------------------------------------------------------------------------- */
/* Test helpers                                                                */
/* -------------------------------------------------------------------------- */

static const char* kDbPath = "test_capi_tmp_db";

static APDB_Env open_env(void) {
    APDB_Env env = NULL;
    /* kDbPath is the data *file* path itself (Environment::open creates it),
       not a directory to pre-create -- see rm_rf's comment below. */
    ASSERT_RC(apdb_env_open(kDbPath, NULL, &env));
    return env;
}

/* -------------------------------------------------------------------------- */
/* Test 1: version_check                                                       */
/* -------------------------------------------------------------------------- */

static void test_version_check(void) {
    printf("  version_check ... ");
    ASSERT(apdb_version_check(APDB_VERSION_MAJOR,
                              APDB_VERSION_MINOR,
                              APDB_VERSION_PATCH) == 0);
    /* Wrong major must fail */
    ASSERT(apdb_version_check(APDB_VERSION_MAJOR + 1, 0, 0) != 0);
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: single-op put / get / del                                           */
/* -------------------------------------------------------------------------- */

static void test_single_op(void) {
    printf("  single-op put/get/del ... ");
    APDB_Env env = open_env();
    if (!env) return;

    ASSERT_RC(apdb_put(env, "alpha", 5, "one", 3));
    ASSERT_RC(apdb_put(env, "beta",  4, "two", 3));

    const void* data = NULL; size_t sz = 0;
    ASSERT_RC(apdb_get(env, "alpha", 5, &data, &sz));
    ASSERT(sz == 3 && memcmp(data, "one", 3) == 0);

    /* Missing key */
    ASSERT_RC_EQ(apdb_get(env, "gamma", 5, &data, &sz), APDB_NOT_FOUND);

    ASSERT_RC(apdb_del(env, "beta", 4));
    ASSERT_RC_EQ(apdb_get(env, "beta", 4, &data, &sz), APDB_NOT_FOUND);

    /* put_nooverwrite */
    ASSERT_RC(apdb_put_nooverwrite(env, "delta", 5, "val", 3));
    ASSERT_RC_EQ(apdb_put_nooverwrite(env, "delta", 5, "other", 5), APDB_KEY_EXISTS);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: explicit read / write transactions                                  */
/* -------------------------------------------------------------------------- */

static void test_transactions(void) {
    printf("  explicit transactions ... ");
    APDB_Env env = open_env();
    if (!env) return;

    /* Write txn */
    APDB_WTxn wtxn = NULL;
    ASSERT_RC(apdb_txn_begin_write(env, &wtxn));
    ASSERT_RC(apdb_wtxn_put(wtxn, "k1", 2, "v1", 2, 1));
    ASSERT_RC(apdb_wtxn_put(wtxn, "k2", 2, "v2", 2, 1));

    /* read-your-own-writes */
    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_wtxn_get(wtxn, "k1", 2, &d, &s));
    ASSERT(s == 2 && memcmp(d, "v1", 2) == 0);

    int exists = 0;
    ASSERT_RC(apdb_wtxn_contains(wtxn, "k2", 2, &exists));
    ASSERT(exists);

    ASSERT_RC(apdb_wtxn_commit(wtxn, APDB_SYNC_DURABLE));

    /* Read txn */
    APDB_RTxn rtxn = NULL;
    ASSERT_RC(apdb_txn_begin_read(env, &rtxn));
    ASSERT_RC(apdb_rtxn_get(rtxn, "k1", 2, &d, &s));
    ASSERT(s == 2 && memcmp(d, "v1", 2) == 0);
    ASSERT_RC(apdb_rtxn_contains(rtxn, "k2", 2, &exists));
    ASSERT(exists);
    ASSERT(apdb_rtxn_id(rtxn) > 0);
    ASSERT_RC(apdb_rtxn_close(rtxn));

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: read cursor — forward iteration                                     */
/* -------------------------------------------------------------------------- */

static void test_read_cursor(void) {
    printf("  read cursor (forward iteration) ... ");
    APDB_Env env = open_env();
    if (!env) return;

    /* Seed: keys "a".."e" with values "va".."ve" */
    static const char* const keys[]   = {"a","b","c","d","e"};
    static const char* const values[] = {"va","vb","vc","vd","ve"};
    for (int i = 0; i < 5; ++i)
        ASSERT_RC(apdb_put(env, keys[i], strlen(keys[i]),
                           values[i], strlen(values[i])));

    APDB_RTxn rtxn = NULL;
    ASSERT_RC(apdb_txn_begin_read(env, &rtxn));

    APDB_Cursor cur = NULL;
    ASSERT_RC(apdb_rtxn_cursor_open(rtxn, &cur));

    ASSERT_RC(apdb_cursor_first(cur));
    int count = 0;
    while (apdb_cursor_valid(cur)) {
        const void* k = NULL; size_t kl = 0;
        const void* v = NULL; size_t vl = 0;
        ASSERT_RC(apdb_cursor_key(cur, &k, &kl));
        ASSERT_RC(apdb_cursor_value(cur, &v, &vl));
        ASSERT(kl == 1 && vl == 2);
        /* keys sorted: a,b,c,d,e */
        ASSERT(*(char*)k == 'a' + count);
        count++;
        APDB_rc rc = apdb_cursor_next(cur);
        if (rc == APDB_NOT_FOUND) break;
        ASSERT(rc == APDB_OK);
    }
    ASSERT(count == 5);

    /* seek */
    ASSERT_RC(apdb_cursor_seek(cur, "c", 1));
    const void* sk = NULL; size_t skl = 0;
    ASSERT_RC(apdb_cursor_key(cur, &sk, &skl));
    ASSERT(skl == 1 && *(char*)sk == 'c');

    ASSERT_RC(apdb_cursor_close(cur));
    ASSERT_RC(apdb_rtxn_close(rtxn));
    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: write cursor — put and del                                          */
/* -------------------------------------------------------------------------- */

static void test_write_cursor(void) {
    printf("  write cursor (put/del) ... ");
    APDB_Env env = open_env();
    if (!env) return;

    APDB_WTxn wtxn = NULL;
    ASSERT_RC(apdb_txn_begin_write(env, &wtxn));

    APDB_Cursor cur = NULL;
    ASSERT_RC(apdb_wtxn_cursor_open(wtxn, &cur));

    /* Append-ordered puts via cursor */
    ASSERT_RC(apdb_cursor_put(cur, "x1", 2, "X", 1, APDB_CURSOR_APPEND));
    ASSERT_RC(apdb_cursor_put(cur, "x2", 2, "Y", 1, APDB_CURSOR_APPEND));
    ASSERT_RC(apdb_cursor_put(cur, "x3", 2, "Z", 1, APDB_CURSOR_APPEND));

    /* seek_exact then del */
    ASSERT_RC(apdb_cursor_seek_exact(cur, "x2", 2));
    ASSERT_RC(apdb_cursor_del(cur));

    ASSERT_RC(apdb_cursor_close(cur));
    ASSERT_RC(apdb_wtxn_commit(wtxn, APDB_SYNC_DURABLE));

    /* Verify: x2 gone, x1/x3 present */
    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_get(env, "x1", 2, &d, &s));
    ASSERT_RC_EQ(apdb_get(env, "x2", 2, &d, &s), APDB_NOT_FOUND);
    ASSERT_RC(apdb_get(env, "x3", 2, &d, &s));

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 6: sub-databases                                                       */
/* -------------------------------------------------------------------------- */

static void test_sub_databases(void) {
    printf("  sub-databases ... ");
#ifndef ANNPOCKETDB_WITH_SUBDATABASE
    printf("SKIP (not built)\n");
    return;
#else
    APDB_Env env = open_env();
    if (!env) return;

    /* Create */
    APDB_SubDb sdb = NULL;
    ASSERT_RC(apdb_subdb_create(env, "mydb", APDB_DB_NONE, &sdb));

    ASSERT_RC(apdb_subdb_put(sdb, "sk1", 3, "sv1", 3, 1, APDB_SYNC_DURABLE));
    ASSERT_RC(apdb_subdb_put(sdb, "sk2", 3, "sv2", 3, 1, APDB_SYNC_DURABLE));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_subdb_get(sdb, "sk1", 3, &d, &s));
    ASSERT(s == 3 && memcmp(d, "sv1", 3) == 0);

    /* List */
    char** names = NULL; size_t count = 0;
    ASSERT_RC(apdb_subdb_list(env, &names, &count));
    ASSERT(count >= 1);
    int found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(names[i], "mydb") == 0) found = 1;
        free(names[i]);
    }
    free(names);
    ASSERT(found);

    apdb_subdb_close(sdb);

    /* Re-open */
    APDB_SubDb sdb2 = NULL;
    ASSERT_RC(apdb_subdb_open(env, "mydb", &sdb2));
    ASSERT_RC(apdb_subdb_get(sdb2, "sk2", 3, &d, &s));
    ASSERT(s == 3 && memcmp(d, "sv2", 3) == 0);
    apdb_subdb_close(sdb2);

    /* Drop */
    ASSERT_RC(apdb_subdb_drop(env, "mydb", APDB_SYNC_DURABLE));
    ASSERT_RC_EQ(apdb_subdb_open(env, "mydb", &sdb2), APDB_NOT_FOUND);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
#endif
}

/* -------------------------------------------------------------------------- */
/* Test 7: apdb_bytes_copy / free (durable copy)                              */
/* -------------------------------------------------------------------------- */

static void test_bytes_api(void) {
    printf("  bytes copy/free ... ");
    APDB_Env env = open_env();
    if (!env) return;

    ASSERT_RC(apdb_put(env, "bk", 2, "bv_data", 7));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_get(env, "bk", 2, &d, &s));

    /* Copy before the pointer goes stale */
    APDB_Bytes b = {NULL, 0};
    ASSERT_RC(apdb_bytes_copy(d, s, &b));
    ASSERT(b.size == 7 && memcmp(b.data, "bv_data", 7) == 0);

    /* Now overwrite scratch — the copy is unaffected */
    ASSERT_RC(apdb_put(env, "bk", 2, "XXXXXXX", 7));
    ASSERT_RC(apdb_get(env, "bk", 2, &d, &s));
    ASSERT(b.size == 7 && memcmp(b.data, "bv_data", 7) == 0);

    apdb_bytes_free(&b);
    ASSERT(b.data == NULL && b.size == 0);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 8: stat / info                                                         */
/* -------------------------------------------------------------------------- */

static void test_stat_info(void) {
    printf("  stat / info ... ");
    APDB_Env env = open_env();
    if (!env) return;

    ASSERT_RC(apdb_put(env, "s1", 2, "val", 3));

    APDB_Stat st; memset(&st, 0, sizeof(st));
    ASSERT_RC(apdb_env_stat(env, &st));
    ASSERT(st.page_size > 0);
    ASSERT(st.entries > 0);

    APDB_EnvInfo inf; memset(&inf, 0, sizeof(inf));
    apdb_env_info(env, &inf);
    ASSERT(inf.map_size > 0);
    ASSERT(inf.max_readers > 0);

    ASSERT(apdb_env_max_key_size(env) > 0);
    ASSERT(apdb_env_map_size(env) > 0);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 9: rtxn reset / renew                                                 */
/* -------------------------------------------------------------------------- */

static void test_reset_renew(void) {
    printf("  rtxn reset/renew ... ");
    APDB_Env env = open_env();
    if (!env) return;

    ASSERT_RC(apdb_put(env, "rr", 2, "data", 4));

    APDB_RTxn rtxn = NULL;
    ASSERT_RC(apdb_txn_begin_read(env, &rtxn));
    ASSERT_RC(apdb_rtxn_reset(rtxn));
    ASSERT_RC(apdb_rtxn_renew(rtxn));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_rtxn_get(rtxn, "rr", 2, &d, &s));
    ASSERT(s == 4 && memcmp(d, "data", 4) == 0);

    ASSERT_RC(apdb_rtxn_close(rtxn));
    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 10: wtxn abort (nothing published)                                    */
/* -------------------------------------------------------------------------- */

static void test_abort(void) {
    printf("  wtxn abort ... ");
    APDB_Env env = open_env();
    if (!env) return;

    APDB_WTxn wtxn = NULL;
    ASSERT_RC(apdb_txn_begin_write(env, &wtxn));
    ASSERT_RC(apdb_wtxn_put(wtxn, "aborted_key", 11, "v", 1, 1));
    ASSERT_RC(apdb_wtxn_abort(wtxn));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC_EQ(apdb_get(env, "aborted_key", 11, &d, &s), APDB_NOT_FOUND);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 11: 2PC                                                                */
/* -------------------------------------------------------------------------- */

static void test_2pc(void) {
    printf("  2pc ... ");
    APDB_Env env = open_env();
    if (!env) return;

    APDB_WTxn wtxn = NULL;
    ASSERT_RC(apdb_txn_begin_write(env, &wtxn));
    ASSERT_RC(apdb_wtxn_put(wtxn, "2pc_key", 7, "2pc_val", 7, 1));
    ASSERT_RC(apdb_wtxn_prepare(wtxn, APDB_SYNC_DURABLE));
    ASSERT_RC(apdb_wtxn_commit(wtxn, APDB_SYNC_DURABLE));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_get(env, "2pc_key", 7, &d, &s));
    ASSERT(s == 7 && memcmp(d, "2pc_val", 7) == 0);

    /* Test rollback_prepared */
    ASSERT_RC_EQ(apdb_env_rollback_prepared(env), APDB_NOT_FOUND); /* none pending */

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 12: blob storage                                                       */
/* -------------------------------------------------------------------------- */

static void test_blob(void) {
    printf("  blob storage ... ");
    APDB_Env env = open_env();
    if (!env) return;

    const char* big_data = "this_is_a_mock_blob_payload";
    size_t big_len = strlen(big_data);
    ASSERT_RC(apdb_env_put_blob(env, "blob_key", 8, big_data, big_len, APDB_SYNC_DURABLE));

    const void* d = NULL; size_t s = 0;
    ASSERT_RC(apdb_env_get_blob(env, "blob_key", 8, &d, &s));
    ASSERT(s == big_len && memcmp(d, big_data, big_len) == 0);

    ASSERT_RC(apdb_env_del_blob(env, "blob_key", 8, APDB_SYNC_DURABLE));
    ASSERT_RC_EQ(apdb_env_get_blob(env, "blob_key", 8, &d, &s), APDB_NOT_FOUND);

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* Test 13: changefeed                                                         */
/* -------------------------------------------------------------------------- */

static void test_changefeed(void) {
    printf("  changefeed ... ");
    APDB_EnvOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.flags = APDB_ENV_CHANGEFEED;
    
    APDB_Env env = NULL;
    ASSERT_RC(apdb_env_open(kDbPath, &opts, &env));

    APDB_WTxn wtxn = NULL;
    ASSERT_RC(apdb_txn_begin_write(env, &wtxn));
    ASSERT_RC(apdb_wtxn_put(wtxn, "cfk", 3, "cfv", 3, 1));
    ASSERT_RC(apdb_wtxn_commit(wtxn, APDB_SYNC_DURABLE));

    APDB_RTxn rtxn = NULL;
    ASSERT_RC(apdb_txn_begin_read(env, &rtxn));
    
    APDB_ChangeFeedIterator iter = NULL;
    ASSERT_RC(apdb_changefeed_open_poll(env, rtxn, 0, &iter));
    
    APDB_ChangeEvent event;
    ASSERT_RC(apdb_changefeed_next(iter, &event));
    ASSERT(event.type == 1); /* Insert */
    ASSERT(event.key_len == 3 && memcmp(event.key, "cfk", 3) == 0);
    ASSERT(event.new_value_len == 3 && memcmp(event.new_value, "cfv", 3) == 0);
    
    ASSERT_RC_EQ(apdb_changefeed_next(iter, &event), APDB_NOT_FOUND);
    ASSERT_RC(apdb_changefeed_close_poll(iter));
    ASSERT_RC(apdb_rtxn_close(rtxn));

    ASSERT_RC(apdb_env_close(env));
    printf("OK\n");
}

/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(void) {
    printf("AnnPocketDb C API smoke test\n");

    /* Clean up from any previous interrupted run */
    rm_rf(kDbPath);

    test_version_check();
    test_single_op();      rm_rf(kDbPath);
    test_transactions();   rm_rf(kDbPath);
    test_read_cursor();    rm_rf(kDbPath);
    test_write_cursor();   rm_rf(kDbPath);
    test_sub_databases();  rm_rf(kDbPath);
    test_bytes_api();      rm_rf(kDbPath);
    test_stat_info();      rm_rf(kDbPath);
    test_reset_renew();    rm_rf(kDbPath);
    test_abort();          rm_rf(kDbPath);
    test_2pc();            rm_rf(kDbPath);
    test_blob();           rm_rf(kDbPath);
    test_changefeed();     rm_rf(kDbPath);

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
