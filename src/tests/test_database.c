#include <glib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <src/fsearch_array.h>
#include <src/fsearch_database.h>
#include <src/fsearch_database_entries_container.h>
#include <src/fsearch_database_entry.h>
#include <src/fsearch_database_exclude_manager.h>
#include <src/fsearch_database_include.h>
#include <src/fsearch_database_include_manager.h>
#include <src/fsearch_database_index.h>
#include <src/fsearch_database_index_properties.h>

// Forward declarations - these types and structures are internal to fsearch_database.c
// We need to declare them here to work with the internal functions

typedef struct {
    GThread *thread;
    GMainLoop *loop;
    GMainContext *ctx;
} FsearchDatabaseThreadContext;

typedef struct {
    // Array of FsearchDatabaseIndex's
    GPtrArray *indices;

    // Hash table to all search results
    GHashTable *search_results;

    // Sorted "lists" of all entries in `indices`
    void *file_container[NUM_DATABASE_INDEX_PROPERTIES];
    void *folder_container[NUM_DATABASE_INDEX_PROPERTIES];

    // Include/Exclude configuration
    FsearchDatabaseIncludeManager *include_manager;
    FsearchDatabaseExcludeManager *exclude_manager;

    // Gets called on every FsearchDatabaseIndex event
    void *event_func;
    void *event_func_data;

    // Stores which properties have been indexed
    uint64_t flags;

    // Shared thread where all indices can listen for file system change events and queue them for being processed later
    FsearchDatabaseThreadContext monitor;
    // Shared thread where all indices can process file system change events
    FsearchDatabaseThreadContext worker;

    bool is_sorted;
    bool running;

    volatile int ref_count;
} FsearchDatabaseIndexStore;

// External functions we need to test (they're not in the header but are used internally)
extern bool
database_file_save(FsearchDatabaseIndexStore *store, const char *file_path);
extern bool
database_file_load(const char *file_path,
                   void (*status_cb)(const char *),
                   FsearchDatabaseIndexStore **store_out,
                   FsearchDatabaseIncludeManager **include_manager_out,
                   FsearchDatabaseExcludeManager **exclude_manager_out);

// Helper function to create a minimal FsearchDatabaseIndexStore for testing
static FsearchDatabaseIndexStore *
create_test_store(void) {
    FsearchDatabaseIndexStore *store = g_slice_new0(FsearchDatabaseIndexStore);

    store->indices = g_ptr_array_new_with_free_func((GDestroyNotify)fsearch_database_index_unref);
    store->search_results = g_hash_table_new(g_direct_hash, g_direct_equal);
    store->include_manager = fsearch_database_include_manager_new();
    store->exclude_manager = fsearch_database_exclude_manager_new();

    // Set basic flags - name and size are common properties
    store->flags = DATABASE_INDEX_PROPERTY_FLAG_NAME | DATABASE_INDEX_PROPERTY_FLAG_SIZE
                 | DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME;

    store->is_sorted = true;
    store->running = false;
    store->ref_count = 1;

    // Initialize contexts to NULL (we don't need threads for save/load tests)
    store->monitor.thread = NULL;
    store->monitor.loop = NULL;
    store->monitor.ctx = NULL;
    store->worker.thread = NULL;
    store->worker.loop = NULL;
    store->worker.ctx = NULL;

    // Initialize all container pointers to NULL
    for (int i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; i++) {
        store->file_container[i] = NULL;
        store->folder_container[i] = NULL;
    }

    // Create empty containers for NAME property (required for save/load)
    DynamicArray *empty_folders = darray_new(0);
    DynamicArray *empty_files = darray_new(0);
    store->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(empty_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               NULL);
    store->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(empty_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               NULL);
    darray_unref(empty_folders);
    darray_unref(empty_files);

    return store;
}

// Helper function to free the test store
static void
free_test_store(FsearchDatabaseIndexStore *store) {
    if (!store) {
        return;
    }

    // Free containers
    for (int i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; i++) {
        if (store->file_container[i]) {
            fsearch_database_entries_container_unref(store->file_container[i]);
            store->file_container[i] = NULL;
        }
        if (store->folder_container[i]) {
            fsearch_database_entries_container_unref(store->folder_container[i]);
            store->folder_container[i] = NULL;
        }
    }

    g_clear_pointer(&store->search_results, g_hash_table_unref);
    g_clear_pointer(&store->indices, g_ptr_array_unref);
    g_clear_object(&store->include_manager);
    g_clear_object(&store->exclude_manager);

    g_slice_free(FsearchDatabaseIndexStore, store);
}

