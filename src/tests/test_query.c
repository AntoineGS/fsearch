#include "fsearch_database_entry.h"
#include <glib.h>
#include <locale.h>
#include <stdlib.h>

#include <src/fsearch_limits.h>
#include <src/fsearch_query.h>

typedef struct QueryTest {
    const char *needle;
    const char *haystack;
    bool is_dir;
    off_t size;
    FsearchQueryFlags flags;
    bool result;
} QueryTest;

static void
test_query(QueryTest *t) {
    FsearchFilterManager *manager = fsearch_filter_manager_new_with_defaults();

    FsearchQuery *q = fsearch_query_new(t->needle, NULL, manager, t->flags, "debug_query");

    FsearchDatabaseIndexPropertyFlags flags = DATABASE_INDEX_PROPERTY_FLAG_SIZE;
    FsearchDatabaseEntry *entry = NULL;
    if (g_str_has_prefix(t->haystack, "/")) {
        const char *haystack = t->haystack + 1;
        g_auto(GStrv) names = g_strsplit(haystack, "/", -1);
        const guint names_len = g_strv_length(names);

        entry = db_entry_new(flags, "", NULL, DATABASE_ENTRY_TYPE_FOLDER);

        if (names_len > 0) {
            for (int i = 0; i < names_len - 1; i++) {
                FsearchDatabaseEntry *old = entry;
                entry = db_entry_new(flags, names[i], old, DATABASE_ENTRY_TYPE_FOLDER);
            }
            FsearchDatabaseEntry *old = entry;
            entry = db_entry_new_with_attributes(flags,
                                                 names[names_len - 1],
                                                 old,
                                                 t->is_dir ? DATABASE_ENTRY_TYPE_FOLDER : DATABASE_ENTRY_TYPE_FILE,
                                                 DATABASE_INDEX_PROPERTY_SIZE,
                                                 t->size,
                                                 DATABASE_INDEX_PROPERTY_NONE);
        }
    }
    else {
        entry = db_entry_new_with_attributes(flags,
                                             t->haystack,
                                             NULL,
                                             t->is_dir ? DATABASE_ENTRY_TYPE_FOLDER : DATABASE_ENTRY_TYPE_FILE,
                                             DATABASE_INDEX_PROPERTY_SIZE,
                                             t->size,
                                             DATABASE_INDEX_PROPERTY_NONE);
    }

    FsearchQueryMatchData *match_data = fsearch_query_match_data_new(NULL, NULL);
    fsearch_query_match_data_set_entry(match_data, entry);

    const bool found = fsearch_query_match(q, match_data);
    g_clear_pointer(&manager, fsearch_filter_manager_unref);
    g_clear_pointer(&q, fsearch_query_unref);
    g_clear_pointer(&match_data, fsearch_query_match_data_free);

    if (found != t->result) {
        g_printerr("[%s] should%s match [name:%s, size:%ld]\n", t->needle, t->result ? "" : " NOT", t->haystack, t->size);
    }
    g_assert_true(found == t->result);
}

static bool
set_locale(const char *locale) {
    char *current_locale = setlocale(LC_CTYPE, NULL);

    if (strcmp(locale, current_locale) != 0) {
        setlocale(LC_CTYPE, locale);
        current_locale = setlocale(LC_CTYPE, NULL);

        if (strncmp(current_locale, locale, 2) != 0) {
            g_printerr("Failed to set locale to %s. Skipping test.\n", locale);
            return false;
        }
    }
    return true;
}

