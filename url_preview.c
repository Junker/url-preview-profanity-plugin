// url_preview.c - Profanity C Plugin for URL Preview
//
// Fetches and displays title/description previews for URLs in chat messages.
// Configurable via /url_preview command. Caches results to disk.

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <glib.h>
#include <gio/gio.h>
#include <strophe.h>

#include <profapi.h>

/* ------------------------------------------------------------------ */
/*  Configuration defaults                                            */
/* ------------------------------------------------------------------ */

#define CACHE_FILENAME    "url_preview_cache.ini"
#define DEFAULT_EXTS      ".jpg,.jpeg,.png,.gif,.webp,.bmp,.svg,.tiff," \
                          ".mp4,.mkv,.webm,.avi,.mov,.flv," \
                          ".mp3,.ogg,.wav,.flac,.m4a," \
                          ".pdf,.doc,.docx,.xls,.xlsx,.ppt,.pptx," \
                          ".zip,.rar,.7z,.tar,.gz,.bz2,.xz," \
                          ".exe,.bin,.iso,.dmg,.apk"
#define DEFAULT_DOMAINS   "conversations.im"
#define USER_AGENT        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                          "AppleWebKit/537.36 (KHTML, like Gecko) " \
                          "Chrome/114.0.0.0 Safari/537.36"
#define MAX_HTML_SIZE     16384
#define MAX_DESC_LEN      150
#define DEFAULT_CACHE_SIZE 1000
#define MAX_REDIRECTS     5L

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    gchar    *data;
    gsize     len;
    gboolean  truncated;  /* TRUE once MAX_HTML_SIZE was reached */
} CurlBuffer;

/* Settings group/name constants */
static gchar * const SET_GROUP = "url_preview";

/* ------------------------------------------------------------------ */
/*  Static globals                                                    */
/* ------------------------------------------------------------------ */

static gchar       *cache_path   = NULL;
static GHashTable  *url_cache    = NULL;
static xmpp_ctx_t *strophe_ctx  = NULL;

/* Compiled regexes (initialised once in prof_init) */
static GRegex *re_url;
static GRegex *re_title;
static GRegex *re_meta_tag;
static GRegex *re_attr;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

/* Regex & parsing */
static void        prepare_regexes(void);
static GHashTable *parse_html_attrs(const gchar *attrs);

/* Cache */
static gchar      *get_cache_path(void);
static void        load_cache(void);
static void        save_cache(void);
static gchar      *get_cached_preview(const gchar *url);
static void        put_cache(const gchar *url, const gchar *preview_text, gint limit);
static void        trim_cache(gint limit);

/* Filtering */
static gboolean    is_extension_ignored(const gchar *path);
static gboolean    is_domain_ignored(const gchar *domain);
static gboolean    is_local_host(const gchar *host);

/* Network */
static gchar      *get_url_preview(const gchar *url, gint timeout);

/* HTML extraction */
static gchar      *extract_metadata(const gchar *html, gsize len);
static gchar      *extract_html_title(const gchar *html);
static gchar      *extract_html_description(const gchar *html);
static gchar      *unescape_html(const gchar *text);

/* Message processing */
static gchar      *add_preview_to_message(const gchar *jid, const gchar *message);

/* Command handling */
static void        cmd_url_preview(char **args);

/* Lifecycle */
static void        plugin_cleanup(void);

/* ------------------------------------------------------------------ */
/*  Regex bootstrap                                                   */
/* ------------------------------------------------------------------ */

