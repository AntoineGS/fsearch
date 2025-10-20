#include "../fsearch_config.h"
#include "../fsearch_database_exclude.h"
#include "../fsearch_database_exclude_manager.h"
#include "../fsearch_database_include.h"
#include "../fsearch_database_include_manager.h"
#include "../fsearch_exclude_path.h"
#include "../fsearch_index.h"
#include <glib.h>
#include <glib/gstdio.h>

static gchar *original_config_dir = NULL;

static void
setup_test_environment(void) {
    original_config_dir = g_strdup(g_getenv("XDG_CONFIG_HOME"));

    gchar *test_config_dir = g_build_filename(g_get_tmp_dir(), "fsearch_integration_test_XXXXXX", NULL);
    test_config_dir = g_mkdtemp(test_config_dir);
    g_assert_nonnull(test_config_dir);

    g_setenv("XDG_CONFIG_HOME", test_config_dir, TRUE);
    g_free(test_config_dir);
}

static void
teardown_test_environment(void) {
    const gchar *test_config_dir = g_getenv("XDG_CONFIG_HOME");
    if (test_config_dir) {
        gchar *test_config_file = g_build_filename(test_config_dir, "fsearch", "fsearch.conf", NULL);
        g_unlink(test_config_file);
        g_free(test_config_file);

        gchar *fsearch_dir = g_build_filename(test_config_dir, "fsearch", NULL);
        g_rmdir(fsearch_dir);
        g_free(fsearch_dir);

        g_rmdir(test_config_dir);
    }

    if (original_config_dir) {
        g_setenv("XDG_CONFIG_HOME", original_config_dir, TRUE);
        g_free(original_config_dir);
        original_config_dir = NULL;
    }
    else {
        g_unsetenv("XDG_CONFIG_HOME");
    }
}

static void
test_preferences_dialog_save_load_workflow(void) {
    setup_test_environment();

    config_make_dir();

    FsearchConfig *config1 = calloc(1, sizeof(FsearchConfig));
    g_assert_nonnull(config1);

    if (!config_load(config1)) {
        config_load_default(config1);
    }

    g_assert_null(config1->indexes);
    g_assert_nonnull(config1->exclude_locations);

    g_autoptr(FsearchDatabaseIncludeManager) include_manager = fsearch_database_include_manager_new();
    g_autoptr(FsearchDatabaseInclude) inc1 =
        fsearch_database_include_new("/home/testuser", TRUE, FALSE, TRUE, FALSE, 0);
    g_autoptr(FsearchDatabaseInclude) inc2 =
        fsearch_database_include_new("/opt", TRUE, TRUE, FALSE, FALSE, 1);
    fsearch_database_include_manager_add(include_manager, inc1);
    fsearch_database_include_manager_add(include_manager, inc2);

    g_autoptr(FsearchDatabaseExcludeManager) exclude_manager = fsearch_database_exclude_manager_new();
    g_autoptr(FsearchDatabaseExclude) exc1 = fsearch_database_exclude_new("/home/testuser/.cache", TRUE);
    fsearch_database_exclude_manager_add(exclude_manager, exc1);

    if (config1->indexes) {
        g_list_free_full(g_steal_pointer(&config1->indexes), (GDestroyNotify)fsearch_index_free);
    }
    g_autoptr(GPtrArray) includes = fsearch_database_include_manager_get_includes(include_manager);
    for (uint32_t i = 0; i < includes->len; ++i) {
        FsearchDatabaseInclude *include = g_ptr_array_index(includes, i);
        FsearchIndex *index = fsearch_index_new(FSEARCH_INDEX_FOLDER_TYPE,
                                                fsearch_database_include_get_path(include),
                                                fsearch_database_include_get_active(include),
                                                TRUE,
                                                fsearch_database_include_get_one_file_system(include),
                                                fsearch_database_include_get_monitored(include),
                                                fsearch_database_include_get_id(include),
                                                0);
        config1->indexes = g_list_append(config1->indexes, index);
    }

    if (config1->exclude_locations) {
        g_list_free_full(g_steal_pointer(&config1->exclude_locations), (GDestroyNotify)fsearch_exclude_path_free);
    }
    g_autoptr(GPtrArray) excludes = fsearch_database_exclude_manager_get_excludes(exclude_manager);
    for (uint32_t i = 0; i < excludes->len; ++i) {
        FsearchDatabaseExclude *exclude = g_ptr_array_index(excludes, i);
        FsearchExcludePath *exclude_path =
            fsearch_exclude_path_new(fsearch_database_exclude_get_path(exclude),
                                    fsearch_database_exclude_get_active(exclude));
        config1->exclude_locations = g_list_append(config1->exclude_locations, exclude_path);
    }

    gboolean save_result = config_save(config1);
    g_assert_true(save_result);

    FsearchConfig *config2 = calloc(1, sizeof(FsearchConfig));
    g_assert_nonnull(config2);

    gboolean load_result = config_load(config2);
    g_assert_true(load_result);

    g_assert_nonnull(config2->indexes);
    g_assert_cmpuint(g_list_length(config2->indexes), ==, 2);

    FsearchIndex *idx1 = g_list_nth_data(config2->indexes, 0);
    g_assert_nonnull(idx1);
    g_assert_cmpstr(idx1->path, ==, "/home/testuser");
    g_assert_true(idx1->enabled);
    g_assert_false(idx1->one_filesystem);
    g_assert_true(idx1->monitor);

    FsearchIndex *idx2 = g_list_nth_data(config2->indexes, 1);
    g_assert_nonnull(idx2);
    g_assert_cmpstr(idx2->path, ==, "/opt");
    g_assert_true(idx2->enabled);
    g_assert_true(idx2->one_filesystem);
    g_assert_false(idx2->monitor);

    g_assert_nonnull(config2->exclude_locations);
    g_assert_cmpuint(g_list_length(config2->exclude_locations), ==, 1);

    FsearchExcludePath *exc = g_list_nth_data(config2->exclude_locations, 0);
    g_assert_nonnull(exc);
    g_assert_cmpstr(exc->path, ==, "/home/testuser/.cache");
    g_assert_true(exc->enabled);

    config_free(config1);
    config_free(config2);

    teardown_test_environment();
}