// Helper function to create test entries and populate store
static void
populate_test_store_with_entries(FsearchDatabaseIndexStore *store) {
    // Create a simple index with some test data
    const uint64_t flags = store->flags;

    // Create root folder
    FsearchDatabaseEntry *root = db_entry_new(flags, "/", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_size(root, 0);
    db_entry_set_mtime(root, 1234567890);
    db_entry_set_index(root, 0);

    // Create a subfolder
    FsearchDatabaseEntry *home = db_entry_new(flags, "home", root, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_size(home, 4096);
    db_entry_set_mtime(home, 1234567900);
    db_entry_set_index(home, 1);

    // Create a file
    FsearchDatabaseEntry *file1 = db_entry_new(flags, "test.txt", home, DATABASE_ENTRY_TYPE_FILE);
    db_entry_set_size(file1, 1024);
    db_entry_set_mtime(file1, 1234567910);
    db_entry_set_index(file1, 0);

    // Create another file
    FsearchDatabaseEntry *file2 = db_entry_new(flags, "readme.md", home, DATABASE_ENTRY_TYPE_FILE);
    db_entry_set_size(file2, 2048);
    db_entry_set_mtime(file2, 1234567920);
    db_entry_set_index(file2, 1);

    // Create arrays for folders and files
    DynamicArray *folders = darray_new(2);
    darray_add_item(folders, root);
    darray_add_item(folders, home);

    DynamicArray *files = darray_new(2);
    darray_add_item(files, file1);
    darray_add_item(files, file2);

    // Create containers directly (skip the index structure)
    store->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               NULL);
    store->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               NULL);

    // Arrays are now owned by the containers, so we need to unref them
    darray_unref(folders);
    darray_unref(files);
}

// Test save and load with actual data
static void
test_database_file_save_load_with_data(void) {
    g_autoptr(GError) error = NULL;

    // Create temporary directory for test
    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    // Create test store with data
    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);
    populate_test_store_with_entries(store);

    // Save the database
    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    // Verify file was created
    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    g_assert_true(g_file_test(db_file, G_FILE_TEST_EXISTS));

    // Check file size is reasonable (not empty)
    struct stat st;
    g_assert_cmpint(stat(db_file, &st), ==, 0);
    g_assert_cmpuint(st.st_size, >, 0);

    // Load the database back
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);

    // Verify properties match
    if (loaded_store) {
        g_assert_cmpuint(store->flags, ==, loaded_store->flags);
    }

    // Cleanup
    free_test_store(store);
    if (loaded_store) {
        free_test_store(loaded_store);
    }
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    // Remove test directory
    remove(db_file);
    rmdir(temp_dir);
}

// Test save with invalid path
static void
test_database_file_save_invalid_path(void) {
    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    // Try to save to non-existent directory
    bool result = database_file_save(store, "/this/path/does/not/exist");
    g_assert_false(result);

    free_test_store(store);
}

// Test load with invalid file
static void
test_database_file_load_invalid_file(void) {
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    // Try to load from non-existent file
    bool result = database_file_load("/this/file/does/not/exist.db",
                                     NULL,
                                     &loaded_store,
                                     &loaded_include_manager,
                                     &loaded_exclude_manager);
    g_assert_false(result);
    g_assert_null(loaded_store);
}

// Test save and load preserves flags
static void
test_database_file_save_load_preserves_flags(void) {
    g_autoptr(GError) error = NULL;

    // Create temporary directory for test
    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    // Create test store with specific flags
    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    // Set various flags
    store->flags = DATABASE_INDEX_PROPERTY_FLAG_NAME | DATABASE_INDEX_PROPERTY_FLAG_SIZE
                 | DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME | DATABASE_INDEX_PROPERTY_FLAG_PATH;

    // Save the database
    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    // Load the database back
    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);

    // Verify flags are preserved
    if (loaded_store) {
        g_assert_cmpuint(store->flags, ==, loaded_store->flags);
    }

    // Cleanup
    free_test_store(store);
    if (loaded_store) {
        free_test_store(loaded_store);
    }
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    // Remove test directory
    remove(db_file);
    rmdir(temp_dir);
}

