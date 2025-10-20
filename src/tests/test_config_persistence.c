#include "../fsearch_config.h"
#include "../fsearch_database_exclude.h"
#include "../fsearch_database_exclude_manager.h"
#include "../fsearch_database_include.h"
#include "../fsearch_database_include_manager.h"
#include "../fsearch_exclude_path.h"
#include "../fsearch_index.h"
#include <glib.h>
#include <glib/gstdio.h>

static gchar *test_config_dir = NULL;
static gchar *test_config_file = NULL;

static void
setup_test_config(void) {
    test_config_dir = g_build_filename(g_get_tmp_dir(), "fsearch_test_XXXXXX", NULL);
    test_config_dir = g_mkdtemp(test_config_dir);
    g_assert_nonnull(test_config_dir);

    test_config_file = g_build_filename(test_config_dir, "fsearch.conf", NULL);
}

static void
teardown_test_config(void) {
    if (test_config_file) {
        g_unlink(test_config_file);
        g_free(test_config_file);
        test_config_file = NULL;
    }
    if (test_config_dir) {
        g_rmdir(test_config_dir);
        g_free(test_config_dir);
        test_config_dir = NULL;
    }
}

static void
save_config_to_test_file(FsearchConfig *config) {
    g_autoptr(GKeyFile) key_file = g_key_file_new();

    g_key_file_set_boolean(key_file, "Database", "update_database_on_launch", config->update_database_on_launch);

    uint32_t pos = 1;
    for (GList *l = config->indexes; l != NULL; l = l->next) {
        FsearchIndex *index = l->data;
        if (!index) {
            continue;
        }

        char key[100] = "";
        snprintf(key, sizeof(key), "location_%d", pos);
        g_key_file_set_string(key_file, "Database", key, index->path);

        snprintf(key, sizeof(key), "location_enabled_%d", pos);
        g_key_file_set_boolean(key_file, "Database", key, index->enabled);

        snprintf(key, sizeof(key), "location_update_%d", pos);
        g_key_file_set_boolean(key_file, "Database", key, index->update);

        snprintf(key, sizeof(key), "location_one_filesystem_%d", pos);
        g_key_file_set_boolean(key_file, "Database", key, index->one_filesystem);

        snprintf(key, sizeof(key), "location_monitor_%d", pos);
        g_key_file_set_boolean(key_file, "Database", key, index->monitor);

        pos++;
    }

    pos = 1;
    for (GList *l = config->exclude_locations; l != NULL; l = l->next) {
        FsearchExcludePath *exclude = l->data;
        if (!exclude) {
            continue;
        }

        char key[100] = "";
        snprintf(key, sizeof(key), "exclude_location_%d", pos);
        g_key_file_set_string(key_file, "Database", key, exclude->path);

        snprintf(key, sizeof(key), "exclude_location_enabled_%d", pos);
        g_key_file_set_boolean(key_file, "Database", key, exclude->enabled);

        pos++;
    }

    g_autoptr(GError) error = NULL;
    gboolean success = g_key_file_save_to_file(key_file, test_config_file, &error);
    g_assert_true(success);
    g_assert_no_error(error);
}

static FsearchConfig *
load_config_from_test_file(void) {
    FsearchConfig *config = g_new0(FsearchConfig, 1);

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_autoptr(GError) error = NULL;

    gboolean success = g_key_file_load_from_file(key_file, test_config_file, G_KEY_FILE_NONE, &error);
    g_assert_true(success);
    g_assert_no_error(error);

    config->update_database_on_launch =
        g_key_file_get_boolean(key_file, "Database", "update_database_on_launch", NULL);

    uint32_t pos = 1;
    while (TRUE) {
        char key[100] = "";
        snprintf(key, sizeof(key), "location_%d", pos);
        g_autofree gchar *path = g_key_file_get_string(key_file, "Database", key, NULL);
        if (!path) {
            break;
        }

        snprintf(key, sizeof(key), "location_enabled_%d", pos);
        gboolean enabled = g_key_file_get_boolean(key_file, "Database", key, NULL);

        snprintf(key, sizeof(key), "location_update_%d", pos);
        gboolean update = g_key_file_get_boolean(key_file, "Database", key, NULL);

        snprintf(key, sizeof(key), "location_one_filesystem_%d", pos);
        gboolean one_filesystem = g_key_file_get_boolean(key_file, "Database", key, NULL);

        snprintf(key, sizeof(key), "location_monitor_%d", pos);
        gboolean monitor = g_key_file_get_boolean(key_file, "Database", key, NULL);

        FsearchIndex *index = fsearch_index_new(FSEARCH_INDEX_FOLDER_TYPE, path, enabled, update, one_filesystem, monitor, pos - 1, 0);
        config->indexes = g_list_append(config->indexes, index);

        pos++;
    }

    pos = 1;
    while (TRUE) {
        char key[100] = "";
        snprintf(key, sizeof(key), "exclude_location_%d", pos);
        g_autofree gchar *path = g_key_file_get_string(key_file, "Database", key, NULL);
        if (!path) {
            break;
        }

        snprintf(key, sizeof(key), "exclude_location_enabled_%d", pos);
        gboolean enabled = g_key_file_get_boolean(key_file, "Database", key, NULL);

        FsearchExcludePath *exclude = fsearch_exclude_path_new(path, enabled);
        config->exclude_locations = g_list_append(config->exclude_locations, exclude);

        pos++;
    }

    return config;
}

