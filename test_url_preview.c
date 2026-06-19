// test_url_preview.c — Unit tests using the GLib testing framework
//
// Compile:  make test
// Run:      ./build/test_url_preview          (summary)
//           ./build/test_url_preview -p       (list paths)
//           ./build/test_url_preview -m slow  (run specific)

/* Pull in the entire source so we can call static functions.
 * The stubs below satisfy profapi symbols only; libstrophe,
 * libcurl, and gio are linked against the real libraries.       */
#include "url_preview.c"

#include <glib.h>

/* ================================================================== */
/*  Stubs for profapi symbols (provided by Profanity at runtime)       */
/* ================================================================== */

void (*prof_log_debug)(const char* message) = NULL;
void (*prof_log_info)(const char* message) = NULL;
void (*prof_log_warning)(const char* message) = NULL;
void (*prof_log_error)(const char* message) = NULL;
int (*prof_cons_bad_cmd_usage)(const char* const cmd) = NULL;

int (*prof_settings_boolean_get)(char* group, char* key, int def) = NULL;
void (*prof_settings_boolean_set)(char* group, char* key, int value) = NULL;
char* (*prof_settings_string_get)(char* group, char* key, char* def) = NULL;
void (*prof_settings_string_set)(char* group, char* key, char* value) = NULL;
int (*prof_settings_int_get)(char* group, char* key, int def) = NULL;
void (*prof_settings_int_set)(char* group, char* key, int value) = NULL;

void (*_prof_register_command)(const char* filename, const char* command_name, int min_args, int max_args,
                               char** synopsis, const char* description, char* arguments[][2], char** examples,
                               CMD_CB callback)
    = NULL;
void (*_prof_completer_add)(const char* filename, const char* key, char** items) = NULL;

/* ================================================================== */
/*  Utility functions                                                  */
/* ================================================================== */

static void test_parse_bool_value(void)
{
    g_assert_true(parse_bool_value("on", FALSE));
    g_assert_false(parse_bool_value("off", TRUE));
    g_assert_true(parse_bool_value(NULL, TRUE));
    g_assert_false(parse_bool_value(NULL, FALSE));
    g_assert_true(parse_bool_value("yes", TRUE));   /* unknown → default */
    g_assert_false(parse_bool_value("no", FALSE));
}

static void test_parse_positive_int(void)
{
    gint val = 0;
    g_assert_true(parse_positive_int("42", &val));
    g_assert_cmpint(val, ==, 42);

    g_assert_true(parse_positive_int("1", &val));
    g_assert_cmpint(val, ==, 1);

    g_assert_false(parse_positive_int("0", &val));
    g_assert_false(parse_positive_int("-5", &val));
    g_assert_false(parse_positive_int("abc", &val));
    g_assert_false(parse_positive_int("", &val));
}

static void test_strv_lowercase_inplace(void)
{
    gchar **strv = g_strsplit("Hello,WORLD,FoO", ",", -1);
    strv_lowercase_inplace(strv);

    g_assert_cmpstr(strv[0], ==, "hello");
    g_assert_cmpstr(strv[1], ==, "world");
    g_assert_cmpstr(strv[2], ==, "foo");

    g_strfreev(strv);
}

static void test_strip_trailing_punct(void)
{
    gchar *s;

    s = g_strdup("https://example.com.");
    strip_trailing_punct(s);
    g_assert_cmpstr(s, ==, "https://example.com");
    g_free(s);

    s = g_strdup("https://example.com/page,");
    strip_trailing_punct(s);
    g_assert_cmpstr(s, ==, "https://example.com/page");
    g_free(s);

    s = g_strdup("https://example.com/page");
    strip_trailing_punct(s);
    g_assert_cmpstr(s, ==, "https://example.com/page");
    g_free(s);

    s = g_strdup("https://example.com/page!:");
    strip_trailing_punct(s);
    g_assert_cmpstr(s, ==, "https://example.com/page");
    g_free(s);

    s = g_strdup("https://example.com>");
    strip_trailing_punct(s);
    g_assert_cmpstr(s, ==, "https://example.com");
    g_free(s);
}

/* ================================================================== */
/*  Content detection                                                  */
/* ================================================================== */