static void
test_database_file_save_load_large_dataset(void) {
    g_autoptr(GError) error = NULL;

    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    const uint64_t flags = store->flags;
    FsearchDatabaseEntry *root = db_entry_new(flags, "/", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_size(root, 0);
    db_entry_set_mtime(root, 1234567890);
    db_entry_set_index(root, 0);

    DynamicArray *folders = darray_new(100);
    darray_add_item(folders, root);

    DynamicArray *files = darray_new(1000);

    for (int i = 1; i < 100; i++) {
        g_autofree char *folder_name = g_strdup_printf("folder_%d", i);
        FsearchDatabaseEntry *folder = db_entry_new(flags, folder_name, root, DATABASE_ENTRY_TYPE_FOLDER);
        db_entry_set_size(folder, 4096);
        db_entry_set_mtime(folder, 1234567890 + i);
        db_entry_set_index(folder, i);
        darray_add_item(folders, folder);
    }

    for (int i = 0; i < 1000; i++) {
        g_autofree char *file_name = g_strdup_printf("file_%d.txt", i);
        FsearchDatabaseEntry *parent = darray_get_item(folders, (i % 99) + 1);
        FsearchDatabaseEntry *file = db_entry_new(flags, file_name, parent, DATABASE_ENTRY_TYPE_FILE);
        db_entry_set_size(file, 1024 * (i + 1));
        db_entry_set_mtime(file, 1234567890 + i);
        db_entry_set_index(file, i);
        darray_add_item(files, file);
    }

    store->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               NULL);
    store->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               NULL);

    darray_unref(folders);
    darray_unref(files);

    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);
    g_assert_nonnull(loaded_store);

    g_autoptr(FsearchDatabaseEntriesContainer) loaded_folder_container =
        loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;
    g_autoptr(FsearchDatabaseEntriesContainer) loaded_file_container =
        loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;

    g_assert_nonnull(loaded_folder_container);
    g_assert_nonnull(loaded_file_container);
    g_assert_cmpuint(fsearch_database_entries_container_get_num_entries(loaded_folder_container), ==, 100);
    g_assert_cmpuint(fsearch_database_entries_container_get_num_entries(loaded_file_container), ==, 1000);

    free_test_store(store);
    free_test_store(loaded_store);
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    remove(db_file);
    rmdir(temp_dir);
}

static void
test_database_file_save_load_preserves_entry_data(void) {
    g_autoptr(GError) error = NULL;

    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);
    populate_test_store_with_entries(store);

    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);
    g_assert_nonnull(loaded_store);

    g_autoptr(FsearchDatabaseEntriesContainer) loaded_folder_container =
        loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;
    g_autoptr(FsearchDatabaseEntriesContainer) loaded_file_container =
        loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;

    g_assert_nonnull(loaded_folder_container);
    g_assert_nonnull(loaded_file_container);

    FsearchDatabaseEntry *root = fsearch_database_entries_container_get_entry(loaded_folder_container, 0);
    g_assert_nonnull(root);
    g_assert_cmpstr(db_entry_get_name_raw(root), ==, "/");
    g_assert_cmpint(db_entry_get_size(root), ==, 0);
    g_assert_cmpint(db_entry_get_mtime(root), ==, 1234567890);

    FsearchDatabaseEntry *home = fsearch_database_entries_container_get_entry(loaded_folder_container, 1);
    g_assert_nonnull(home);
    g_assert_cmpstr(db_entry_get_name_raw(home), ==, "home");
    g_assert_cmpint(db_entry_get_size(home), ==, 4096);
    g_assert_cmpint(db_entry_get_mtime(home), ==, 1234567900);
    g_assert_true(db_entry_get_parent(home) == root);

    FsearchDatabaseEntry *file0 = fsearch_database_entries_container_get_entry(loaded_file_container, 0);
    FsearchDatabaseEntry *file1 = fsearch_database_entries_container_get_entry(loaded_file_container, 1);
    g_assert_nonnull(file0);
    g_assert_nonnull(file1);

    const char *name0 = db_entry_get_name_raw(file0);
    const char *name1 = db_entry_get_name_raw(file1);

    if (g_strcmp0(name0, "readme.md") == 0) {
        g_assert_cmpstr(name0, ==, "readme.md");
        g_assert_cmpint(db_entry_get_size(file0), ==, 2048);
        g_assert_cmpint(db_entry_get_mtime(file0), ==, 1234567920);
        g_assert_true(db_entry_get_parent(file0) == home);

        g_assert_cmpstr(name1, ==, "test.txt");
        g_assert_cmpint(db_entry_get_size(file1), ==, 1024);
        g_assert_cmpint(db_entry_get_mtime(file1), ==, 1234567910);
        g_assert_true(db_entry_get_parent(file1) == home);
    }
    else {
        g_assert_cmpstr(name0, ==, "test.txt");
        g_assert_cmpint(db_entry_get_size(file0), ==, 1024);
        g_assert_cmpint(db_entry_get_mtime(file0), ==, 1234567910);
        g_assert_true(db_entry_get_parent(file0) == home);

        g_assert_cmpstr(name1, ==, "readme.md");
        g_assert_cmpint(db_entry_get_size(file1), ==, 2048);
        g_assert_cmpint(db_entry_get_mtime(file1), ==, 1234567920);
        g_assert_true(db_entry_get_parent(file1) == home);
    }

    free_test_store(store);
    free_test_store(loaded_store);
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    remove(db_file);
    rmdir(temp_dir);
}