static void
test_main(void) {
    if (set_locale("en_US.UTF-8")) {
        QueryTest main_tests[] = {
            // Mismatches
            {"i j l", "I J K", false, 0, 0, false},
            {"i", "j", false, 0, 0, false},
            {"i", "ı", false, 0, 0, false},
            {"abc", "ab_c", false, 0, 0, false},

            {"é", "e", false, 0, 0, false},
            {"ó", "o", false, 0, 0, false},
            {"å", "a", false, 0, 0, false},

            // ensure that we don't match turkic "i" mappings
            {"ı", "i", false, 0, 0, false},
            {"ı", "I", false, 0, 0, false},
            {"i", "ı", false, 0, 0, false},
            {"i", "İ", false, 0, 0, false},
            {"I", "ı", false, 0, 0, false},
            {"İ", "i", false, 0, 0, false},
            // wildcards
            {"?", "aa", false, 0, 0, false},
            {"*.txt", "testtxt", false, 0, 0, false},
            // regex
            {"^a", "ba", false, 0, QUERY_FLAG_REGEX, false},
            // match case
            {"a", "A", false, 0, QUERY_FLAG_MATCH_CASE, false},
            // auto match case
            {"A", "a", false, 0, QUERY_FLAG_AUTO_MATCH_CASE, false},

            // Matches
            {"é", "É", false, 0, 0, true},
            {"ó", "Ó", false, 0, 0, true},
            {"å", "Å", false, 0, 0, true},
            {"É", "é", false, 0, 0, true},
            {"Ó", "Ó", false, 0, 0, true},
            {"Å", "å", false, 0, 0, true},

            {"ﬀ", "affe", false, 0, 0, true},
            {"i", "I J K", false, 0, 0, true},
            {"j i", "I J K", false, 0, 0, true},
            {"i j", "İIäój", false, 0, 0, true},
            {"abc", "abcdef", false, 0, 0, true},
            {"ab cd", "abcdef", false, 0, 0, true},
            // wildcards
            {"?", "ı", false, 0, 0, true},
            {"*c*f", "abcdef", false, 0, 0, true},
            {"ab*ef", "abcdef", false, 0, 0, true},
            {"abc?ef", "abcdef", false, 0, 0, true},
            // regex
            {"^b", "ba", false, 0, QUERY_FLAG_REGEX, true},
            {"^B", "ba", false, 0, QUERY_FLAG_REGEX, true},
            // match case
            {"a", "a", false, 0, QUERY_FLAG_MATCH_CASE, true},
            // auto match case
            {"A", "A", false, 0, QUERY_FLAG_AUTO_MATCH_CASE, true},

            // boolean logic
            {"a && (b || c)", "ab", false, 0, 0, true},
            {"a && (b || c)", "ac", false, 0, 0, true},
            {"a && (b || c)", "ad", false, 0, 0, false},
            {"a && (b || c)", "bc", false, 0, 0, false},
            {"a && (b || c || d || e)", "ae", false, 0, 0, true},
            {"a && (b || (c && d))", "bc", false, 0, 0, false},
            {"a && (b || (c && d))", "ac", false, 0, 0, false},
            {"a && (b || (c && d))", "bcd", false, 0, 0, false},
            {"a && (b || (c && d))", "acd", false, 0, 0, true},
            {"a && (b || (c && d))", "ab", false, 0, 0, true},
            {"!a", "b", false, 0, 0, true},
            {"!b", "b", false, 0, 0, false},
            {"!!b", "b", false, 0, 0, true},
            {"a && !(b || c)", "abc", false, 0, 0, false},
            {"a && !(b || !c)", "ac", false, 0, 0, true},
            {"a && !(b || !c)", "ac", false, 0, 0, true},
            {"a (b || c)", "ac", false, 0, 0, true},
            {"a (b || c)", "ab", false, 0, 0, true},
            {"a (b || c)", "a", false, 0, 0, false},
            {"a (b || c)", "b", false, 0, 0, false},
            {"a (b || c)", "c", false, 0, 0, false},
            {"a (b || c)", "bc", false, 0, 0, false},
            {"a !b", "ac", false, 0, 0, true},
            {"a !b", "ab", false, 0, 0, false},
            {"a !b", "cd", false, 0, 0, false},
            {"a b !c", "abc", false, 0, 0, false},
            {"a b !c", "abd", false, 0, 0, true},
            {"a b c !d", "abcd", false, 0, 0, false},
            {"a b c !d", "abce", false, 0, 0, true},
            // Closing bracket without corresponding open bracket
            //{"a)", "a", 0, 0, false},
            {"a !b || c)", "ad", false, 0, 0, false},
            {"a !b || c)", "c", false, 0, 0, false},
            {"a !b || c)", "ac", false, 0, 0, false},
            {"a !b || c)", "ab", false, 0, 0, false},
            {"a !b || c)", "b", false, 0, 0, false},

            // fields
            {"size:1", "test", false, 1, 0, true},
            {"size:300..", "test", false, 1000, 0, true},
            {"size:300..", "test", false, 200, 0, false},
            {"size:>300", "test", false, 301, 0, true},
            {"size:>300", "test", false, 300, 0, false},
            {"size:>=300", "test", false, 300, 0, true},
            {"size:>300 size:<400", "test", false, 350, 0, true},
            {"size:>300 size:<400", "test", false, 250, 0, false},
            {"size:>300 size:<400", "test", false, 450, 0, false},
            {"size:>1MB", "test", false, 1000001, 0, true},
            {"size:>1MB", "test", false, 1000000, 0, false},
            {"size:abc", "test", false, 1000000, 0, false},
            {"size:abc test", "test", false, 1000000, 0, false},
            {"size:abc abc", "test", false, 1000000, 0, false},
            // bug #388
            {"size:1kb..2kb", "test", false, 1000, 0, true},

            {"regex:suffix$", "suffix prefix", false, 0, 0, false},
            {"regex:suffix$", "prefix suffix", false, 0, 0, true},
            {"exact:ABC", "aBc", false, 0, 0, true},
            {"exact:ABC", "aBcd", false, 0, 0, false},
            {"case:exact:ABC", "aBc", false, 0, 0, false},
            {"exact:Ȁ", "Ȁ", false, 0, 0, true},
            {"exact:ȁ", "Ȁ", false, 0, 0, true},
            {"exact:Ȁ", "ȁ", false, 0, 0, true},
            {"case:exact:ȁ", "Ȁ", false, 0, 0, false},
            {"case:exact:Ȁ", "ȁ", false, 0, 0, false},
            {"case:exact:Ȁ", "Ȁ", false, 0, 0, true},
            {"exact:Ȁ", "Ȁb", false, 0, 0, false},
            {"case:(A (b || c)) d", "AbD", false, 0, 0, true},
            {"D case:(A (b || c))", "Acd", false, 0, 0, true},
            {"case:(A (b || c)) d", "ab", false, 0, 0, false},
            {"case:(A (b || c)) d", "AC", false, 0, 0, false},
            {"!case:(A || B) c", "ac", false, 0, 0, true},
            {"!case:(A || B) c", "bc", false, 0, 0, true},
            {"!case:(A || B) c", "abc", false, 0, 0, true},
            {"!case:(A || B) c", "Ac", false, 0, 0, false},
            {"!case:(A || B) c", "Bc", false, 0, 0, false},
            {"!case:(A || B) c", "ABc", false, 0, 0, false},
            {"!case:(A || B) c", "abd", false, 0, 0, false},
            {"ext:pdf;jpg", "test.pdf", false, 0, 0, true},
            {"ext:pdf;jpg", "test.jpg", false, 0, 0, true},
            {"ext:pdf;jpg", "test.c", false, 0, 0, false},
            {"ext:", "test.c", false, 0, 0, false},
            {"ext:", "test", false, 0, 0, true},
            {"case:(TE || AB) cd", "TEcd", false, 0, 0, true},
            {"case:(TE || AB) cd", "ABcd", false, 0, 0, true},
            {"case:(TE || AB) cd", "AB", false, 0, 0, false},
            {"case:(TE || AB) cd", "TE", false, 0, 0, false},
            {"case:(TE || AB) cd", "ABTE", false, 0, 0, false},
            {"case:(TE || AB) cd", "cd", false, 0, 0, false},
            {"nocase:a", "A", false, 0, QUERY_FLAG_MATCH_CASE, true},

            {"depth:0", "/", false, 0, 0, true},
            {"depth:2", "/1/2/3", false, 0, 0, false},
            {"depth:3", "/1/2/3", false, 0, 0, true},

            {"path:d", "/a/b/c", false, 0, 0, false},
            {"path:a", "/a/b/c", false, 0, 0, true},
            {"path:b", "/a/b/c", false, 0, 0, true},
            {"path:c", "/a/b/c", false, 0, 0, true},
            {"path:/", "/a/b/c", false, 0, 0, true},
            {"path:/a/b/c", "/a/b/c", false, 0, 0, true},
            {"path:(a && b && c && d)", "/a/b/c", false, 0, 0, false},
            {"path:(a && b && c)", "/a/b/c", false, 0, 0, true},

            {"parent:/b/a", "/a/b/c", false, 0, 0, false},
            {"parent:/a/b", "/a/b/c", false, 0, 0, true},
            {"parent:/a", "/a/b", false, 0, 0, true},
            {"parent:/", "/a", false, 0, 0, true},
            {"parent:/a/b/c", "/a/b/c/d", false, 0, 0, true},

            // depth edge cases
            {"depth:1", "/a", false, 0, 0, true},
            {"depth:0", "/a", false, 0, 0, false},
            {"depth:4", "/a/b/c/d", false, 0, 0, true},
            {"depth:<3", "/a/b", false, 0, 0, true},
            {"depth:<3", "/a/b/c", false, 0, 0, false},
            {"depth:>2", "/a/b/c", false, 0, 0, true},
            {"depth:>2", "/a/b", false, 0, 0, false},
            {"depth:2..4", "/a/b/c", false, 0, 0, true},
            {"depth:2..4", "/a", false, 0, 0, false},
            {"depth:2..4", "/a/b/c/d/e", false, 0, 0, false},

            // path with wildcards
            {"path:*a*", "/foo/bar/baz", false, 0, 0, true},
            {"path:*/foo/*", "/dir/foo/bar", false, 0, 0, true},
            {"path:*/foo/*", "/dir/bar/baz", false, 0, 0, false},

            // combined path and name searches
            {"path:home name.txt", "/home/user/name.txt", false, 0, 0, true},
            {"path:home name.txt", "/var/log/name.txt", false, 0, 0, false},
            {"path:/usr lib", "/usr/local/lib", false, 0, 0, true},

            // folder type matching
            {"folder:", "testdir", true, 0, 0, true},
            {"folder:", "testfile", false, 0, 0, false},
            {"folder:test", "testdir", true, 0, 0, true},
            {"folder:test", "testfile", false, 0, 0, false},
            {"!folder:", "testfile", false, 0, 0, true},
            {"!folder:", "testdir", true, 0, 0, false},

            // file type matching
            {"file:", "testfile", false, 0, 0, true},
            {"file:", "testdir", true, 0, 0, false},
            {"file:doc", "document.txt", false, 0, 0, true},
            {"file:doc", "docdir", true, 0, 0, false},

            // size edge cases
            {"size:0", "empty", false, 0, 0, true},
            {"size:0", "nonempty", false, 1, 0, false},
            {"size:<100", "small", false, 99, 0, true},
            {"size:<100", "large", false, 100, 0, false},
            {"size:<=100", "exact", false, 100, 0, true},
            {"size:1..100", "mid", false, 50, 0, true},
            {"size:1..100", "zero", false, 0, 0, false},
            {"size:1..100", "toobig", false, 101, 0, false},
            {"size:1KB..1MB", "medium", false, 500000, 0, true},
            {"size:>1GB", "huge", false, 2000000000, 0, true},

            // empty and whitespace queries
            {"", "anything", false, 0, 0, true},
            {"   ", "anything", false, 0, 0, true},

            // special characters in names
            {"test-file", "test-file.txt", false, 0, 0, true},
            {"test_file", "test_file.txt", false, 0, 0, true},
            {"test.old", "test.old.bak", false, 0, 0, true},

            // multiple extensions
            {"ext:txt;md;rst", "readme.txt", false, 0, 0, true},
            {"ext:txt;md;rst", "readme.md", false, 0, 0, true},
            {"ext:txt;md;rst", "readme.rst", false, 0, 0, true},
            {"ext:txt;md;rst", "readme.pdf", false, 0, 0, false},
            {"ext:gz", "archive.tar.gz", false, 0, 0, true},
            {"ext:tar", "archive.tar.gz", false, 0, 0, false},

            // case sensitivity edge cases
            {"exact:test", "TEST", false, 0, 0, true},
            {"exact:test", "Test", false, 0, 0, true},
            {"exact:test", "test", false, 0, 0, true},
            {"exact:test", "testing", false, 0, 0, false},
            {"case:exact:test", "TEST", false, 0, 0, false},
            {"case:exact:test", "test", false, 0, 0, true},

            // regex edge cases
            {"regex:^test$", "test", false, 0, 0, true},
            {"regex:^test$", "test.txt", false, 0, 0, false},
            {"regex:^test$", "mytest", false, 0, 0, false},
            {"regex:[0-9]+", "file123", false, 0, 0, true},
            {"regex:[0-9]+", "filename", false, 0, 0, false},
            {"regex:test|demo", "testfile", false, 0, 0, true},
            {"regex:test|demo", "demofile", false, 0, 0, true},
            {"regex:test|demo", "prodfile", false, 0, 0, false},

            // complex boolean combinations
            {"a && b && c && d", "abcd", false, 0, 0, true},
            {"a && b && c && d", "abc", false, 0, 0, false},
            {"a || b || c || d", "a", false, 0, 0, true},
            {"a || b || c || d", "e", false, 0, 0, false},
            {"(a || b) && (c || d)", "ac", false, 0, 0, true},
            {"(a || b) && (c || d)", "bd", false, 0, 0, true},
            {"(a || b) && (c || d)", "ab", false, 0, 0, false},
            {"(a || b) && (c || d)", "cd", false, 0, 0, false},
            {"a && !b && c", "ac", false, 0, 0, true},
            {"a && !b && c", "abc", false, 0, 0, false},
            {"!(a || b) && c", "c", false, 0, 0, true},
            {"!(a || b) && c", "ac", false, 0, 0, false},
            {"!(a || b) && c", "bc", false, 0, 0, false},

            // path depth combinations
            {"path:a depth:2", "/a/b", false, 0, 0, true},
            {"path:a depth:2", "/a/b/c", false, 0, 0, false},
            {"path:a depth:3", "/a/b/c", false, 0, 0, true},

            // size with other filters
            {"size:>100 ext:txt", "large.txt", false, 200, 0, true},
            {"size:>100 ext:txt", "large.pdf", false, 200, 0, false},
            {"size:>100 ext:txt", "small.txt", false, 50, 0, false},
            {"folder: size:0", "emptydir", true, 0, 0, true},
            {"file: size:0", "emptyfile", false, 0, 0, true},

            // parent with name matching
            {"parent:/home test", "/home/user/test.txt", false, 0, 0, false},
            {"parent:/home/user test", "/home/user/test.txt", false, 0, 0, true},

            // deeply nested paths
            {"depth:10", "/1/2/3/4/5/6/7/8/9/10", false, 0, 0, true},
            {"parent:/1/2/3/4/5", "/1/2/3/4/5/6", false, 0, 0, true},
            {"path:/1/2/3/4/5/6/7/8/9/10", "/1/2/3/4/5/6/7/8/9/10", false, 0, 0, true},

            // macros
            {"test || (pic: video:)", "test.jpg", false, 0, 0, true},
            {"test || (pic: video:)", "test.mp4", false, 0, 0, true},
            {"test || (pic: video:)", "test.mp4", false, 0, 0, true},
            {"test || (pic: video:)", "test.doc", false, 0, 0, true},
            {"test || (pic: video:)", "test.doc", false, 0, 0, true},

            // bug reports:
            // #360
            {"(", "test", false, 0, QUERY_FLAG_REGEX, false},
            {"folder:", "", false, 0, 0, false},

        };

        for (uint32_t i = 0; i < G_N_ELEMENTS(main_tests); i++) {
            QueryTest *t = &main_tests[i];
            g_print("main_query: %d\n", i);
            test_query(t);
        }
    }
}