static void test_content_type_is_html(void)
{
    g_assert_true(content_type_is_html("text/html"));
    g_assert_true(content_type_is_html("text/html; charset=utf-8"));
    g_assert_false(content_type_is_html("application/json"));
    g_assert_false(content_type_is_html(NULL));
    g_assert_false(content_type_is_html("image/png"));
}

static void test_sniff_is_html(void)
{
    g_assert_true(sniff_is_html("<html><body>hi</body></html>"));
    g_assert_true(sniff_is_html("<!DOCTYPE html>"));
    g_assert_true(sniff_is_html("<head><title>"));
    g_assert_true(sniff_is_html("<HTML>"));
    g_assert_false(sniff_is_html("just some text"));
    g_assert_false(sniff_is_html("{\"key\": \"value\"}"));
}

static void test_is_local_host(void)
{
    g_assert_true(is_local_host("localhost"));
    g_assert_true(is_local_host("myhost.local"));
    g_assert_true(is_local_host("127.0.0.1"));
    g_assert_true(is_local_host("10.0.0.1"));
    g_assert_true(is_local_host("192.168.1.1"));
    g_assert_true(is_local_host("172.16.0.1"));
    g_assert_true(is_local_host("169.254.1.1"));
    g_assert_false(is_local_host("example.com"));
    g_assert_false(is_local_host("8.8.8.8"));
    g_assert_false(is_local_host(NULL));
}

/* ================================================================== */
/*  HTML entity handling                                               */
/* ================================================================== */

static void test_try_named_entity(void)
{
    GString *out = g_string_new(NULL);
    gsize consumed;

    /* &amp; */
    consumed = try_named_entity("&amp;hello", out);
    g_assert_cmpuint(consumed, ==, 5);
    g_assert_cmpstr(out->str, ==, "&");
    g_string_truncate(out, 0);

    /* &lt; */
    consumed = try_named_entity("&lt;x", out);
    g_assert_cmpuint(consumed, ==, 4);
    g_assert_cmpstr(out->str, ==, "<");
    g_string_truncate(out, 0);

    /* &gt; */
    consumed = try_named_entity("&gt;", out);
    g_assert_cmpuint(consumed, ==, 4);
    g_assert_cmpstr(out->str, ==, ">");
    g_string_truncate(out, 0);

    /* No match */
    consumed = try_named_entity("&foo;", out);
    g_assert_cmpuint(consumed, ==, 0);
    g_assert_cmpstr(out->str, ==, "");

    g_string_free(out, TRUE);
}

static void test_try_numeric_entity(void)
{
    GString *out = g_string_new(NULL);
    gsize consumed;

    /* Decimal: &#65; = 'A' */
    consumed = try_numeric_entity("&#65;", out);
    g_assert_cmpuint(consumed, ==, 5);
    g_assert_cmpstr(out->str, ==, "A");
    g_string_truncate(out, 0);

    /* Hex: &#x41; = 'A' */
    consumed = try_numeric_entity("&#x41;", out);
    g_assert_cmpuint(consumed, ==, 6);
    g_assert_cmpstr(out->str, ==, "A");
    g_string_truncate(out, 0);

    /* Hex uppercase: &#X2F; = '/' */
    consumed = try_numeric_entity("&#X2F;", out);
    g_assert_cmpuint(consumed, ==, 6);
    g_assert_cmpstr(out->str, ==, "/");
    g_string_truncate(out, 0);

    /* Invalid: &#0; (not a valid Unicode codepoint) */
    consumed = try_numeric_entity("&#0;", out);
    g_assert_cmpuint(consumed, ==, 0);

    /* Not an entity at all */
    consumed = try_numeric_entity("hello", out);
    g_assert_cmpuint(consumed, ==, 0);

    g_string_free(out, TRUE);
}