static void
test_database_file_save_load_nested_folders(void) {
    g_autoptr(GError) error = NULL;

    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    const uint64_t flags = store->flags;

    FsearchDatabaseEntry *root = db_entry_new(flags, "/", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_index(root, 0);

    FsearchDatabaseEntry *level1 = db_entry_new(flags, "level1", root, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_index(level1, 1);

    FsearchDatabaseEntry *level2 = db_entry_new(flags, "level2", level1, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_index(level2, 2);

    FsearchDatabaseEntry *level3 = db_entry_new(flags, "level3", level2, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_index(level3, 3);

    DynamicArray *folders = darray_new(4);
    darray_add_item(folders, root);
    darray_add_item(folders, level1);
    darray_add_item(folders, level2);
    darray_add_item(folders, level3);

    DynamicArray *files = darray_new(1);
    FsearchDatabaseEntry *deep_file = db_entry_new(flags, "deep.txt", level3, DATABASE_ENTRY_TYPE_FILE);
    db_entry_set_size(deep_file, 512);
    db_entry_set_index(deep_file, 0);
    darray_add_item(files, deep_file);

    store->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               NULL);
    store->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               NULL);

    darray_unref(folders);
    darray_unref(files);

    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);

    g_autoptr(FsearchDatabaseEntriesContainer) loaded_folder_container =
        loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;
    g_autoptr(FsearchDatabaseEntriesContainer) loaded_file_container =
        loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;

    g_assert_nonnull(loaded_folder_container);
    g_assert_nonnull(loaded_file_container);

    FsearchDatabaseEntry *loaded_root = fsearch_database_entries_container_get_entry(loaded_folder_container, 0);
    FsearchDatabaseEntry *loaded_level1 = fsearch_database_entries_container_get_entry(loaded_folder_container, 1);
    FsearchDatabaseEntry *loaded_level2 = fsearch_database_entries_container_get_entry(loaded_folder_container, 2);
    FsearchDatabaseEntry *loaded_level3 = fsearch_database_entries_container_get_entry(loaded_folder_container, 3);

    g_assert_true(db_entry_get_parent(loaded_level1) == loaded_root);
    g_assert_true(db_entry_get_parent(loaded_level2) == loaded_level1);
    g_assert_true(db_entry_get_parent(loaded_level3) == loaded_level2);

    FsearchDatabaseEntry *loaded_file = fsearch_database_entries_container_get_entry(loaded_file_container, 0);
    g_assert_true(db_entry_get_parent(loaded_file) == loaded_level3);

    free_test_store(store);
    free_test_store(loaded_store);
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    remove(db_file);
    rmdir(temp_dir);
}

static void
test_database_file_save_load_empty_database(void) {
    g_autoptr(GError) error = NULL;

    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);
    g_assert_nonnull(loaded_store);

    g_autoptr(FsearchDatabaseEntriesContainer) loaded_folder_container =
        loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->folder_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;
    g_autoptr(FsearchDatabaseEntriesContainer) loaded_file_container =
        loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;

    g_assert_nonnull(loaded_folder_container);
    g_assert_nonnull(loaded_file_container);
    g_assert_cmpuint(fsearch_database_entries_container_get_num_entries(loaded_folder_container), ==, 0);
    g_assert_cmpuint(fsearch_database_entries_container_get_num_entries(loaded_file_container), ==, 0);

    free_test_store(store);
    free_test_store(loaded_store);
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    remove(db_file);
    rmdir(temp_dir);
}

