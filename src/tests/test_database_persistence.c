#include "../fsearch_database.h"
#include "../fsearch_database_exclude_manager.h"
#include "../fsearch_database_include.h"
#include "../fsearch_database_include_manager.h"
#include "../fsearch_database_work.h"
#include <glib.h>
#include <glib/gstdio.h>

static gchar *test_db_file = NULL;

static void
setup_test(void) {
    gchar *test_dir = g_build_filename(g_get_tmp_dir(), "fsearch_db_test_XXXXXX", NULL);
    test_dir = g_mkdtemp(test_dir);
    g_assert_nonnull(test_dir);

    test_db_file = g_build_filename(test_dir, "test.db", NULL);
    g_free(test_dir);
}

static void
teardown_test(void) {
    if (test_db_file) {
        g_unlink(test_db_file);
        gchar *test_dir = g_path_get_dirname(test_db_file);
        g_rmdir(test_dir);
        g_free(test_dir);
        g_free(test_db_file);
        test_db_file = NULL;
    }
}

static void
on_scan_started_counter(FsearchDatabase *db, gpointer user_data) {
    gint *count = user_data;
    (*count)++;
    g_debug("Scan started, count: %d", *count);
}

static void
on_scan_finished_quit_loop(FsearchDatabase *db, gpointer arg1, gpointer user_data) {
    GMainLoop *loop = user_data;
    g_debug("Scan finished, quitting loop");
    g_main_loop_quit(loop);
}

static void
on_load_started_counter(FsearchDatabase *db, gpointer user_data) {
    gint *count = user_data;
    (*count)++;
    g_debug("Load started, count: %d", *count);
}

static void
test_database_configuration_persists(void) {
    setup_test();

    g_autoptr(GFile) db_file = g_file_new_for_path(test_db_file);

    gint scan_count1 = 0;
    g_autoptr(GMainLoop) loop1 = g_main_loop_new(NULL, FALSE);
    g_autoptr(FsearchDatabase) db1 = fsearch_database_new(db_file);
    g_signal_connect(db1, "scan-started", G_CALLBACK(on_scan_started_counter), &scan_count1);
    g_signal_connect(db1, "scan-finished", G_CALLBACK(on_scan_finished_quit_loop), loop1);

    g_autoptr(FsearchDatabaseIncludeManager) include_manager = fsearch_database_include_manager_new();
    g_autoptr(FsearchDatabaseInclude) inc1 = fsearch_database_include_new("/tmp", TRUE, FALSE, FALSE, FALSE, 0);
    fsearch_database_include_manager_add(include_manager, inc1);

    g_autoptr(FsearchDatabaseExcludeManager) exclude_manager = fsearch_database_exclude_manager_new();

    g_autoptr(FsearchDatabaseWork) work_scan =
        fsearch_database_work_new_scan(include_manager,
                                       exclude_manager,
                                       DATABASE_INDEX_PROPERTY_FLAG_NAME | DATABASE_INDEX_PROPERTY_FLAG_PATH);
    fsearch_database_queue_work(db1, work_scan);

    g_timeout_add_seconds(10, (GSourceFunc)g_main_loop_quit, loop1);
    g_main_loop_run(loop1);

    g_assert_cmpint(scan_count1, >=, 1);

    g_autoptr(FsearchDatabaseWork) work_save = fsearch_database_work_new_save();
    fsearch_database_queue_work(db1, work_save);

    g_usleep(200000);

    g_clear_object(&db1);

    gint scan_count2 = 0;
    g_autoptr(FsearchDatabase) db2 = fsearch_database_new(db_file);
    g_signal_connect(db2, "scan-started", G_CALLBACK(on_scan_started_counter), &scan_count2);

    g_usleep(500000);

    g_assert_cmpint(scan_count2, ==, 0);

    g_autoptr(FsearchDatabaseIncludeManager) loaded_include_manager = fsearch_database_get_include_manager(db2);
    g_assert_nonnull(loaded_include_manager);

    g_autoptr(GPtrArray) loaded_includes = fsearch_database_include_manager_get_includes(loaded_include_manager);
    g_assert_cmpuint(loaded_includes->len, ==, 1);

    FsearchDatabaseInclude *loaded_inc = g_ptr_array_index(loaded_includes, 0);
    g_assert_cmpstr(fsearch_database_include_get_path(loaded_inc), ==, "/tmp");
    g_assert_true(fsearch_database_include_get_active(loaded_inc));

    teardown_test();
}

static void
test_database_no_rescan_after_load(void) {
    setup_test();

    g_autoptr(GFile) db_file = g_file_new_for_path(test_db_file);

    test_database_configuration_persists();

    gint scan_count = 0;
    g_autoptr(FsearchDatabase) db2 = fsearch_database_new(db_file);
    g_signal_connect(db2, "scan-started", G_CALLBACK(on_scan_started_counter), &scan_count);

    g_usleep(500000);

    g_assert_cmpint(scan_count, ==, 0);

    teardown_test();
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/database/configuration_persists", test_database_configuration_persists);
    return g_test_run();
}