static void
test_turkic_case_mapping(void) {
    if (set_locale("tr_TR.UTF-8")) {
        QueryTest tr_tests[] = {
            // Mismatches
            {"i", "ı", false, 0, false},
            {"i", "I", false, 0, false},
            {"ı", "i", false, 0, false},
            {"ı", "İ", false, 0, false},
            {"İ", "ı", false, 0, false},
            {"İ", "I", false, 0, false},
            {"I", "i", false, 0, false},
            {"I", "İ", false, 0, false},

            // Matches
            {"ı", "I", false, 0, true},
            {"i", "İ", false, 0, true},
            // trigger 0, wildcard search
            //{"ı*", "I", 0, true},
            //{"i*", "İ", 0, true},
            //{"I*", "ı", 0, true},
            //{"İ*", "i", 0, true},
        };

        for (uint32_t i = 0; i < G_N_ELEMENTS(tr_tests); i++) {
            QueryTest *t = &tr_tests[i];
            test_query(t);
            // the tests still need to pass if haystack and needle are swapped, since they're all single characters
            test_query(t);
        }
    }
}

static void
test_german_case_mapping(void) {
    if (set_locale("de_DE.UTF-8")) {
        QueryTest de_tests[] = {
            // Mismatches
            {"a", "ä", false, 0, 0, false},
            {"A", "ä", false, 0, 0, false},
            {"a", "Ä", false, 0, 0, false},
            {"A", "Ä", false, 0, 0, false},
            {"o", "ö", false, 0, 0, false},
            {"O", "ö", false, 0, 0, false},
            {"o", "Ö", false, 0, 0, false},
            {"O", "Ö", false, 0, 0, false},
            {"u", "ü", false, 0, 0, false},
            {"U", "ü", false, 0, 0, false},
            {"u", "Ü", false, 0, 0, false},
            {"U", "Ü", false, 0, 0, false},

            // Matches
            {"ä", "ä", false, 0, 0, true},
            {"ö", "ö", false, 0, 0, true},
            {"ü", "ü", false, 0, 0, true},
            {"Ä", "ä", false, 0, 0, true},
            {"Ö", "ö", false, 0, 0, true},
            {"Ü", "ü", false, 0, 0, true},

            {"ß", "ẞ", false, 0, 0, true},
        };

        for (uint32_t i = 0; i < G_N_ELEMENTS(de_tests); i++) {
            QueryTest *t = &de_tests[i];
            test_query(t);
            // the tests still need to pass if haystack and needle are swapped, since they're all single characters
            test_query(t);
        }
    }
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/query/main", test_main);
    g_test_add_func("/FSearch/query/mappings_turkic", test_turkic_case_mapping);
    g_test_add_func("/FSearch/query/mappings_german", test_german_case_mapping);
    return g_test_run();
}