static void
test_database_file_save_load_special_characters(void) {
    g_autoptr(GError) error = NULL;

    g_autofree char *temp_dir = g_dir_make_tmp("fsearch_test_XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(temp_dir);

    FsearchDatabaseIndexStore *store = create_test_store();
    g_assert_nonnull(store);

    const uint64_t flags = store->flags;

    FsearchDatabaseEntry *root = db_entry_new(flags, "/", NULL, DATABASE_ENTRY_TYPE_FOLDER);
    db_entry_set_index(root, 0);

    DynamicArray *folders = darray_new(1);
    darray_add_item(folders, root);

    DynamicArray *files = darray_new(5);

    const char *special_names[] = {"file with spaces.txt",
                                   "файл.txt",
                                   "文件.txt",
                                   "file-dash_underscore.txt",
                                   "file.multiple.dots.txt"};

    for (int i = 0; i < 5; i++) {
        FsearchDatabaseEntry *file = db_entry_new(flags, special_names[i], root, DATABASE_ENTRY_TYPE_FILE);
        db_entry_set_size(file, 100 + i);
        db_entry_set_index(file, i);
        darray_add_item(files, file);
    }

    store->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               NULL);
    store->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               NULL);

    darray_unref(folders);
    darray_unref(files);

    bool save_result = database_file_save(store, temp_dir);
    g_assert_true(save_result);

    g_autofree char *db_file = g_build_filename(temp_dir, "fsearch.db", NULL);
    FsearchDatabaseIndexStore *loaded_store = NULL;
    FsearchDatabaseIncludeManager *loaded_include_manager = NULL;
    FsearchDatabaseExcludeManager *loaded_exclude_manager = NULL;

    bool load_result = database_file_load(db_file, NULL, &loaded_store, &loaded_include_manager, &loaded_exclude_manager);
    g_assert_true(load_result);
    g_assert_nonnull(loaded_store);

    g_autoptr(FsearchDatabaseEntriesContainer) loaded_file_container =
        loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME]
            ? fsearch_database_entries_container_ref(loaded_store->file_container[DATABASE_INDEX_PROPERTY_NAME])
            : NULL;

    g_assert_nonnull(loaded_file_container);
    g_assert_cmpuint(fsearch_database_entries_container_get_num_entries(loaded_file_container), ==, 5);

    bool found[5] = {false, false, false, false, false};
    for (int i = 0; i < 5; i++) {
        FsearchDatabaseEntry *file = fsearch_database_entries_container_get_entry(loaded_file_container, i);
        g_assert_nonnull(file);
        const char *name = db_entry_get_name_raw(file);
        for (int j = 0; j < 5; j++) {
            if (g_strcmp0(name, special_names[j]) == 0) {
                g_assert_false(found[j]);
                found[j] = true;
                break;
            }
        }
    }

    for (int i = 0; i < 5; i++) {
        g_assert_true(found[i]);
    }

    free_test_store(store);
    free_test_store(loaded_store);
    g_clear_object(&loaded_include_manager);
    g_clear_object(&loaded_exclude_manager);

    remove(db_file);
    rmdir(temp_dir);
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/FSearch/database/file_save_load_with_data", test_database_file_save_load_with_data);
    g_test_add_func("/FSearch/database/file_save_invalid_path", test_database_file_save_invalid_path);
    g_test_add_func("/FSearch/database/file_load_invalid_file", test_database_file_load_invalid_file);
    g_test_add_func("/FSearch/database/file_save_load_preserves_flags", test_database_file_save_load_preserves_flags);
    g_test_add_func("/FSearch/database/file_save_load_large_dataset", test_database_file_save_load_large_dataset);
    g_test_add_func("/FSearch/database/file_save_load_preserves_entry_data",
                    test_database_file_save_load_preserves_entry_data);
    g_test_add_func("/FSearch/database/file_save_load_nested_folders", test_database_file_save_load_nested_folders);
    g_test_add_func("/FSearch/database/file_save_load_empty_database", test_database_file_save_load_empty_database);
    g_test_add_func("/FSearch/database/file_save_load_special_characters",
                    test_database_file_save_load_special_characters);

    return g_test_run();
}