static void prepare_regexes(void)
{
    re_url = g_regex_new("https?://[^\\s<>\"{}|\\\\^`]+",
                         G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    re_title = g_regex_new("<title[^>]*>(.*?)</title>",
                           G_REGEX_CASELESS | G_REGEX_DOTALL | G_REGEX_OPTIMIZE,
                           0, NULL);
    re_meta_tag = g_regex_new("<meta\\s+([^>]+)>",
                              G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    re_attr = g_regex_new("(\\w[\\w-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')",
                          G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
}

/* ------------------------------------------------------------------ */
/*  Utility helpers                                                   */
/* ------------------------------------------------------------------ */

/** Parse a boolean string (on/true/1/yes or off/false/0/no). */
static gboolean parse_bool_value(const gchar *val, gboolean default_val)
{
    if (!val) return default_val;

    g_autofree gchar *lower = g_utf8_strdown(val, -1);

    if (g_strcmp0(lower, "on") == 0  || g_strcmp0(lower, "true") == 0 ||
        g_strcmp0(lower, "1") == 0   || g_strcmp0(lower, "yes") == 0)
        return TRUE;

    if (g_strcmp0(lower, "off") == 0  || g_strcmp0(lower, "false") == 0 ||
        g_strcmp0(lower, "0") == 0    || g_strcmp0(lower, "no") == 0)
        return FALSE;

    return default_val;
}


/* Printf-style wrapper around prof_log_debug */
#define log_debug(fmt, ...) do { \
        gchar *_ld_msg = g_strdup_printf(fmt, ##__VA_ARGS__); \
        gchar *_ld_msg2 = g_strconcat("url_preview plugin: ", _ld_msg, NULL); \
        prof_log_debug(_ld_msg2); \
        g_free(_ld_msg); \
        g_free(_ld_msg2); \
    } while (0)

/* ------------------------------------------------------------------ */
/*  HTML attribute parser                                             */
/* ------------------------------------------------------------------ */

/**
 * Parse HTML attribute string into a hash table.
 * Keys are lower-cased; values keep original case.
 * Caller unrefs the returned table.
 */
static GHashTable *parse_html_attrs(const gchar *attrs)
{
    GHashTable *ht = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);

    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(re_attr, attrs, 0, &match))
        return ht;

    while (g_match_info_matches(match)) {
        g_autofree gchar *name_raw = g_match_info_fetch(match, 1);

        /* Prefer double-quoted value; fall back to single-quoted */
        gchar *val = g_match_info_fetch(match, 2);
        if (!val)
            val = g_match_info_fetch(match, 3);

        if (val) {
            gchar *lower_name = g_utf8_strdown(name_raw, -1);
            g_hash_table_insert(ht, lower_name, val);  /* takes ownership */
        }

        g_match_info_next(match, NULL);
    }

    return ht;
}

/* ------------------------------------------------------------------ */
/*  Cache persistence                                                 */
/* ------------------------------------------------------------------ */

/** Return (and lazily build) the cache file path. */
static gchar *get_cache_path(void)
{
    if (cache_path)
        return cache_path;

    const gchar *dir = g_get_user_data_dir();
    cache_path = g_build_filename(dir, "profanity", CACHE_FILENAME, NULL);

    gchar *parent = g_path_get_dirname(cache_path);
    g_mkdir_with_parents(parent, 0755);
    g_free(parent);

    return cache_path;
}

static void load_cache(void)
{
    if (url_cache)
        g_hash_table_destroy(url_cache);

    url_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    if (!g_file_test(get_cache_path(), G_FILE_TEST_EXISTS))
        return;

    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, get_cache_path(), G_KEY_FILE_NONE, NULL))
        return;

    gsize n = 0;
    g_auto(GStrv) keys = g_key_file_get_keys(kf, "urls", &n, NULL);
    if (!keys) return;

    for (gsize i = 0; i < n; i++) {
        g_autofree gchar *val =
            g_key_file_get_string(kf, "urls", keys[i], NULL);
        if (val)
            g_hash_table_insert(url_cache,
                                g_strdup(keys[i]),
                                g_steal_pointer(&val));
    }
}

static void save_cache(void)
{
    if (!url_cache) return;

    g_autoptr(GKeyFile) kf = g_key_file_new();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, url_cache);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_key_file_set_string(kf, "urls", key, value);

    g_autoptr(GError) error = NULL;
    g_key_file_save_to_file(kf, get_cache_path(), &error);
    if (error)
        log_debug("%s", error->message);
}

static gchar *get_cached_preview(const gchar *url)
{
    if (!prof_settings_boolean_get(SET_GROUP, "cache", TRUE))
        return NULL;

    return g_hash_table_lookup(url_cache, url);  /* borrowed ref */
}

