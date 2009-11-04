#include <glib.h>
#include <string.h>

static char **
get_source_files (const char *workdir)
{
        GRegex     *regex;
        GMatchInfo *match;
        GError     *error = NULL;
        char       *path, *data;
        gsize       len;
        int         start, end;
        char      **sources;

        path = g_build_filename (workdir, "Makefile.am", NULL);

        if (!g_file_get_contents (path, &data, &len, &error))
                g_error ("%s", error->message);

        g_free (path);

        regex = g_regex_new ("libuchardet_a_SOURCES =(.*?)\\$\\(NULL\\)",
                             G_REGEX_DOTALL, 0, NULL);

        if (!g_regex_match (regex, data, 0, &match))
          g_assert_not_reached ();

        g_match_info_fetch_pos (match, 1, &start, &end);
        g_match_info_free (match);
        g_regex_unref (regex);

        data[end] = '\0';

        sources = g_regex_split_simple ("[[:space:]\\n\\\\]+",
                                        data + start, 0, 0);

        return sources;
}

static GHashTable *
grab_charsets (void)
{
        GError      *error = NULL;
        GRegex      *regex_changes;
        GRegex      *regex_cleanup;
        GRegex      *regex_string;
        GHashTable  *charset_table;
        char       **sources, **p;
        char        *workdir;
        const char  *srcdir;

        srcdir = g_getenv ("srcdir");
        g_assert (NULL != srcdir);

        workdir = g_build_filename (srcdir, "..", "src", NULL);
        sources = get_source_files (workdir);

        regex_changes = g_regex_new ("#if MAEMO_CHANGES\n(.*?)#else.*?#endif",
                                     G_REGEX_DOTALL, 0, NULL);
        regex_cleanup = g_regex_new ("#if !MAEMO_CHANGES\n(.*?)#(else|endif)|"
                                     "/\\*.*?\\*/|//.*?\n|"
                                     "#\\s*include\\s+\".*?\"|"
                                     "printf\\s*\\(\".*?\""
                                  , G_REGEX_DOTALL, 0, NULL);
        regex_string  = g_regex_new ("\"(.*?)\"", 0, 0, NULL);

        charset_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_free);

        for (p = sources; *p; ++p) {
                char       *path, *data, *tmp;
                gsize       len;
                GMatchInfo *match;

                if ('\0' == *p[0])
                        continue;

                path = g_build_filename (workdir, *p, NULL);

                if (!g_file_get_contents (path, &data, &len, &error))
                        g_error ("%s", error->message);

                g_free (path);

                data = g_regex_replace (regex_changes,
                                        tmp = data, len, 0,
                                        "\\1", 0, NULL);
                g_free (tmp);

                data = g_regex_replace (regex_cleanup,
                                        tmp = data, -1, 0,
                                        "", 0, NULL);
                g_free (tmp);

                if (g_regex_match (regex_string, data, 0, &match)) {
                        do {
                                tmp = g_match_info_fetch (match, 1);
                                g_hash_table_insert (charset_table, tmp,
                                                     g_strdup (*p));
                        } while (g_match_info_next (match, NULL));
                }

                g_match_info_free (match);
                g_free (data);
        }

        g_regex_unref (regex_changes);
        g_regex_unref (regex_cleanup);
        g_regex_unref (regex_string);

        g_strfreev (sources);

        return charset_table;
}

int
main (int    argc,
      char **argv)
{
        int             retval = 0;
        const char     *charset, *filename;
        GHashTable     *charset_table;
        GHashTableIter  iter;
        GIConv          conv;

        charset_table = grab_charsets ();

        g_hash_table_iter_init (&iter, charset_table);

        while (g_hash_table_iter_next (&iter, (gpointer) &charset, (gpointer) &filename)) {
                conv = g_iconv_open ("UTF-8", charset);

                if ((GIConv) -1 == conv) {
                        g_print ("%s: unsupported charset %s\n", filename, charset);
                        retval = 1;
                        continue;
                }

                g_iconv_close (conv);
        }

        g_hash_table_unref (charset_table);

        return retval;
}