static void test_unescape_html(void)
{
    gchar *r;

    /* Basic named entities */
    r = unescape_html("&amp; &lt; &gt;");
    g_assert_cmpstr(r, ==, "& < >");
    g_free(r);

    /* Numeric */
    r = unescape_html("&#65;&#66;&#67;");
    g_assert_cmpstr(r, ==, "ABC");
    g_free(r);

    /* Hex */
    r = unescape_html("&#x41;&#x42;");
    g_assert_cmpstr(r, ==, "AB");
    g_free(r);

    /* Mixed */
    r = unescape_html("a &amp; b &lt; c &#x2F;");
    g_assert_cmpstr(r, ==, "a & b < c /");
    g_free(r);

    /* Unknown entity: passed through literally */
    r = unescape_html("&unknown;");
    g_assert_cmpstr(r, ==, "&unknown;");
    g_free(r);

    /* No entities at all */
    r = unescape_html("plain text");
    g_assert_cmpstr(r, ==, "plain text");
    g_free(r);

    /* NULL */
    r = unescape_html(NULL);
    g_assert_cmpstr(r, ==, "");
    g_free(r);

    /* Empty string */
    r = unescape_html("");
    g_assert_cmpstr(r, ==, "");
    g_free(r);

    /* &nbsp; → space */
    r = unescape_html("hello&nbsp;world");
    g_assert_cmpstr(r, ==, "hello world");
    g_free(r);

    /* &quot; */
    r = unescape_html("&quot;hi&quot;");
    g_assert_cmpstr(r, ==, "\"hi\"");
    g_free(r);
}

/* ================================================================== */
/*  HTML parsing                                                       */
/* ================================================================== */

static void test_parse_html_attrs(void)
{
    GHashTable *ht;
    gchar *r;

    /* Simple double-quoted */
    ht = parse_html_attrs("name=\"description\" content=\"hello world\"");
    r = g_hash_table_lookup(ht, "name");
    g_assert_cmpstr(r, ==, "description");
    r = g_hash_table_lookup(ht, "content");
    g_assert_cmpstr(r, ==, "hello world");
    g_hash_table_unref(ht);

    /* Single-quoted */
    ht = parse_html_attrs("property='og:title' content='My Page'");
    r = g_hash_table_lookup(ht, "property");
    g_assert_cmpstr(r, ==, "og:title");
    r = g_hash_table_lookup(ht, "content");
    g_assert_cmpstr(r, ==, "My Page");
    g_hash_table_unref(ht);

    /* Key lowercased */
    ht = parse_html_attrs("NAME=\"test\"");
    r = g_hash_table_lookup(ht, "name");
    g_assert_nonnull(r);
    g_assert_cmpstr(r, ==, "test");
    g_hash_table_unref(ht);

    /* Empty attrs */
    ht = parse_html_attrs("");
    g_assert_cmpuint(g_hash_table_size(ht), ==, 0);
    g_hash_table_unref(ht);

    /* Mixed case keys normalized */
    ht = parse_html_attrs("ProPerty=\"og:image\" CoNteNT=\"img.png\"");
    r = g_hash_table_lookup(ht, "property");
    g_assert_cmpstr(r, ==, "og:image");
    g_hash_table_unref(ht);
}

static void test_extract_html_title(void)
{
    gchar *r;

    r = extract_html_title("<title>Hello World</title>");
    g_assert_cmpstr(r, ==, "Hello World");
    g_free(r);

    /* Whitespace trimming */
    r = extract_html_title("<title>  Hello  </title>");
    g_assert_cmpstr(r, ==, "Hello");
    g_free(r);

    /* Case-insensitive */
    r = extract_html_title("<TITLE>FoO</TITLE>");
    g_assert_cmpstr(r, ==, "FoO");
    g_free(r);

    /* No title */
    r = extract_html_title("<body>No title here</body>");
    g_assert_null(r);

    /* Empty title */
    r = extract_html_title("<title>   </title>");
    g_assert_null(r);

    /* Entity in title */
    r = extract_html_title("<title>Rock &amp; Roll</title>");
    g_assert_cmpstr(r, ==, "Rock & Roll");
    g_free(r);
}

static void test_extract_html_description(void)
{
    gchar *r;

    /* og:description takes priority */
    r = extract_html_description(
        "<meta property=\"og:description\" content=\"OG Desc\">"
        "<meta name=\"description\" content=\"Meta Desc\">");
    g_assert_cmpstr(r, ==, "OG Desc");
    g_free(r);

    /* Fallback to name=description */
    r = extract_html_description(
        "<meta name=\"description\" content=\"Meta Desc\">");
    g_assert_cmpstr(r, ==, "Meta Desc");
    g_free(r);

    /* No description meta */
    r = extract_html_description(
        "<meta charset=\"utf-8\">");
    g_assert_null(r);

    /* Entity unescaping in description */
    r = extract_html_description(
        "<meta property=\"og:description\" content=\"a &lt; b\">");
    g_assert_cmpstr(r, ==, "a < b");
    g_free(r);
}