static void
test_empty_indexes_save_load(void) {
    setup_test_environment();

    config_make_dir();

    FsearchConfig *config1 = calloc(1, sizeof(FsearchConfig));
    g_assert_nonnull(config1);
    config_load_default(config1);

    config1->indexes = NULL;

    gboolean save_result = config_save(config1);
    g_assert_true(save_result);

    FsearchConfig *config2 = calloc(1, sizeof(FsearchConfig));
    g_assert_nonnull(config2);

    gboolean load_result = config_load(config2);
    g_assert_true(load_result);

    g_assert_null(config2->indexes);

    config_free(config1);
    config_free(config2);

    teardown_test_environment();
}

static void
test_multiple_save_load_cycles(void) {
    setup_test_environment();

    config_make_dir();

    for (int cycle = 0; cycle < 3; cycle++) {
        FsearchConfig *config = calloc(1, sizeof(FsearchConfig));
        g_assert_nonnull(config);

        if (cycle == 0) {
            config_load_default(config);
        }
        else {
            gboolean load_result = config_load(config);
            g_assert_true(load_result);
        }

        if (config->indexes) {
            g_list_free_full(g_steal_pointer(&config->indexes), (GDestroyNotify)fsearch_index_free);
        }

        FsearchIndex *idx = fsearch_index_new(FSEARCH_INDEX_FOLDER_TYPE,
                                              g_strdup_printf("/test/path%d", cycle),
                                              TRUE,
                                              TRUE,
                                              FALSE,
                                              FALSE,
                                              cycle,
                                              0);
        config->indexes = g_list_append(config->indexes, idx);

        gboolean save_result = config_save(config);
        g_assert_true(save_result);

        config_free(config);
    }

    FsearchConfig *final_config = calloc(1, sizeof(FsearchConfig));
    gboolean load_result = config_load(final_config);
    g_assert_true(load_result);

    g_assert_nonnull(final_config->indexes);
    g_assert_cmpuint(g_list_length(final_config->indexes), ==, 1);

    FsearchIndex *idx = g_list_nth_data(final_config->indexes, 0);
    g_assert_nonnull(idx);
    g_assert_cmpstr(idx->path, ==, "/test/path2");

    config_free(final_config);

    teardown_test_environment();
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/config/preferences_dialog_workflow", test_preferences_dialog_save_load_workflow);
    g_test_add_func("/FSearch/config/empty_indexes_save_load", test_empty_indexes_save_load);
    g_test_add_func("/FSearch/config/multiple_save_load_cycles", test_multiple_save_load_cycles);
    return g_test_run();
}