static void trim_cache(gint limit)
{
    while ((gint)g_hash_table_size(url_cache) > limit) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, url_cache);
        if (g_hash_table_iter_next(&iter, &key, NULL))
            g_hash_table_remove(url_cache, key);
    }
}

static void put_cache(const gchar *url, const gchar *preview_text, gint limit)
{
    if (!prof_settings_boolean_get(SET_GROUP, "cache", TRUE))
        return;
    if (limit <= 0) return;

    g_hash_table_insert(url_cache,
                        g_strdup(url),
                        g_strdup(preview_text ? preview_text : ""));

    trim_cache(limit);

    save_cache();
}

/* ------------------------------------------------------------------ */
/*  Ignored domains & extensions                                      */
/* ------------------------------------------------------------------ */

static gchar **get_ignored_domains(void)
{
    g_autofree gchar *raw = prof_settings_string_get(
        SET_GROUP, "ignored_domains", DEFAULT_DOMAINS);
    return g_strsplit_set(raw, ",", -1);
}

static gchar **get_ignored_exts(void)
{
    g_autofree gchar *raw = prof_settings_string_get(
        SET_GROUP, "ignored_extensions", DEFAULT_EXTS);
    return g_strsplit_set(raw, ",", -1);
}

static gboolean strv_contains_ci(GStrv strv, const gchar *needle)
{
    for (gint i = 0; strv[i]; i++) {
        if (g_strcmp0(needle, strv[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean is_extension_ignored(const gchar *path)
{
    const gchar *dot = strrchr(path, '.');
    if (!dot) return FALSE;

    g_autofree gchar *ext = g_utf8_strdown(dot, -1);
    g_auto(GStrv) ignored = get_ignored_exts();

    return strv_contains_ci(ignored, ext);
}

static gboolean is_domain_ignored(const gchar *domain)
{
    g_auto(GStrv) ignored = get_ignored_domains();
    return strv_contains_ci(ignored, domain);
}

static gboolean is_local_host(const gchar *host)
{
    if (!host) return FALSE;

    /* Check for localhost hostname */
    if (g_str_equal(host, "localhost"))
        return TRUE;

    /* Check for .local mDNS hostnames */
    if (g_str_has_suffix(host, ".local"))
        return TRUE;

    /* Try to parse as an IP address */
    g_autoptr(GInetAddress) addr = g_inet_address_new_from_string(host);
    if (addr) {
        if (g_inet_address_get_is_loopback(addr) ||
            g_inet_address_get_is_site_local(addr) ||
            g_inet_address_get_is_link_local(addr))
            return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  HTTP fetch (libcurl)                                              */
/* ------------------------------------------------------------------ */

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuffer *buf = userdata;
    gsize total = size * nmemb;

    /* Respect download cap: accept what fits and tell curl we consumed it all */
    if (buf->len + total > MAX_HTML_SIZE)
        total = MAX_HTML_SIZE - buf->len;
    if (total == 0) {
        buf->truncated = TRUE;
        return size * nmemb;  /* claim we consumed everything so curl is happy */
    }

    buf->data = g_realloc(buf->data, buf->len + total + 1);
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';

    return total;
}

static gboolean content_type_is_html(const gchar *ctype)
{
    return ctype && g_ascii_strncasecmp(ctype, "text/html", 9) == 0;
}

/** Sniff whether raw data looks like HTML (case-insensitive). */
static gboolean sniff_is_html(const gchar *data)
{
    g_autofree gchar *lower = g_utf8_strdown(data, -1);
    return (strstr(lower, "<html")     != NULL ||
            strstr(lower, "<!doctype") != NULL ||
            strstr(lower, "<head")     != NULL);
}

static gchar *get_url_preview(const gchar *url, gint timeout)
{
    log_debug("getting preview for URL: %s", url);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuffer buf = { .data = g_malloc0(1), .len = 0, .truncated = FALSE };

    struct curl_slist *headers = curl_slist_append(NULL, "Accept: text/html");

    curl_easy_setopt(curl, CURLOPT_URL,             url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,       USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         (long)timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(curl, CURLOPT_ENCODING,        "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);

    CURLcode res = curl_easy_perform(curl);
    gchar *preview = NULL;

    /* Accept data on success or when we truncated at MAX_HTML_SIZE.
     * CURLE_WRITE_ERROR (23) means our callback signalled stop */
    if ((res == CURLE_OK || res == CURLE_WRITE_ERROR) && buf.len > 0) {
        gchar *ctype = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);

        gboolean is_html = content_type_is_html(ctype)
                        || (!ctype && sniff_is_html(buf.data));

        if (is_html)
            preview = extract_metadata(buf.data, buf.len);
    }
    else
        log_debug("curl error %d on url: %s", res, url);

    g_free(buf.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return preview;
}

/* ------------------------------------------------------------------ */
/*  HTML entity unescaping                                            */
/* ------------------------------------------------------------------ */

/** Decode common HTML entities (&amp; &lt; &gt; &quot; &apos; &nbsp;
 *  as well as numeric references &#NNN; and &#xHHH;). */
static gchar *unescape_html(const gchar *text)
{
    if (!text || !*text)
        return g_strdup(text ? text : "");

    GString *out = g_string_sized_new(strlen(text));
    const gchar *p = text;

    while (*p) {
        if (*p != '&') {
            g_string_append_c(out, *p);
            p++;
            continue;
        }

        /* Named entities */
        if (g_str_has_prefix(p, "&amp;"))    { g_string_append_c(out, '&');  p += 5; continue; }
        if (g_str_has_prefix(p, "&lt;"))     { g_string_append_c(out, '<');  p += 4; continue; }
        if (g_str_has_prefix(p, "&gt;"))     { g_string_append_c(out, '>');  p += 4; continue; }
        if (g_str_has_prefix(p, "&quot;"))   { g_string_append_c(out, '"');  p += 6; continue; }
        if (g_str_has_prefix(p, "&apos;"))   { g_string_append_c(out, '\''); p += 6; continue; }
        if (g_str_has_prefix(p, "&#39;"))    { g_string_append_c(out, '\''); p += 5; continue; }
        if (g_str_has_prefix(p, "&nbsp;"))   { g_string_append_c(out, ' ');  p += 6; continue; }

        /* Hex numeric entity  &#xHHH; */
        if (p[1] == '#' && (p[2] == 'x' || p[2] == 'X')) {
            const gchar *semi = strchr(p + 3, ';');
            if (semi) {
                gchar *endptr = NULL;
                gulong val = strtoul(p + 3, &endptr, 16);
                if (endptr == semi && val != 0 && g_unichar_validate((gunichar)val)) {
                    gchar buf[6];
                    gint len = g_unichar_to_utf8((gunichar)val, buf);
                    g_string_append_len(out, buf, len);
                    p = semi + 1;
                    continue;
                }
            }
        }

        /* Decimal numeric entity  &#NNN; */
        if (p[1] == '#') {
            const gchar *semi = strchr(p + 2, ';');
            if (semi) {
                gchar *endptr = NULL;
                gulong val = strtoul(p + 2, &endptr, 10);
                if (endptr == semi && val != 0 && g_unichar_validate((gunichar)val)) {
                    gchar buf[6];
                    gint len = g_unichar_to_utf8((gunichar)val, buf);
                    g_string_append_len(out, buf, len);
                    p = semi + 1;
                    continue;
                }
            }
        }

        /* Not a recognised entity – copy literally */
        g_string_append_c(out, *p);
        p++;
    }

    return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  HTML metadata extraction (GRegex-based, case-insensitive)         */
/* ------------------------------------------------------------------ */

static gchar *extract_html_title(const gchar *html)
{
    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(re_title, html, 0, &match))
        return NULL;

    g_autofree gchar *raw = g_match_info_fetch(match, 1);
    if (!raw) return NULL;

    g_strstrip(raw);
    if (!*raw) return NULL;

    return unescape_html(raw);
}

static gchar *extract_html_description(const gchar *html)
{
    gchar *og_desc  = NULL;
    gchar *meta_desc = NULL;

    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(re_meta_tag, html, 0, &match))
        return NULL;

    do {
        g_autofree gchar *attrs_str = g_match_info_fetch(match, 1);
        g_autoptr(GHashTable) attrs = parse_html_attrs(attrs_str);

        const gchar *prop    = g_hash_table_lookup(attrs, "property");
        const gchar *name    = g_hash_table_lookup(attrs, "name");
        const gchar *content = g_hash_table_lookup(attrs, "content");

        if (!content) continue;

        if (!og_desc && prop &&
            g_ascii_strcasecmp(prop, "og:description") == 0) {
            og_desc = unescape_html(content);
        } else if (!meta_desc && name &&
                   g_ascii_strcasecmp(name, "description") == 0) {
            meta_desc = unescape_html(content);
        }

        if (og_desc && meta_desc) break;

    } while (g_match_info_next(match, NULL));

    if (og_desc) {
        g_free(meta_desc);
        return og_desc;
    }
    return meta_desc;
}

/** Build the preview string from a fetched HTML page. */
static gchar *extract_metadata(const gchar *html, gsize len)
{
    if (!html || len == 0) return NULL;

    g_autofree gchar *title       = extract_html_title(html);
    g_autofree gchar *description = extract_html_description(html);

    GString *out = g_string_new(NULL);

    if (title && *title)
        g_string_append_printf(out, "\n🔗 Title: %s", title);

    if (description && *description) {
        if (strlen(description) > MAX_DESC_LEN)
            g_string_append_printf(out, "\n📝 Desc: %.*s...",
                                   MAX_DESC_LEN - 3, description);
        else
            g_string_append_printf(out, "\n📝 Desc: %s", description);
    }

    return out->len > 0 ? g_string_free(out, FALSE)
                         : (g_string_free(out, TRUE), NULL);
}

/* ------------------------------------------------------------------ */
/*  JID / URL helpers                                                 */
/* ------------------------------------------------------------------ */


/** Strip common trailing punctuation from a URL. */
static void strip_trailing_punct(gchar *url)
{
    gsize len = strlen(url);
    while (len > 0 && strchr(".,;:!>", url[len - 1]))
        url[--len] = '\0';
}

/** Check whether a URL should be skipped for preview.
 *  jid_domain_lower must be a lower-cased domain string. */
static gboolean should_skip_url(const gchar *url, const gchar *jid_domain_lower)
{
    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (!uri) return TRUE;

    const gchar *host = g_uri_get_host(uri);
    const gchar *path = g_uri_get_path(uri);
    if (!host) return TRUE;

    g_autofree gchar *host_lower = g_utf8_strdown(host, -1);
    g_autofree gchar *domain_suffix = g_strconcat(".", jid_domain_lower, NULL);

    /* Skip same domain or subdomain of JID */
    if (g_str_equal(host_lower, jid_domain_lower) ||
        g_str_has_suffix(host_lower, domain_suffix))
        return TRUE;

    /* Skip local/private addresses (loopback, site-local, link-local) */
    if (is_local_host(host_lower))
        return TRUE;

    /* Skip ignored domains */
    if (is_domain_ignored(host_lower))
        return TRUE;

    /* Skip ignored extensions */
    if (path && is_extension_ignored(path))
        return TRUE;

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Core: augment a chat message with a URL preview                   */
/* ------------------------------------------------------------------ */

static gchar *add_preview_to_message(const gchar *jid, const gchar *message)
{
    if (!message || !*message)
        return NULL;

    if (!prof_settings_boolean_get(SET_GROUP, "enable", TRUE))
        return NULL;

    char *jid_domain_raw = xmpp_jid_domain(strophe_ctx, jid);
    if (!jid_domain_raw) return NULL;
    g_autofree gchar *jid_domain = g_utf8_strdown(jid_domain_raw, -1);
    xmpp_free(strophe_ctx, jid_domain_raw);

    gint timeout = prof_settings_int_get(SET_GROUP, "timeout", 2);

    g_autoptr(GMatchInfo) match = NULL;
    if (!g_regex_match(re_url, message, 0, &match))
        return NULL;

    GString *output = g_string_new(message);

    while (g_match_info_matches(match)) {
        g_autofree gchar *url = g_match_info_fetch(match, 0);

        strip_trailing_punct(url);

        if (should_skip_url(url, jid_domain)) {
            g_match_info_next(match, NULL);
            continue;
        }

        /* Try cache first */
        const gchar *cached = get_cached_preview(url);
        if (cached) {
            if (*cached) {
                g_string_append_c(output, '\n');
                g_string_append(output, cached);
                break;   /* preview only first valid URL */
            }
            /* Empty cache entry: known no-preview, try next URL */
            g_match_info_next(match, NULL);
            continue;
        }

        gint cache_size = prof_settings_int_get(SET_GROUP, "cache_size", DEFAULT_CACHE_SIZE);

        /* Fetch and cache */
        g_autofree gchar *preview = get_url_preview(url, timeout);
        if (!preview) {
            put_cache(url, "", cache_size);  /* remember: no preview available */
            g_match_info_next(match, NULL);
            continue;
        }

        put_cache(url, preview, cache_size);
        g_string_append_c(output, '\n');
        g_string_append(output, preview);
        break;   /* preview only first valid URL */
    }

    /* Return result only if we appended a preview */
    gchar *result = NULL;
    if (output->len > strlen(message))
        result = g_string_free(output, FALSE);
    else
        g_string_free(output, TRUE);


    return result;
}

/* ------------------------------------------------------------------ */
/*  /url_preview command sub-handlers                                 */
/* ------------------------------------------------------------------ */

static void cmd_handle_enable(char **args)
{
    if (args[0]) {
        gboolean val = parse_bool_value(args[0], TRUE);
        prof_settings_boolean_set(SET_GROUP, "enable", val);
        prof_cons_show(val ? "URL Preview enabled."
                           : "URL Preview disabled.");
    } else {
        gboolean current = prof_settings_boolean_get(SET_GROUP, "enable", TRUE);
        prof_cons_show(current ? "URL Preview is currently enabled."
                               : "URL Preview is currently disabled.");
    }
}

static void cmd_handle_cache(char **args)
{
    if (args[0]) {
        gboolean val = parse_bool_value(args[0], TRUE);
        prof_settings_boolean_set(SET_GROUP, "cache", val);
        prof_cons_show(val ? "URL Preview cache enabled."
                           : "URL Preview cache disabled.");
    } else {
        gboolean current = prof_settings_boolean_get(SET_GROUP, "cache", TRUE);
        prof_cons_show(current ? "URL Preview cache is currently enabled."
                               : "URL Preview cache is currently disabled.");
    }
}

static void show_current_int(char *key, const gchar *label, gint default_val)
{
    gint current = prof_settings_int_get(SET_GROUP, key, default_val);
    g_autofree gchar *msg = g_strdup_printf("%s is currently %d.", label, current);
    prof_cons_show(msg);
}

static void cmd_handle_cache_size(char **args)
{
    if (args[0]) {
        gchar *endptr = NULL;
        guint64 val = g_ascii_strtoll(args[0], &endptr, 10);
        if (*endptr == '\0' && val > 0) {
            prof_settings_int_set(SET_GROUP, "cache_size", (gint)val);
            g_autofree gchar *msg =
                g_strdup_printf("URL Preview cache size set to %ld.", val);
            prof_cons_show(msg);

            load_cache();
            trim_cache((gint)val);
            save_cache();
        } else {
            prof_cons_show("Cache size must be a positive integer.");
        }
    } else {
        show_current_int("cache_size", "URL Preview cache size", DEFAULT_CACHE_SIZE);
    }
}

static void cmd_handle_timeout(char **args)
{
    if (args[0]) {
        gchar *endptr = NULL;
        guint64 val = g_ascii_strtoll(args[0], &endptr, 10);
        if (*endptr == '\0' && val > 0) {
            prof_settings_int_set(SET_GROUP, "timeout", (gint)val);
            g_autofree gchar *msg =
                g_strdup_printf("URL Preview timeout set to %ld seconds.", val);
            prof_cons_show(msg);
        } else {
            prof_cons_show("Timeout must be a positive integer.");
        }
    } else {
        show_current_int("timeout", "URL Preview timeout", 2);
    }
}

static void cmd_handle_ignored_extensions(char **args)
{
    if (args[0]) {
        /* Join remaining args */
        GString *vs = g_string_new(args[0]);
        for (gint i = 1; args[i]; i++)
            g_string_append_printf(vs, " %s", args[i]);
        g_autofree gchar *val_str = g_string_free(vs, FALSE);

        prof_settings_string_set(SET_GROUP, "ignored_extensions", val_str);
        g_autofree gchar *msg = g_strdup_printf("URL Preview ignored extensions set to: %s", val_str);
        prof_cons_show(msg);
    } else {
        g_autofree gchar *current = prof_settings_string_get(
            SET_GROUP, "ignored_extensions", DEFAULT_EXTS);
        g_autofree gchar *msg =
            g_strdup_printf("URL Preview ignored extensions: %s", current);
        prof_cons_show(msg);
    }
}

static void cmd_handle_ignored_domains(char **args)
{
    if (args[0]) {
        /* Join remaining args */
        GString *vs = g_string_new(args[0]);
        for (gint i = 1; args[i]; i++)
            g_string_append_printf(vs, " %s", args[i]);
        g_autofree gchar *val_str = g_string_free(vs, FALSE);

        prof_settings_string_set(SET_GROUP, "ignored_domains", val_str);
        g_autofree gchar *msg =
            g_strdup_printf("URL Preview ignored domains set to: %s", val_str);
        prof_cons_show(msg);
    } else {
        g_autofree gchar *current = prof_settings_string_get(
            SET_GROUP, "ignored_domains", "");
        if (current && *current) {
            g_autofree gchar *msg = g_strdup_printf("URL Preview ignored domains: %s", current);
            prof_cons_show(msg);
        } else {
            prof_cons_show("URL Preview ignored domains: (none)");
        }
    }
}

static void cmd_handle_status(G_GNUC_UNUSED char **args)
{
    gboolean enable     = prof_settings_boolean_get(SET_GROUP, "enable", TRUE);
    gboolean cache      = prof_settings_boolean_get(SET_GROUP, "cache", TRUE);
    gint     cache_size = prof_settings_int_get(SET_GROUP, "cache_size", DEFAULT_CACHE_SIZE);
    gint     timeout    = prof_settings_int_get(SET_GROUP, "timeout", 2);
    g_autofree gchar *ignored_exts = prof_settings_string_get(
        SET_GROUP, "ignored_extensions", DEFAULT_EXTS);
    g_autofree gchar *ignored_doms = prof_settings_string_get(
        SET_GROUP, "ignored_domains", DEFAULT_DOMAINS);
    gint cached_urls = url_cache ? g_hash_table_size(url_cache) : 0;

    prof_cons_show("URL Preview Settings:");
    prof_cons_show(enable     ? "  Enable:             ON"
                              : "  Enable:             OFF");
    prof_cons_show(cache      ? "  Cache:              ON"
                              : "  Cache:              OFF");
    {
        g_autofree gchar *msg = g_strdup_printf(
            "  Cache size:         %d (%d used)", cache_size, cached_urls);
        prof_cons_show(msg);
    }
    {
        g_autofree gchar *msg = g_strdup_printf(
            "  Timeout:            %d sec", timeout);
        prof_cons_show(msg);
    }
    {
        g_autofree gchar *msg = g_strdup_printf(
            "  Ignored extensions: %s", ignored_exts);
        prof_cons_show(msg);
    }
    {
        g_autofree gchar *msg = g_strdup_printf(
            "  Ignored domains:    %s", ignored_doms ? ignored_doms : "(none)");
        prof_cons_show(msg);
    }
}

/* ------------------------------------------------------------------ */
/*  /url_preview command dispatch                                     */
/* ------------------------------------------------------------------ */

typedef void (*cmd_handler_fn)(char **args);

typedef struct {
    const gchar *name;
    cmd_handler_fn handler;
} CmdDispatch;

static const CmdDispatch cmd_dispatch[] = {
    { "enable",              cmd_handle_enable              },
    { "cache",               cmd_handle_cache               },
    { "cache_size",          cmd_handle_cache_size          },
    { "timeout",             cmd_handle_timeout             },
    { "ignored_extensions",  cmd_handle_ignored_extensions  },
    { "ignored_domains",     cmd_handle_ignored_domains     },
    { "status",              cmd_handle_status              },
    { NULL, NULL }
};

static void cmd_url_preview(char **args)
{
    if (!args[0]) {
        cmd_handle_status(NULL);
        return;
    }

    const gchar *subcmd = args[0];

    for (const CmdDispatch *d = cmd_dispatch; d->name; d++) {
        if (g_str_equal(subcmd, d->name)) {
            d->handler(args + 1);
            return;
        }
    }

    prof_cons_bad_cmd_usage("/url_preview");
}

/* ------------------------------------------------------------------ */
/*  Plugin cleanup (shared by shutdown & unload)                      */
/* ------------------------------------------------------------------ */

static void plugin_cleanup(void)
{
    save_cache();

    g_clear_pointer(&url_cache, g_hash_table_destroy);
    g_clear_pointer(&cache_path, g_free);
    g_clear_pointer(&strophe_ctx, xmpp_ctx_free);
}

/* ------------------------------------------------------------------ */
/*  Profanity hooks                                                   */
/* ------------------------------------------------------------------ */

void prof_init(G_GNUC_UNUSED const char *const version,
               G_GNUC_UNUSED const char *const status,
               G_GNUC_UNUSED const char *const account_name,
               G_GNUC_UNUSED const char *const fulljid)
{
    prof_log_info("url_preview plugin initialized");

    /* Register command */
    gchar *synopsis[] = {
        "/url_preview",
        "/url_preview status",
        "/url_preview enable [on|off]",
        "/url_preview ignored_extensions [list]",
        "/url_preview ignored_domains [list]",
        "/url_preview timeout [seconds]",
        "/url_preview cache [on|off]",
        "/url_preview cache_size [number]",
        NULL
    };

    gchar *description = "Configure URL Preview plugin settings.";

    gchar *arguments[][2] = {
        {"status",              "Show current settings"},
        {"enable",              "Enable or disable the plugin"},
        {"ignored_extensions",  "Comma-separated list of extensions to ignore"},
        {"ignored_domains",     "Comma-separated list of domains to ignore"},
        {"timeout",             "Timeout for HTTP requests in seconds"},
        {"cache",               "Enable or disable the URL preview caching"},
        {"cache_size",          "Maximum number of cached URL previews"},
        {NULL, NULL}
    };

    gchar *examples[] = {
        "/url_preview",
        "/url_preview enable off",
        "/url_preview ignored_extensions .jpg,.png,.gif",
        "/url_preview ignored_domains upload.example.com,share.org",
        "/url_preview timeout 3",
        "/url_preview cache on",
        "/url_preview cache_size 200",
        NULL
    };

    prof_register_command("/url_preview", 0, 10, synopsis, description,
                          arguments, examples, cmd_url_preview);

    /* Register completions */
    gchar *completions[] = {"status", "enable", "ignored_extensions", "ignored_domains",
                            "timeout", "cache", "cache_size", NULL};
    prof_completer_add("/url_preview", completions);

    gchar *on_off[] = {"on", "off", NULL};
    prof_completer_add("/url_preview enable", on_off);
    prof_completer_add("/url_preview cache",  on_off);

    /* Initialise state */
    strophe_ctx = xmpp_ctx_new(NULL, NULL);
    prepare_regexes();
    load_cache();
}

void prof_on_shutdown(void)
{
    plugin_cleanup();
}

void prof_on_unload(void)
{
    log_debug("on_unload");
    plugin_cleanup();
}

char *prof_pre_chat_message_display(const char *barejid,
                                    G_GNUC_UNUSED const char *resource,
                                    const char *message)
{
    return add_preview_to_message(barejid, message);
}

char *prof_pre_room_message_display(const char *barejid,
                                    G_GNUC_UNUSED const char *nick,
                                    const char *message)
{
    return add_preview_to_message(barejid, message);
}

char *prof_pre_priv_message_display(const char *barejid,
                                    G_GNUC_UNUSED const char *nick,
                                    const char *message)
{
    return add_preview_to_message(barejid, message);
}