static void test_extract_metadata(void)
{
    gchar *r;

    /* Title only */
    r = extract_metadata("<html><head><title>Test</title></head></html>", 200);
    g_assert_nonnull(r);
    g_assert_nonnull(strstr(r, "Title: Test"));
    g_free(r);

    /* Title + description */
    r = extract_metadata(
        "<html><head>"
        "<title>My Page</title>"
        "<meta name=\"description\" content=\"A great page\">"
        "</head></html>", 300);
    g_assert_nonnull(r);
    g_assert_nonnull(strstr(r, "Title: My Page"));
    g_assert_nonnull(strstr(r, "Desc: A great page"));
    g_free(r);

    /* No title, no description */
    r = extract_metadata("<html><body>just text</body></html>", 100);
    g_assert_null(r);

    /* NULL html */
    r = extract_metadata(NULL, 0);
    g_assert_null(r);

    /* Description truncation (> MAX_DESC_LEN chars) */
    gchar longdesc[300];
    memset(longdesc, 'x', 250);
    longdesc[250] = '\0';
    gchar *html = g_strdup_printf(
        "<html><head><title>T</title>"
        "<meta name=\"description\" content=\"%s\"></head></html>",
        longdesc);
    r = extract_metadata(html, strlen(html));
    g_assert_nonnull(r);
    g_assert_nonnull(strstr(r, "..."));
    g_free(r);
    g_free(html);
}

/* ================================================================== */
/*  Cache helpers                                                       */
/* ================================================================== */

static void test_trim_cache(void)
{
    /* Set up a fresh url_cache */
    url_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    g_hash_table_insert(url_cache, g_strdup("k1"), g_strdup("v1"));
    g_hash_table_insert(url_cache, g_strdup("k2"), g_strdup("v2"));
    g_hash_table_insert(url_cache, g_strdup("k3"), g_strdup("v3"));
    g_assert_cmpuint(g_hash_table_size(url_cache), ==, 3);

    /* Trim to 2 — removes one key */
    trim_cache(2);
    g_assert_cmpuint(g_hash_table_size(url_cache), ==, 2);

    /* Trim to 5 — nothing removed */
    trim_cache(5);
    g_assert_cmpuint(g_hash_table_size(url_cache), ==, 2);

    /* Trim to 0 — remove everything */
    trim_cache(0);
    g_assert_cmpuint(g_hash_table_size(url_cache), ==, 0);

    g_hash_table_destroy(url_cache);
    url_cache = NULL;
}

/* ================================================================== */
/*  Test runner                                                        */
/* ================================================================== */

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Initialise global state (regexes + strophe context) */
    prepare_regexes();
    strophe_ctx = xmpp_ctx_new(NULL, NULL);

    /* Utility functions */
    g_test_add_func("/utility/parse_bool_value",      test_parse_bool_value);
    g_test_add_func("/utility/parse_positive_int",     test_parse_positive_int);
    g_test_add_func("/utility/strv_lowercase_inplace", test_strv_lowercase_inplace);
    g_test_add_func("/utility/strip_trailing_punct",   test_strip_trailing_punct);

    /* Content detection */
    g_test_add_func("/content/content_type_is_html", test_content_type_is_html);
    g_test_add_func("/content/sniff_is_html",        test_sniff_is_html);
    g_test_add_func("/content/is_local_host",         test_is_local_host);

    /* HTML entity handling */
    g_test_add_func("/entity/try_named_entity",  test_try_named_entity);
    g_test_add_func("/entity/try_numeric_entity", test_try_numeric_entity);
    g_test_add_func("/entity/unescape_html",      test_unescape_html);

    /* HTML parsing */
    g_test_add_func("/html/parse_html_attrs",       test_parse_html_attrs);
    g_test_add_func("/html/extract_html_title",     test_extract_html_title);
    g_test_add_func("/html/extract_html_description", test_extract_html_description);
    g_test_add_func("/html/extract_metadata",       test_extract_metadata);

    /* Cache helpers */
    g_test_add_func("/cache/trim_cache", test_trim_cache);

    int ret = g_test_run();

    /* Clean up strophe context */
    xmpp_ctx_free(strophe_ctx);
    strophe_ctx = NULL;

    return ret;
}