static void
test_config_indexes_to_include_manager_conversion(void) {
    setup_test_config();

    FsearchConfig *config = g_new0(FsearchConfig, 1);
    config->update_database_on_launch = TRUE;

    FsearchIndex *idx1 = fsearch_index_new(FSEARCH_INDEX_FOLDER_TYPE, "/home/user1", TRUE, TRUE, FALSE, TRUE, 0, 0);
    FsearchIndex *idx2 = fsearch_index_new(FSEARCH_INDEX_FOLDER_TYPE, "/home/user2", TRUE, TRUE, TRUE, FALSE, 1, 0);
    config->indexes = g_list_append(config->indexes, idx1);
    config->indexes = g_list_append(config->indexes, idx2);

    FsearchExcludePath *exc1 = fsearch_exclude_path_new("/proc", TRUE);
    FsearchExcludePath *exc2 = fsearch_exclude_path_new("/sys", FALSE);
    config->exclude_locations = g_list_append(config->exclude_locations, exc1);
    config->exclude_locations = g_list_append(config->exclude_locations, exc2);

    save_config_to_test_file(config);

    FsearchConfig *loaded_config = load_config_from_test_file();
    g_assert_nonnull(loaded_config);
    g_assert_true(loaded_config->update_database_on_launch);

    g_assert_nonnull(loaded_config->indexes);
    g_assert_cmpuint(g_list_length(loaded_config->indexes), ==, 2);

    FsearchIndex *loaded_idx1 = g_list_nth_data(loaded_config->indexes, 0);
    g_assert_nonnull(loaded_idx1);
    g_assert_cmpstr(loaded_idx1->path, ==, "/home/user1");
    g_assert_true(loaded_idx1->enabled);
    g_assert_true(loaded_idx1->update);
    g_assert_false(loaded_idx1->one_filesystem);
    g_assert_true(loaded_idx1->monitor);

    FsearchIndex *loaded_idx2 = g_list_nth_data(loaded_config->indexes, 1);
    g_assert_nonnull(loaded_idx2);
    g_assert_cmpstr(loaded_idx2->path, ==, "/home/user2");
    g_assert_true(loaded_idx2->enabled);
    g_assert_true(loaded_idx2->update);
    g_assert_true(loaded_idx2->one_filesystem);
    g_assert_false(loaded_idx2->monitor);

    g_assert_nonnull(loaded_config->exclude_locations);
    g_assert_cmpuint(g_list_length(loaded_config->exclude_locations), ==, 2);

    FsearchExcludePath *loaded_exc1 = g_list_nth_data(loaded_config->exclude_locations, 0);
    g_assert_nonnull(loaded_exc1);
    g_assert_cmpstr(loaded_exc1->path, ==, "/proc");
    g_assert_true(loaded_exc1->enabled);

    FsearchExcludePath *loaded_exc2 = g_list_nth_data(loaded_config->exclude_locations, 1);
    g_assert_nonnull(loaded_exc2);
    g_assert_cmpstr(loaded_exc2->path, ==, "/sys");
    g_assert_false(loaded_exc2->enabled);

    if (config->indexes) {
        g_list_free_full(config->indexes, (GDestroyNotify)fsearch_index_free);
    }
    if (config->exclude_locations) {
        g_list_free_full(config->exclude_locations, (GDestroyNotify)fsearch_exclude_path_free);
    }
    g_free(config);

    if (loaded_config->indexes) {
        g_list_free_full(loaded_config->indexes, (GDestroyNotify)fsearch_index_free);
    }
    if (loaded_config->exclude_locations) {
        g_list_free_full(loaded_config->exclude_locations, (GDestroyNotify)fsearch_exclude_path_free);
    }
    g_free(loaded_config);

    teardown_test_config();
}

static void
test_include_manager_to_config_indexes_conversion(void) {
    g_autoptr(FsearchDatabaseIncludeManager) include_manager = fsearch_database_include_manager_new();

    g_autoptr(FsearchDatabaseInclude) inc1 =
        fsearch_database_include_new("/home/user1", TRUE, FALSE, TRUE, FALSE, 0);
    g_autoptr(FsearchDatabaseInclude) inc2 =
        fsearch_database_include_new("/home/user2", TRUE, TRUE, FALSE, FALSE, 1);
    fsearch_database_include_manager_add(include_manager, inc1);
    fsearch_database_include_manager_add(include_manager, inc2);

    g_autoptr(FsearchDatabaseExcludeManager) exclude_manager = fsearch_database_exclude_manager_new();

    g_autoptr(FsearchDatabaseExclude) exc1 = fsearch_database_exclude_new("/proc", TRUE);
    g_autoptr(FsearchDatabaseExclude) exc2 = fsearch_database_exclude_new("/sys", FALSE);
    fsearch_database_exclude_manager_add(exclude_manager, exc1);
    fsearch_database_exclude_manager_add(exclude_manager, exc2);

    GList *indexes = NULL;
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
        indexes = g_list_append(indexes, index);
    }

    g_assert_nonnull(indexes);
    g_assert_cmpuint(g_list_length(indexes), ==, 2);

    FsearchIndex *idx1 = g_list_nth_data(indexes, 0);
    g_assert_nonnull(idx1);
    g_assert_cmpstr(idx1->path, ==, "/home/user1");
    g_assert_true(idx1->enabled);
    g_assert_false(idx1->one_filesystem);
    g_assert_true(idx1->monitor);
    g_assert_cmpint(idx1->id, ==, 0);

    FsearchIndex *idx2 = g_list_nth_data(indexes, 1);
    g_assert_nonnull(idx2);
    g_assert_cmpstr(idx2->path, ==, "/home/user2");
    g_assert_true(idx2->enabled);
    g_assert_true(idx2->one_filesystem);
    g_assert_false(idx2->monitor);
    g_assert_cmpint(idx2->id, ==, 1);

    GList *exclude_locations = NULL;
    g_autoptr(GPtrArray) excludes = fsearch_database_exclude_manager_get_excludes(exclude_manager);
    for (uint32_t i = 0; i < excludes->len; ++i) {
        FsearchDatabaseExclude *exclude = g_ptr_array_index(excludes, i);
        FsearchExcludePath *exclude_path =
            fsearch_exclude_path_new(fsearch_database_exclude_get_path(exclude),
                                    fsearch_database_exclude_get_active(exclude));
        exclude_locations = g_list_append(exclude_locations, exclude_path);
    }

    g_assert_nonnull(exclude_locations);
    g_assert_cmpuint(g_list_length(exclude_locations), ==, 2);

    FsearchExcludePath *exc_path1 = g_list_nth_data(exclude_locations, 0);
    g_assert_nonnull(exc_path1);
    g_assert_cmpstr(exc_path1->path, ==, "/proc");
    g_assert_true(exc_path1->enabled);

    FsearchExcludePath *exc_path2 = g_list_nth_data(exclude_locations, 1);
    g_assert_nonnull(exc_path2);
    g_assert_cmpstr(exc_path2->path, ==, "/sys");
    g_assert_false(exc_path2->enabled);

    if (indexes) {
        g_list_free_full(indexes, (GDestroyNotify)fsearch_index_free);
    }
    if (exclude_locations) {
        g_list_free_full(exclude_locations, (GDestroyNotify)fsearch_exclude_path_free);
    }
}

static void
test_bidirectional_conversion(void) {
    setup_test_config();

    g_autoptr(FsearchDatabaseIncludeManager) orig_include_manager = fsearch_database_include_manager_new();
    g_autoptr(FsearchDatabaseInclude) inc1 =
        fsearch_database_include_new("/test/path1", TRUE, FALSE, TRUE, FALSE, 42);
    fsearch_database_include_manager_add(orig_include_manager, inc1);

    GList *indexes = NULL;
    g_autoptr(GPtrArray) includes = fsearch_database_include_manager_get_includes(orig_include_manager);
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
        indexes = g_list_append(indexes, index);
    }

    g_autoptr(FsearchDatabaseIncludeManager) new_include_manager = fsearch_database_include_manager_new();
    for (GList *l = indexes; l != NULL; l = l->next) {
        FsearchIndex *index = l->data;
        if (!index) {
            continue;
        }
        FsearchDatabaseInclude *include = fsearch_database_include_new(index->path,
                                                                       index->enabled,
                                                                       index->one_filesystem,
                                                                       index->monitor,
                                                                       FALSE,
                                                                       index->id);
        fsearch_database_include_manager_add(new_include_manager, include);
        g_clear_pointer(&include, fsearch_database_include_unref);
    }

    g_assert_true(fsearch_database_include_manager_equal(orig_include_manager, new_include_manager));

    if (indexes) {
        g_list_free_full(indexes, (GDestroyNotify)fsearch_index_free);
    }

    teardown_test_config();
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/config/indexes_to_include_manager", test_config_indexes_to_include_manager_conversion);
    g_test_add_func("/FSearch/config/include_manager_to_indexes", test_include_manager_to_config_indexes_conversion);
    g_test_add_func("/FSearch/config/bidirectional_conversion", test_bidirectional_conversion);
    return g_test_run();
}
