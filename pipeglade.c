/*
 * Copyright (c) 2014-2016 Bert Burgemeister <trebbu@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cairo-pdf.h>
#include <cairo-ps.h>
#include <cairo-svg.h>
#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <gtk/gtkunixprint.h>
#include <gtk/gtkx.h>
#include <inttypes.h>
#include <libxml/xpath.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "4.7.0"
#define BUFLEN 256
#define WHITESPACE " \t\n"
#define MAIN_WIN "main"
#define USAGE                                           \
        "usage: pipeglade [[-i in-fifo] "               \
                          "[-o out-fifo] "              \
                          "[-b] "                       \
                          "[-u glade-file.ui] "         \
                          "[-e xid]\n"                  \
        "                  [-l log-file] "              \
                          "[-O err-file] "              \
                          "[--display X-server]] | "    \
                         "[-h |"                        \
                          "-G |"                        \
                          "-V]\n"

#define ABORT                                           \
        {                                               \
                fprintf(stderr,                         \
                        "In %s (%s:%d): ",              \
                        __func__, __FILE__, __LINE__);  \
                abort();                                \
        }

#define OOM_ABORT                                               \
        {                                                       \
                fprintf(stderr,                                 \
                        "Out of memory in %s (%s:%d): ",        \
                        __func__, __FILE__, __LINE__);          \
                abort();                                        \
        }

static FILE *out;               /* UI feedback messages */
static FILE *save;              /* saving user data */
static FILE *log_out;           /* logging output */
static GtkBuilder *builder;     /* to be read from .ui file */


/*
 * ============================================================
 *  Helper functions
 * ============================================================
 */

/*
 * Check if s1 and s2 are equal strings
 */
static bool
eql(const char *s1, const char *s2)
{
        return s1 != NULL && s2 != NULL && strcmp(s1, s2) == 0;
}

/*
 * Print a formatted message to stream s and give up with status
 */
static void
bye(int status, FILE *s, const char *fmt, ...)
{
        va_list ap;

        va_start(ap, fmt);
        vfprintf(s, fmt, ap);
        va_end(ap);
        exit(status);
}

static void
show_lib_versions(void)
{
        bye(EXIT_SUCCESS, stdout,
            "GTK+  v%d.%d.%d (running v%d.%d.%d)\n"
            "cairo v%s (running v%s)\n",
            GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
            gtk_get_major_version(), gtk_get_minor_version(),
            gtk_get_micro_version(),
            CAIRO_VERSION_STRING, cairo_version_string());
}

/*
 * XEmbed us if xid_s is given, or show a standalone window; give up
 * on errors
 */
static void
xembed_if(char *xid_s, GObject *main_window)
{
        GtkWidget *plug, *body;
        Window xid;
        char xid_s2[BUFLEN];

        if (xid_s == NULL) {    /* standalone */
                gtk_widget_show(GTK_WIDGET(main_window));
                return;
        }
        /* We're being XEmbedded */
        xid = strtoul(xid_s, NULL, 10);
        snprintf(xid_s2, BUFLEN, "%lu", xid);
        if (!eql(xid_s, xid_s2))
                bye(EXIT_FAILURE, stderr,
                    "%s is not a valid XEmbed socket id\n", xid_s);
        body = gtk_bin_get_child(GTK_BIN(main_window));
        gtk_container_remove(GTK_CONTAINER(main_window), body);
        plug = gtk_plug_new(xid);
        if (!gtk_plug_get_embedded(GTK_PLUG(plug)))
                bye(EXIT_FAILURE, stderr,
                    "unable to embed into XEmbed socket %s\n", xid_s);
        gtk_container_add(GTK_CONTAINER(plug), body);
        gtk_widget_show(plug);
}

/*
 * If requested, redirect stderr to file name
 */
static void
redirect_stderr(const char *name)
{
        if (name == NULL)
                return;
        if (freopen(name, "a", stderr) == NULL)
                /* complaining on stdout since stderr is closed now */
                bye(EXIT_FAILURE, stdout, "redirecting stderr to %s: %s\n",
                    name, strerror(errno));
        if (fchmod(fileno(stderr), 0600) < 0)
                bye(EXIT_FAILURE, stdout, "setting permissions of %s: %s\n",
                    name, strerror(errno));
                return;
}

/*
 * fork() if requested in bg; give up on errors
 */
static void
go_bg_if(bool bg, FILE *in, FILE *out, char *err_file)
{
        pid_t pid = 0;

        if (!bg)
                return;
        if (in == stdin || out == stdout)
                bye(EXIT_FAILURE, stderr,
                    "parameter -b requires both -i and -o\n");
        pid = fork();
        if (pid < 0)
                bye(EXIT_FAILURE, stderr,
                    "going to background: %s\n", strerror(errno));
        if (pid > 0)
                bye(EXIT_SUCCESS, stdout, "%d\n", pid);
        /* We're the child */
        close(fileno(stdin));   /* making certain not-so-smart     */
        close(fileno(stdout));  /* system/run-shell commands happy */
        if (err_file == NULL)
                freopen("/dev/null", "w", stderr);
}

/*
 * Return the current locale and set it to "C".  Should be free()d if
 * done.
 */
static char *
lc_numeric()
{
        char *lc_orig;
        char *lc = setlocale(LC_NUMERIC, NULL);

        if ((lc_orig = malloc(strlen(lc) + 1)) == NULL)
                OOM_ABORT;
        strcpy(lc_orig, lc);
        setlocale(LC_NUMERIC, "C");
        return lc_orig;
}

/*
 * Set locale (back) to lc; free lc
 */
static void
lc_numeric_free(char *lc)
{
        setlocale(LC_NUMERIC, lc);
        free(lc);
}

/*
 * Print a warning about a malformed command to stderr
 */
static void
ign_cmd(GType type, const char *msg)
{
        const char *name, *pad = " ";

        if (type == G_TYPE_INVALID) {
                name = "";
                pad = "";
        } else
                name = g_type_name(type);
        fprintf(stderr, "ignoring %s%scommand \"%s\"\n", name, pad, msg);
}

/*
 * Check if n is, or can be made, the name of a fifo.  Give up if n
 * exists but is not a fifo.
 */
static void
find_fifo(const char *n)
{
        struct stat sb;

        stat(n, &sb);
        if (S_ISFIFO(sb.st_mode)) {
                if (chmod(n, 0600) != 0)
                        bye(EXIT_FAILURE, stderr, "using pre-existing fifo %s: %s\n",
                            n, strerror(errno));
        } else if (mkfifo(n, 0600) != 0)
                bye(EXIT_FAILURE, stderr, "making fifo %s: %s\n",
                    n, strerror(errno));
}

/*
 * Create a fifo if necessary, and open it.  Give up if the file
 * exists but is not a fifo
 */
static FILE *
open_in_fifo(const char *name)
{
        FILE *s = NULL;
        int fd;

        if (name == NULL)
                s = stdin;
        else {
                find_fifo(name);
                if ((fd = open(name, O_RDWR | O_NONBLOCK)) < 0)
                        bye(EXIT_FAILURE, stderr, "opening fifo %s (r): %s\n",
                            name, strerror(errno));
                if ((s = fdopen(fd, "r")) == NULL)
                        bye(EXIT_FAILURE, stderr, "opening fifo %s (r): %s\n",
                            name, strerror(errno));
        }
        setvbuf(s, NULL, _IONBF, 0);
        return s;
}

static FILE *
open_out_fifo(const char *name)
{
        FILE *s = NULL;

        if (name == NULL)
                s = stdout;
        else {
                find_fifo(name);
                if ((s = fopen(name, "w+")) == NULL)
                        bye(EXIT_FAILURE, stderr, "opening fifo %s (w): %s\n",
                            name, strerror(errno));
        }
        setvbuf(s, NULL, _IOLBF, 0);
        return s;
}

/*
 * Create a log file if necessary, and open it.  A name of "-"
 * requests use of stderr.
 */
static FILE *
open_log(const char *name)
{
        FILE *s = NULL;

        if (name == NULL)
                return NULL;
        if (eql(name, "-"))
                return stderr;
        if ((s = fopen(name, "a")) == NULL)
                bye(EXIT_FAILURE, stderr, "opening log file %s: %s\n",
                    name, strerror(errno));
        if (fchmod(fileno(s), 0600) < 0)
                bye(EXIT_FAILURE, stderr, "setting permissions of %s: %s\n",
                    name, strerror(errno));
        return s;
}

static void
rm_unless(FILE *forbidden, FILE *s, char *name)
{
        if (s == forbidden)
                return;
        fclose(s);
        unlink(name);
}

/*
 * Microseconds elapsed since start
 */
static long int
usec_since(struct timespec *start)
{
        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);
        return (now.tv_sec - start->tv_sec) * 1e6 +
                (now.tv_nsec - start->tv_nsec) / 1e3;
}

/*
 * Write log file
 */
static void
log_msg(char *msg)
{
        static char *old_msg;
        static struct timespec start;

        if (log_out == NULL)    /* no logging */
                return;
        if (msg == NULL && old_msg == NULL)
                fprintf(log_out,
                        "##########\t##### (New Pipeglade session) #####\n");
        else if (msg == NULL && old_msg != NULL) { /* command done; start idle */
                fprintf(log_out,
                        "%10ld\t%s\n", usec_since(&start), old_msg);
                free(old_msg);
                old_msg = NULL;
        } else if (msg != NULL && old_msg == NULL) { /* idle done; start command */
                fprintf(log_out,
                        "%10ld\t### (Idle) ###\n", usec_since(&start));
                if ((old_msg = malloc(strlen(msg) + 1)) == NULL)
                        OOM_ABORT;
                strcpy(old_msg, msg);
        } else
                ABORT;
        clock_gettime(CLOCK_MONOTONIC, &start);
}

static bool
has_suffix(const char *s, const char *suffix)
{
        int s_suf = strlen(s) - strlen(suffix);

        if (s_suf < 0)
                return false;
        return eql(suffix, s + s_suf);
}

/*
 * Remove suffix from name; find the object named like this
 */
static GObject *
obj_sans_suffix(const char *suffix, const char *name)
{
        char str[BUFLEN + 1] = {'\0'};
        int str_l;

        str_l = suffix - name;
        strncpy(str, name, str_l < BUFLEN ? str_l : BUFLEN);
        return gtk_builder_get_object(builder, str);
}

/*
 * Read UI definition from ui_file; give up on errors
 */
static GtkBuilder *
builder_from_file(char *ui_file)
{
        GError *error = NULL;
        GtkBuilder *b;

        b = gtk_builder_new();
        if (gtk_builder_add_from_file(b, ui_file, &error) == 0)
                bye(EXIT_FAILURE, stderr, "%s\n", error->message);
        return b;
}

static const char *
widget_name(GtkBuildable *obj)
{
        return gtk_buildable_get_name(obj);
}

/*
 * Get the main window; give up on errors
 */
static GObject *
find_main_window(void)
{
        GObject *mw;

        if (GTK_IS_WINDOW(mw = gtk_builder_get_object(builder, MAIN_WIN)))
                return mw;
        bye(EXIT_FAILURE, stderr, "no toplevel window named \'" MAIN_WIN "\'\n");
        return NULL;            /* NOT REACHED */
}

/*
 * Store a line from stream s into buf, which should have been malloc'd
 * to bufsize.  Enlarge buf and bufsize if necessary.
 */
static size_t
read_buf(FILE *s, char **buf, size_t *bufsize)
{
        bool esc = false;
        fd_set rfds;
        int c;
        int ifd = fileno(s);
        size_t i = 0;

        FD_ZERO(&rfds);
        FD_SET(ifd, &rfds);
        for (;;) {
                select(ifd + 1, &rfds, NULL, NULL, NULL);
                c = getc(s);
                if (c == '\n' || feof(s))
                        break;
                if (i >= *bufsize - 1)
                        if ((*buf = realloc(*buf, *bufsize *= 2)) == NULL)
                                OOM_ABORT;
                if (esc) {
                        esc = false;
                        switch (c) {
                        case 'n': (*buf)[i++] = '\n'; break;
                        case 'r': (*buf)[i++] = '\r'; break;
                        default: (*buf)[i++] = c; break;
                        }
                } else if (c == '\\')
                        esc = true;
                else
                        (*buf)[i++] = c;
        }
        (*buf)[i] = '\0';
        return i;
}


/*
 * ============================================================
 * Receiving feedback from the GUI
 * ============================================================
 */

static void
send_msg_to(FILE* o, GtkBuildable *obj, const char *tag, va_list ap)
{
        char *data;
        const char *w_name = widget_name(obj);
        fd_set wfds;
        int ofd = fileno(o);
        struct timeval timeout = {1, 0};

        FD_ZERO(&wfds);
        FD_SET(ofd, &wfds);
        if (select(ofd + 1, NULL, &wfds, NULL, &timeout) == 1) {
                fprintf(o, "%s:%s ", w_name, tag);
                while ((data = va_arg(ap, char *)) != NULL) {
                        size_t i = 0;
                        char c;

                        while ((c = data[i++]) != '\0')
                                if (c == '\\')
                                        fprintf(o, "\\\\");
                                else if (c == '\n')
                                        fprintf(o, "\\n");
                                else
                                        putc(c, o);
                }
                putc('\n', o);
        } else
                fprintf(stderr,
                        "send error; discarding feedback message %s:%s\n",
                        w_name, tag);
}

/*
 * Send GUI feedback to global stream "out".  The message format is
 * "<origin>:<tag> <data ...>".  The variadic arguments are strings;
 * last argument must be NULL.
 */
static void
send_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(out, obj, tag, ap);
        va_end(ap);
}

/*
 * Send message from GUI to global stream "save".  The message format
 * is "<origin>:<tag> <data ...>".  The variadic arguments are strings;
 * last argument must be NULL.
 */
static void
save_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(save, obj, tag, ap);
        va_end(ap);
}

/*
 * Send message from GUI to global stream "save".  The message format
 * is "<origin>:set <data ...>".  The variadic arguments are strings;
 * last argument must be NULL.
 */
static void
save_action_set_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(save, obj, "set", ap);
        va_end(ap);
}

/*
 * Use msg_sender() to send a message describing a particular cell
 */
static void
send_tree_cell_msg_by(void msg_sender(GtkBuildable *, const char *, ...),
                      GtkTreeModel *model, const char *path_s,
                      GtkTreeIter *iter, int col, GtkBuildable *obj)
{
        GType col_type;
        GValue value = G_VALUE_INIT;
        char str[BUFLEN], *lc = lc_numeric();

        gtk_tree_model_get_value(model, iter, col, &value);
        col_type = gtk_tree_model_get_column_type(model, col);
        switch (col_type) {
        case G_TYPE_INT:
                snprintf(str, BUFLEN, " %d %d", col, g_value_get_int(&value));
                msg_sender(obj, "gint", path_s, str, NULL);
                break;
        case G_TYPE_LONG:
                snprintf(str, BUFLEN, " %d %ld", col, g_value_get_long(&value));
                msg_sender(obj, "glong", path_s, str, NULL);
                break;
        case G_TYPE_INT64:
                snprintf(str, BUFLEN, " %d %" PRId64, col, g_value_get_int64(&value));
                msg_sender(obj, "gint64", path_s, str, NULL);
                break;
        case G_TYPE_UINT:
                snprintf(str, BUFLEN, " %d %u", col, g_value_get_uint(&value));
                msg_sender(obj, "guint", path_s, str, NULL);
                break;
        case G_TYPE_ULONG:
                snprintf(str, BUFLEN, " %d %lu", col, g_value_get_ulong(&value));
                msg_sender(obj, "gulong", path_s, str, NULL);
                break;
        case G_TYPE_UINT64:
                snprintf(str, BUFLEN, " %d %" PRIu64, col, g_value_get_uint64(&value));
                msg_sender(obj, "guint64", path_s, str, NULL);
                break;
        case G_TYPE_BOOLEAN:
                snprintf(str, BUFLEN, " %d %d", col, g_value_get_boolean(&value));
                msg_sender(obj, "gboolean", path_s, str, NULL);
                break;
        case G_TYPE_FLOAT:
                snprintf(str, BUFLEN, " %d %f", col, g_value_get_float(&value));
                msg_sender(obj, "gfloat", path_s, str, NULL);
                break;
        case G_TYPE_DOUBLE:
                snprintf(str, BUFLEN, " %d %f", col, g_value_get_double(&value));
                msg_sender(obj, "gdouble", path_s, str, NULL);
                break;
        case G_TYPE_STRING:
                snprintf(str, BUFLEN, " %d ", col);
                msg_sender(obj, "gchararray", path_s, str, g_value_get_string(&value), NULL);
                break;
        default:
                fprintf(stderr, "column %d not implemented: %s\n", col, G_VALUE_TYPE_NAME(&value));
                break;
        }
        g_value_unset(&value);
        lc_numeric_free(lc);
}

/*
 * Use msg_sender() to send one message per column for a single row
 */
static void
send_tree_row_msg_by(void msg_sender(GtkBuildable *, const char *, ...),
                     GtkTreeModel *model, char *path_s,
                     GtkTreeIter *iter, GtkBuildable *obj)
{
        int col;

        for (col = 0; col < gtk_tree_model_get_n_columns(model); col++)
                send_tree_cell_msg_by(msg_sender, model, path_s, iter, col, obj);
}

/*
 * send_tree_row_msg serves as an argument for
 * gtk_tree_selection_selected_foreach()
 */
static gboolean
send_tree_row_msg(GtkTreeModel *model,
                  GtkTreePath *path, GtkTreeIter *iter, GtkBuildable *obj)
{
        char *path_s = gtk_tree_path_to_string(path);

        send_tree_row_msg_by(send_msg, model, path_s, iter, obj);
        g_free(path_s);
        return FALSE;
}

/*
 * save_tree_row_msg serves as an argument for
 * gtk_tree_model_foreach().
 * Send message from GUI to global stream "save".
 */
static gboolean
save_tree_row_msg(GtkTreeModel *model,
                  GtkTreePath *path, GtkTreeIter *iter, GtkBuildable *obj)
{
        char *path_s = gtk_tree_path_to_string(path);

        (void) path;
        send_tree_row_msg_by(save_action_set_msg, model, path_s, iter, obj);
        g_free(path_s);
        return FALSE;
}

static void
cb_calendar(GtkBuildable *obj, const char *tag)
{
        char str[BUFLEN];
        unsigned int year = 0, month = 0, day = 0;

        gtk_calendar_get_date(GTK_CALENDAR(obj), &year, &month, &day);
        snprintf(str, BUFLEN, "%04u-%02u-%02u", year, ++month, day);
        send_msg(obj, tag, str, NULL);
}

static void
cb_color_button(GtkBuildable *obj, const char *tag)
{
        GdkRGBA color;

        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &color);
        send_msg(obj, tag, gdk_rgba_to_string(&color), NULL);
}

static void
cb_editable(GtkBuildable *obj, const char *tag)
{
        send_msg(obj, tag, gtk_entry_get_text(GTK_ENTRY(obj)), NULL);
}

/*
 * Callback that sends a message about a pointer device button press
 * in a GtkEventBox
 */
static bool
cb_event_box_button(GtkBuildable *obj, GdkEvent *e, gpointer user_data)
{
        char data[BUFLEN], *lc = lc_numeric();

        snprintf(data, BUFLEN, "%d %.1lf %.1lf",
                 e->button.button, e->button.x, e->button.y);
        send_msg(obj, user_data, data, NULL);
        lc_numeric_free(lc);
        return true;
}

/*
 * Callback that sends in a message the name of the key pressed when
 * a GtkEventBox is focused
 */
static bool
cb_event_box_key(GtkBuildable *obj, GdkEvent *e, gpointer user_data)
{
        send_msg(obj, user_data, gdk_keyval_name(e->key.keyval), NULL);
        return true;
}

/*
 * Callback that sends a message about pointer device motion in a
 * GtkEventBox
 */
static bool
cb_event_box_motion(GtkBuildable *obj, GdkEvent *e, gpointer user_data)
{
        char data[BUFLEN], *lc = lc_numeric();

        snprintf(data, BUFLEN, "%.1lf %.1lf", e->button.x, e->button.y);
        send_msg(obj, user_data, data, NULL);
        lc_numeric_free(lc);
        return true;
}

/*
 * Callback that only sends "name:tag" and returns false
 */
static bool
cb_event_simple(GtkBuildable *obj, GdkEvent *e, const char *tag)
{
        (void) e;
        send_msg(obj, tag, NULL);
        return false;
}

static void
cb_file_chooser_button(GtkBuildable *obj, const char *tag)
{
        send_msg(obj, tag, gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(obj)), NULL);
}

static void
cb_font_button(GtkBuildable *obj, const char *tag)
{
        send_msg(obj, tag, gtk_font_button_get_font_name(GTK_FONT_BUTTON(obj)), NULL);
}

static void
cb_menu_item(GtkBuildable *obj, const char *tag)
{
        send_msg(obj, tag, gtk_menu_item_get_label(GTK_MENU_ITEM(obj)), NULL);
}

static void
cb_range(GtkBuildable *obj, const char *tag)
{
        char str[BUFLEN], *lc = lc_numeric();

        snprintf(str, BUFLEN, "%f", gtk_range_get_value(GTK_RANGE(obj)));
        send_msg(obj, tag, str, NULL);
        lc_numeric_free(lc);
}

/*
 * Callback that sends user's selection from a file dialog
 */
static void
cb_send_file_chooser_dialog_selection(gpointer user_data)
{
        send_msg(user_data, "file",
                 gtk_file_chooser_get_filename(user_data), NULL);
        send_msg(user_data, "folder",
                 gtk_file_chooser_get_current_folder(user_data), NULL);
}

/*
 * Callback that sends in a message the content of the text buffer
 * passed in user_data
 */
static void
cb_send_text(GtkBuildable *obj, gpointer user_data)
{
        GtkTextIter a, b;

        gtk_text_buffer_get_bounds(user_data, &a, &b);
        send_msg(obj, "text", gtk_text_buffer_get_text(user_data, &a, &b, TRUE), NULL);
}

/*
 * Callback that sends in a message the highlighted text from the text
 * buffer which was passed in user_data
 */
static void
cb_send_text_selection(GtkBuildable *obj, gpointer user_data)
{
        GtkTextIter a, b;

        gtk_text_buffer_get_selection_bounds(user_data, &a, &b);
        send_msg(obj, "text", gtk_text_buffer_get_text(user_data, &a, &b, TRUE), NULL);
}

/*
 * Callback that only sends "name:tag" and returns true
 */
static bool
cb_simple(GtkBuildable *obj, const char *tag)
{
        send_msg(obj, tag, NULL);
        return true;
}

static void
cb_spin_button(GtkBuildable *obj, const char *tag)
{
        char str[BUFLEN], *lc = lc_numeric();

        snprintf(str, BUFLEN, "%f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(obj)));
        send_msg(obj, tag, str, NULL);
        lc_numeric_free(lc);
}

static void
cb_switch(GtkBuildable *obj, void *pspec, void *user_data)
{
        (void) pspec;
        (void) user_data;
        send_msg(obj, gtk_switch_get_active(GTK_SWITCH(obj)) ? "1" : "0", NULL);
}

static void
cb_toggle_button(GtkBuildable *obj, const char *tag)
{
        (void) tag;
        send_msg(obj, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(obj)) ? "1" : "0", NULL);
}

static void
cb_tree_selection(GtkBuildable *obj, const char *tag)
{
        GtkTreeSelection *sel = GTK_TREE_SELECTION(obj);
        GtkTreeView *view = gtk_tree_selection_get_tree_view(sel);


        send_msg(GTK_BUILDABLE(view), tag, NULL);
        gtk_tree_selection_selected_foreach(
                sel, (GtkTreeSelectionForeachFunc) send_tree_row_msg, view);
}


/*
 * ============================================================
 *  cb_draw() maintains a drawing on a GtkDrawingArea; it needs a few
 *  helper functions
 * ============================================================
 */

/*
 * The set of supported drawing operations
 */
enum cairo_fn {
        ARC,
        ARC_NEGATIVE,
        CLOSE_PATH,
        CURVE_TO,
        FILL,
        FILL_PRESERVE,
        LINE_TO,
        MOVE_TO,
        RECTANGLE,
        REL_CURVE_TO,
        REL_LINE_TO,
        REL_MOVE_TO,
        REL_MOVE_FOR,
        RESET_CTM,
        SET_DASH,
        SET_FONT_FACE,
        SET_FONT_SIZE,
        SET_LINE_CAP,
        SET_LINE_JOIN,
        SET_LINE_WIDTH,
        SET_SOURCE_RGBA,
        SHOW_TEXT,
        STROKE,
        STROKE_PRESERVE,
        TRANSFORM,
};

/*
 * Text placement mode for rel_move_for()
 */
enum ref_point {
        C,
        E,
        N,
        NE,
        NW,
        S,
        SE,
        SW,
        W,
};

enum draw_op_policy {
        APPEND,
        BEFORE,
        REPLACE,
};

/*
 * One single element of a drawing
 */
struct draw_op {
        struct draw_op *next;
        struct draw_op *prev;
        unsigned long long int id;
        unsigned long long int before;
        enum draw_op_policy policy;
        enum cairo_fn op;
        void *op_args;
};

/*
 * Argument sets for the various drawing operations
 */
struct arc_args {
        double x;
        double y;
        double radius;
        double angle1;
        double angle2;
};

struct curve_to_args {
        double x1;
        double y1;
        double x2;
        double y2;
        double x3;
        double y3;
};

struct move_to_args {
        double x;
        double y;
};

struct rectangle_args {
        double x;
        double y;
        double width;
        double height;
};

struct rel_move_for_args {
        enum ref_point ref;
        int len;
        char text[];
};

struct set_dash_args {
        int num_dashes;
        double dashes[];
};

struct set_font_face_args {
        cairo_font_slant_t slant;
        cairo_font_weight_t weight;
        char family[];
};

struct set_font_size_args {
        double size;
};

struct set_line_cap_args {
        cairo_line_cap_t line_cap;
};

struct set_line_join_args {
        cairo_line_join_t line_join;
};

struct set_line_width_args {
        double width;
};

struct set_source_rgba_args {
        GdkRGBA color;
};

struct show_text_args {
        int len;
        char text[];
};

struct transform_args {
        cairo_matrix_t matrix;
};

static void
draw(cairo_t *cr, enum cairo_fn op, void *op_args)
{
        switch (op) {
        case LINE_TO: {
                struct move_to_args *args = op_args;

                cairo_line_to(cr, args->x, args->y);
                break;
        }
        case REL_LINE_TO: {
                struct move_to_args *args = op_args;

                cairo_rel_line_to(cr, args->x, args->y);
                break;
        }
        case MOVE_TO: {
                struct move_to_args *args = op_args;

                cairo_move_to(cr, args->x, args->y);
                break;
        }
        case REL_MOVE_TO: {
                struct move_to_args *args = op_args;

                cairo_rel_move_to(cr, args->x, args->y);
                break;
        }
        case ARC: {
                struct arc_args *args = op_args;

                cairo_arc(cr, args->x, args->y, args->radius, args->angle1, args->angle2);
                break;
        }
        case ARC_NEGATIVE: {
                struct arc_args *args = op_args;

                cairo_arc_negative(cr, args->x, args->y, args->radius, args->angle1, args->angle2);
                break;
        }
        case CURVE_TO: {
                struct curve_to_args *args = op_args;

                cairo_curve_to(cr, args->x1, args->y1, args->x2, args->y2, args->x3, args->y3);
                break;
        }
        case REL_CURVE_TO: {
                struct curve_to_args *args = op_args;

                cairo_curve_to(cr, args->x1, args->y1, args->x2, args->y2, args->x3, args->y3);
                break;
        }
        case RECTANGLE: {
                struct rectangle_args *args = op_args;

                cairo_rectangle(cr, args->x, args->y, args->width, args->height);
                break;
        }
        case CLOSE_PATH:
                cairo_close_path(cr);
                break;
        case SHOW_TEXT: {
                struct show_text_args *args = op_args;

                cairo_show_text(cr, args->text);
                break;
        }
        case REL_MOVE_FOR: {
                cairo_text_extents_t e;
                double dx = 0.0, dy = 0.0;
                struct rel_move_for_args *args = op_args;

                cairo_text_extents(cr, args->text, &e);
                switch (args->ref) {
                case C: dx = -e.width / 2; dy = e.height / 2; break;
                case E: dx = -e.width; dy = e.height / 2; break;
                case N: dx = -e.width / 2; dy = e.height; break;
                case NE: dx = -e.width; dy = e.height; break;
                case NW: dy = e.height; break;
                case S: dx = -e.width / 2; break;
                case SE: dx = -e.width; break;
                case SW: break;
                case W: dy = e.height / 2; break;
                default: ABORT; break;
                }
                cairo_rel_move_to(cr, dx, dy);
                break;
        }
        case RESET_CTM:
                cairo_identity_matrix(cr);
                break;
        case STROKE:
                cairo_stroke(cr);
                break;
        case STROKE_PRESERVE:
                cairo_stroke_preserve(cr);
                break;
        case FILL:
                cairo_fill(cr);
                break;
        case FILL_PRESERVE:
                cairo_fill_preserve(cr);
                break;
        case SET_DASH: {
                struct set_dash_args *args = op_args;

                cairo_set_dash(cr, args->dashes, args->num_dashes, 0);
                break;
        }
        case SET_FONT_FACE: {
                struct set_font_face_args *args = op_args;

                cairo_select_font_face(cr, args->family, args->slant, args->weight);
                break;
        }
        case SET_FONT_SIZE: {
                struct set_font_size_args *args = op_args;

                cairo_set_font_size(cr, args->size);
                break;
        }
        case SET_LINE_CAP: {
                struct set_line_cap_args *args = op_args;

                cairo_set_line_cap(cr, args->line_cap);
                break;
        }
        case SET_LINE_JOIN: {
                struct set_line_join_args *args = op_args;

                cairo_set_line_join(cr, args->line_join);
                break;
        }
        case SET_LINE_WIDTH: {
                struct set_line_width_args *args = op_args;

                cairo_set_line_width(cr, args->width);
                break;
        }
        case SET_SOURCE_RGBA: {
                struct set_source_rgba_args *args = op_args;

                gdk_cairo_set_source_rgba(cr, &args->color);
                break;
        }
        case TRANSFORM: {
                struct transform_args *args = op_args;

                cairo_transform(cr, &args->matrix);
                break;
        }
        default:
                ABORT;
                break;
        }
}

/*
 * Callback that draws on a GtkDrawingArea
 */
static gboolean
cb_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
        struct draw_op *op;

        (void) data;
        for (op = g_object_get_data(G_OBJECT(widget), "draw_ops");
             op != NULL;
             op = op->next)
                draw(cr, op->op, op->op_args);
        return FALSE;
}


/*
 * ============================================================
 *  Manipulating the GUI
 * ============================================================
 */

static void
update_button(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(obj), data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_calendar(GObject *obj, const char *action,
                const char *data, const char *whole_msg, GType type)
{
        GtkCalendar *calendar = GTK_CALENDAR(obj);
        char dummy;
        int year = 0, month = 0, day = 0;

        if (eql(action, "select_date") &&
            sscanf(data, "%d-%d-%d %c", &year, &month, &day, &dummy) == 3) {
                if (month > -1 && month <= 11 && day > 0 && day <= 31) {
                        gtk_calendar_select_month(calendar, --month, year);
                        gtk_calendar_select_day(calendar, day);
                } else
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "mark_day") &&
                   sscanf(data, "%d %c", &day, &dummy) == 1) {
                if (day > 0 && day <= 31)
                        gtk_calendar_mark_day(calendar, day);
                else
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "clear_marks") && sscanf(data, " %c", &dummy) < 1)
                gtk_calendar_clear_marks(calendar);
        else
                ign_cmd(type, whole_msg);
}

/*
 * Common actions for various kinds of window.  Return false if
 * command is ignored
 */
static bool
update_class_window(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GtkWindow *window = GTK_WINDOW(obj);
        char dummy;
        int x, y;

        (void) type;
        (void) whole_msg;
        if (eql(action, "set_title"))
                gtk_window_set_title(window, data);
        else if (eql(action, "fullscreen") && sscanf(data, " %c", &dummy) < 1)
                gtk_window_fullscreen(window);
        else if (eql(action, "unfullscreen") && sscanf(data, " %c", &dummy) < 1)
                gtk_window_unfullscreen(window);
        else if (eql(action, "resize") &&
                 sscanf(data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_window_resize(window, x, y);
        else if (eql(action, "resize") && sscanf(data, " %c", &dummy) < 1) {
                gtk_window_get_default_size(window, &x, &y);
                gtk_window_resize(window, x, y);
        } else if (eql(action, "move") &&
                   sscanf(data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_window_move(window, x, y);
        else
                return false;
        return true;
}

static void
update_color_button(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GdkRGBA color;

        if (eql(action, "set_color")) {
                gdk_rgba_parse(&color, data);
                gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(obj), &color);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_combo_box_text(GObject *obj, const char *action,
                      const char *data, const char *whole_msg, GType type)
{
        GtkComboBoxText *combobox = GTK_COMBO_BOX_TEXT(obj);
        char data1[strlen(data) + 1];
        char dummy;
        int val;

        strcpy(data1, data);
        if (eql(action, "prepend_text"))
                gtk_combo_box_text_prepend_text(combobox, data1);
        else if (eql(action, "append_text"))
                gtk_combo_box_text_append_text(combobox, data1);
        else if (eql(action, "remove") && sscanf(data, "%d %c", &val, &dummy) == 1)
                gtk_combo_box_text_remove(combobox, strtol(data1, NULL, 10));
        else if (eql(action, "insert_text")) {
                char *position = strtok(data1, WHITESPACE);
                char *text = strtok(NULL, WHITESPACE);

                gtk_combo_box_text_insert_text(combobox,
                                               strtol(position, NULL, 10), text);
        } else
                ign_cmd(type, whole_msg);
}

/*
 * Maintaining a list of drawing operations.  It is the responsibility
 * of cb_draw() to actually draw them.  update_drawing_area() needs a
 * few helper functions.
 */

enum draw_op_stat {
        FAILURE,
        SUCCESS,
        NEED_REDRAW,
};

/*
 * Fill structure *op with the drawing operation according to action
 * and with the appropriate set of arguments
 */
static enum draw_op_stat
set_draw_op(struct draw_op *op, const char *action, const char *data)
{
        char dummy;
        const char *raw_args = data;
        enum draw_op_stat result = SUCCESS;
        int args_start = 0;

        if (sscanf(data, "=%llu %n", &op->id, &args_start) == 1) {
                op->policy = REPLACE;
                result = NEED_REDRAW;
        } else if (sscanf(data, "%llu<%llu %n", &op->id, &op->before, &args_start) == 2) {
                op->policy = BEFORE;
                result = NEED_REDRAW;
        } else if (sscanf(data, "%llu %n", &op->id, &args_start) == 1)
                op->policy = APPEND;
        else
                return FAILURE;
        raw_args += args_start;
        if (eql(action, "line_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = LINE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &args->x, &args->y, &dummy) != 2)
                        return FAILURE;
        } else if (eql(action, "rel_line_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_LINE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &args->x, &args->y, &dummy) != 2)
                        return FAILURE;
        } else if (eql(action, "move_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = MOVE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &args->x, &args->y, &dummy) != 2)
                        return FAILURE;
        } else if (eql(action, "rel_move_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_MOVE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &args->x, &args->y, &dummy) != 2)
                        return FAILURE;
        } else if (eql(action, "arc")) {
                struct arc_args *args;
                double deg1, deg2;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = ARC;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %lf %lf %lf %c",
                           &args->x, &args->y, &args->radius, &deg1, &deg2, &dummy) != 5)
                        return FAILURE;
                args->angle1 = deg1 * (M_PI / 180.L);
                args->angle2 = deg2 * (M_PI / 180.L);
        } else if (eql(action, "arc_negative")) {
                double deg1, deg2;
                struct arc_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = ARC_NEGATIVE;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %lf %lf %lf %c",
                           &args->x, &args->y, &args->radius, &deg1, &deg2, &dummy) != 5)
                        return FAILURE;
                args->angle1 = deg1 * (M_PI / 180.L);
                args->angle2 = deg2 * (M_PI / 180.L);
        } else if (eql(action, "curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = CURVE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %lf %lf %lf %lf %c",
                           &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3, &dummy) != 6)
                        return FAILURE;
        } else if (eql(action, "rel_curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_CURVE_TO;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %lf %lf %lf %lf %c",
                           &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3, &dummy) != 6)
                        return FAILURE;
        } else if (eql(action, "rectangle")) {
                struct rectangle_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = RECTANGLE;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %lf %lf %c",
                           &args->x, &args->y, &args->width, &args->height, &dummy) != 4)
                        return FAILURE;
        } else if (eql(action, "close_path")) {
                op->op = CLOSE_PATH;
                if (sscanf(raw_args, " %c", &dummy) > 0)
                        return FAILURE;
                op->op_args = NULL;
        } else if (eql(action, "show_text")) {
                struct show_text_args *args;
                int len;

                len = strlen(raw_args) + 1;
                if ((args = malloc(sizeof(*args) + len * sizeof(args->text[0]))) == NULL)
                        OOM_ABORT;
                op->op = SHOW_TEXT;
                op->op_args = args;
                args->len = len; /* not used */
                strncpy(args->text, raw_args, len);
                result = NEED_REDRAW;
        } else if (eql(action, "rel_move_for")) {
                char ref_point[2 + 1];
                int start, len;
                struct rel_move_for_args *args;

                if (sscanf(raw_args, "%2s %n", ref_point, &start) < 1)
                        return FAILURE;
                len = strlen(raw_args + start) + 1;
                if ((args = malloc(sizeof(*args) + len * sizeof(args->text[0]))) == NULL)
                        OOM_ABORT;
                if (eql(ref_point, "c"))
                        args->ref = C;
                else if (eql(ref_point, "e"))
                        args->ref = E;
                else if (eql(ref_point, "n"))
                        args->ref = N;
                else if (eql(ref_point, "ne"))
                        args->ref = NE;
                else if (eql(ref_point, "nw"))
                        args->ref = NW;
                else if (eql(ref_point, "s"))
                        args->ref = S;
                else if (eql(ref_point, "se"))
                        args->ref = SE;
                else if (eql(ref_point, "sw"))
                        args->ref = SW;
                else if (eql(ref_point, "w"))
                        args->ref = W;
                else
                        return FAILURE;
                op->op = REL_MOVE_FOR;
                op->op_args = args;
                args->len = len; /* not used */
                strncpy(args->text, (raw_args + start), len);
        } else if (eql(action, "stroke")) {
                op->op = STROKE;
                if (sscanf(raw_args, " %c", &dummy) > 0)
                        return FAILURE;
                op->op_args = NULL;
                result = NEED_REDRAW;
        } else if (eql(action, "stroke_preserve")) {
                op->op = STROKE_PRESERVE;
                if (sscanf(raw_args, " %c", &dummy) > 0)
                        return FAILURE;
                op->op_args = NULL;
                result = NEED_REDRAW;
        } else if (eql(action, "fill")) {
                op->op = FILL;
                if (sscanf(raw_args, " %c", &dummy) > 0)
                        return FAILURE;
                op->op_args = NULL;
                result = NEED_REDRAW;
        } else if (eql(action, "fill_preserve")) {
                op->op = FILL_PRESERVE;
                if (sscanf(raw_args, " %c", &dummy) > 0)
                        return FAILURE;
                op->op_args = NULL;
                result = NEED_REDRAW;
        } else if (eql(action, "set_dash")) {
                char *next, *end;
                char data1[strlen(raw_args) + 1];
                int n, i;
                struct set_dash_args *args;

                strcpy(data1, raw_args);
                next = end = data1;
                n = -1;
                do {
                        n++;
                        next = end;
                        strtod(next, &end);
                } while (next != end);
                if ((args = malloc(sizeof(*args) + n * sizeof(args->dashes[0]))) == NULL)
                        OOM_ABORT;
                op->op = SET_DASH;
                op->op_args = args;
                args->num_dashes = n;
                for (i = 0, next = data1; i < n; i++, next = end) {
                        args->dashes[i] = strtod(next, &end);
                }
        } else if (eql(action, "set_font_face")) {
                char slant[7 + 1];  /* "oblique" */
                char weight[6 + 1]; /* "normal" */
                int family_start, family_len;
                struct set_font_face_args *args;

                if (sscanf(raw_args, "%7s %6s %n%*s", slant, weight, &family_start) != 2)
                        return FAILURE;
                family_len = strlen(raw_args + family_start) + 1;
                if ((args = malloc(sizeof(*args) + family_len * sizeof(args->family[0]))) == NULL)
                        OOM_ABORT;
                op->op = SET_FONT_FACE;
                op->op_args = args;
                strncpy(args->family, raw_args + family_start, family_len);
                if (eql(slant, "normal"))
                        args->slant = CAIRO_FONT_SLANT_NORMAL;
                else if (eql(slant, "italic"))
                        args->slant = CAIRO_FONT_SLANT_ITALIC;
                else if (eql(slant, "oblique"))
                        args->slant = CAIRO_FONT_SLANT_OBLIQUE;
                else
                        return FAILURE;
                if (eql(weight, "normal"))
                        args->weight = CAIRO_FONT_WEIGHT_NORMAL;
                else if (eql(weight, "bold"))
                        args->weight = CAIRO_FONT_WEIGHT_BOLD;
                else
                        return FAILURE;
        } else if (eql(action, "set_font_size")) {
                struct set_font_size_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_FONT_SIZE;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %c", &args->size, &dummy) != 1)
                        return FAILURE;
        } else if (eql(action, "set_line_cap")) {
                char str[6 + 1]; /* "square" */
                struct set_line_cap_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_CAP;
                op->op_args = args;
                if (sscanf(raw_args, "%6s %c", str, &dummy) != 1)
                        return FAILURE;
                if (eql(str, "butt"))
                        args->line_cap = CAIRO_LINE_CAP_BUTT;
                else if (eql(str, "round"))
                        args->line_cap = CAIRO_LINE_CAP_ROUND;
                else if (eql(str, "square"))
                        args->line_cap = CAIRO_LINE_CAP_SQUARE;
                else
                        return FAILURE;
        } else if (eql(action, "set_line_join")) {
                char str[5 + 1]; /* "miter" */
                struct set_line_join_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_JOIN;
                op->op_args = args;
                if (sscanf(raw_args, "%5s %c", str, &dummy) != 1)
                        return FAILURE;
                if (eql(str, "miter"))
                        args->line_join = CAIRO_LINE_JOIN_MITER;
                else if (eql(str, "round"))
                        args->line_join = CAIRO_LINE_JOIN_ROUND;
                else if (eql(str, "bevel"))
                        args->line_join = CAIRO_LINE_JOIN_BEVEL;
                else
                        return FAILURE;
        } else if (eql(action, "set_line_width")) {
                struct set_line_width_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_WIDTH;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %c", &args->width, &dummy) != 1)
                        return FAILURE;
        } else if (eql(action, "set_source_rgba")) {
                struct set_source_rgba_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_SOURCE_RGBA;
                op->op_args = args;
                gdk_rgba_parse(&args->color, raw_args);
        } else if (eql(action, "transform")) {
                char dummy;
                double xx, yx, xy, yy, x0, y0;

                if (sscanf(raw_args, "%lf %lf %lf %lf %lf %lf %c",
                           &xx, &yx, &xy, &yy, &x0, &y0, &dummy) == 6) {
                        struct transform_args *args;

                        if ((args = malloc(sizeof(*args))) == NULL)
                                OOM_ABORT;
                        op->op_args = args;
                        op->op = TRANSFORM;
                        cairo_matrix_init(&args->matrix, xx, yx, xy, yy, x0, y0);
                } else if (sscanf(raw_args, " %c", &dummy) < 1) {
                        op->op = RESET_CTM;
                        op->op_args = NULL;
                } else
                        return FAILURE;
        } else if (eql(action, "translate")) {
                double tx, ty;
                struct transform_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = TRANSFORM;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &tx, &ty, &dummy) != 2)
                        return FAILURE;
                cairo_matrix_init_translate(&args->matrix, tx, ty);
        } else if (eql(action, "scale")) {
                double sx, sy;
                struct transform_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = TRANSFORM;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %lf %c", &sx, &sy, &dummy) != 2)
                        return FAILURE;
                cairo_matrix_init_scale(&args->matrix, sx, sy);
        } else if (eql(action, "rotate")) {
                double angle;
                struct transform_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = TRANSFORM;
                op->op_args = args;
                if (sscanf(raw_args, "%lf %c", &angle, &dummy) != 1)
                        return FAILURE;
                cairo_matrix_init_rotate(&args->matrix, angle * (M_PI / 180.L));
        } else
                return FAILURE;
        return result;
}

/*
 * Add another element to widget's "draw_ops" list
 */
static enum draw_op_stat
ins_draw_op(GObject *widget, const char *action, const char *data)
{
        enum draw_op_stat result;
        struct draw_op *new_op = NULL, *draw_ops = NULL, *prev_op = NULL;

        if ((new_op = malloc(sizeof(*new_op))) == NULL)
                OOM_ABORT;
        new_op->op_args = NULL;
        new_op->next = NULL;
        if ((result = set_draw_op(new_op, action, data)) == FAILURE) {
                free(new_op->op_args);
                free(new_op);
                return FAILURE;
        }
        switch (new_op->policy) {
        case APPEND:
                if ((draw_ops = g_object_get_data(widget, "draw_ops")) == NULL)
                        g_object_set_data(widget, "draw_ops", new_op);
                else {
                        for (prev_op = draw_ops;
                             prev_op->next != NULL;
                             prev_op = prev_op->next);
                        prev_op->next = new_op;
                }
                break;
        case BEFORE:
                for (prev_op = NULL, draw_ops = g_object_get_data(widget, "draw_ops");
                     draw_ops != NULL && draw_ops->id != new_op->before;
                     prev_op = draw_ops, draw_ops = draw_ops->next);
                if (prev_op == NULL) { /* prepend a new first element */
                        g_object_set_data(widget, "draw_ops", new_op);
                        new_op->next = draw_ops;
                } else if (draw_ops == NULL) /* append */
                        prev_op->next = new_op;
                else {          /* insert */
                        new_op->next = draw_ops;
                        prev_op->next = new_op;
                }
                break;
        case REPLACE:
                for (prev_op = NULL, draw_ops = g_object_get_data(widget, "draw_ops");
                     draw_ops != NULL && draw_ops->id != new_op->id;
                     prev_op = draw_ops, draw_ops = draw_ops->next);
                if (draw_ops == NULL && prev_op == NULL) /* start a new list */
                        g_object_set_data(widget, "draw_ops", new_op);
                else if (prev_op == NULL) { /* replace the first element */
                        g_object_set_data(widget, "draw_ops", new_op);
                        new_op->next = draw_ops->next;
                        free(draw_ops->op_args);
                        free(draw_ops);
                } else if (draw_ops == NULL) /* append */
                        prev_op->next = new_op;
                else {          /* replace some other element */
                        new_op->next = draw_ops->next;
                        prev_op->next = new_op;
                        free(draw_ops->op_args);
                        free(draw_ops);
                }
                break;
        default:
                ABORT;
                break;
        }
        return result;
}

/*
 * Remove all elements with the given id from widget's "draw_ops" list
 */
static enum draw_op_stat
rem_draw_op(GObject *widget, const char *data)
{
        char dummy;
        struct draw_op *op, *next_op, *prev_op = NULL;
        unsigned long long int id;

        if (sscanf(data, "%llu %c", &id, &dummy) != 1)
                return FAILURE;
        op = g_object_get_data(widget, "draw_ops");
        while (op != NULL) {
                next_op = op->next;
                if (op->id == id) {
                        if (prev_op == NULL) /* list head */
                                g_object_set_data(widget, "draw_ops", op->next);
                        else
                                prev_op->next = op->next;
                        free(op->op_args);
                        free(op);
                } else
                        prev_op = op;
                op = next_op;
        }
        return NEED_REDRAW;
}

static gboolean
refresh_widget(GtkWidget *widget)
{
        gint height = gtk_widget_get_allocated_height(widget);
        gint width = gtk_widget_get_allocated_width(widget);

        gtk_widget_queue_draw_area(widget, 0, 0, width, height);
        return G_SOURCE_REMOVE;
}

/*
 * Write the drawing from the GtkDrawingArea widget in an appropriate
 * format to file
 */
static enum draw_op_stat
save_drawing(GtkWidget *widget, const char *fn)
{
        cairo_surface_t *sur;
        cairo_t *cr;
        int height = gtk_widget_get_allocated_height(widget);
        int width = gtk_widget_get_allocated_width(widget);

        if (has_suffix(fn, ".epsf") || has_suffix(fn, ".eps")) {
                sur = cairo_ps_surface_create(fn, width, height);
                cairo_ps_surface_set_eps(sur, TRUE);
        } else if (has_suffix(fn, ".pdf"))
                sur = cairo_pdf_surface_create(fn, width, height);
        else if (has_suffix(fn, ".ps"))
                sur = cairo_ps_surface_create(fn, width, height);
        else if (has_suffix(fn, ".svg"))
                sur = cairo_svg_surface_create(fn, width, height);
        else
                return FAILURE;
        cr = cairo_create(sur);
        cb_draw(widget, cr, NULL);
        cairo_destroy(cr);
        cairo_surface_destroy(sur);
        return SUCCESS;
}

static void
update_drawing_area(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        enum draw_op_stat dost;

        if (eql(action, "remove"))
                dost = rem_draw_op(obj, data);
        else if (eql(action, "save"))
                dost = save_drawing(GTK_WIDGET(obj), data);
        else
                dost = ins_draw_op(obj, action, data);
        switch (dost) {
        case NEED_REDRAW:
                gdk_threads_add_idle_full(G_PRIORITY_LOW,
                                          (GSourceFunc) refresh_widget,
                                          GTK_WIDGET(obj), NULL);
                break;
        case FAILURE:
                ign_cmd(type, whole_msg);
                break;
        case SUCCESS:
                break;
        default:
                ABORT;
                break;
        }
}

static void
update_entry(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        GtkEntry *entry = GTK_ENTRY(obj);

        if (eql(action, "set_text"))
                gtk_entry_set_text(entry, data);
        else if (eql(action, "set_placeholder_text"))
                gtk_entry_set_placeholder_text(entry, data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_expander(GObject *obj, const char *action,
                const char *data, const char *whole_msg, GType type)
{
        GtkExpander *expander = GTK_EXPANDER(obj);
        char dummy;
        unsigned int val;

        if (eql(action, "set_expanded") &&
            sscanf(data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_expander_set_expanded(expander, val);
        else if (eql(action, "set_label"))
                gtk_expander_set_label(expander, data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_file_chooser_button(GObject *obj, const char *action,
                           const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_filename"))
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(obj), data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_file_chooser_dialog(GObject *obj, const char *action,
                           const char *data, const char *whole_msg, GType type)
{
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(obj);

        if (eql(action, "set_filename"))
                gtk_file_chooser_set_filename(chooser, data);
        else if (eql(action, "set_current_name"))
                gtk_file_chooser_set_current_name(chooser, data);
        else if (update_class_window(obj, action, data, whole_msg, type));
        else
                ign_cmd(type, whole_msg);
}

static void
update_focus(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        char dummy;

        (void) action;
        (void) data;
        if (sscanf(data, " %c", &dummy) < 1 &&
            gtk_widget_get_can_focus(GTK_WIDGET(obj)))
                gtk_widget_grab_focus(GTK_WIDGET(obj));
        else
                ign_cmd(type, whole_msg);
}

static void
update_font_button(GObject *obj, const char *action,
                   const char *data, const char *whole_msg, GType type)
{
        GtkFontButton *font_button = GTK_FONT_BUTTON(obj);

        if (eql(action, "set_font_name"))
                gtk_font_button_set_font_name(font_button, data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_frame(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_label"))
                gtk_frame_set_label(GTK_FRAME(obj), data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_image(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        GtkIconSize size;
        GtkImage *image = GTK_IMAGE(obj);

        gtk_image_get_icon_name(image, NULL, &size);
        if (eql(action, "set_from_file"))
                gtk_image_set_from_file(image, data);
        else if (eql(action, "set_from_icon_name"))
                gtk_image_set_from_icon_name(image, data, size);
        else
                ign_cmd(type, whole_msg);
}

static void
update_label(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_text"))
                gtk_label_set_text(GTK_LABEL(obj), data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_notebook(GObject *obj, const char *action,
                const char *data, const char *whole_msg, GType type)
{
        char dummy;
        unsigned int val;

        if (eql(action, "set_current_page") &&
            sscanf(data, "%u %c", &val, &dummy) == 1)
                gtk_notebook_set_current_page(GTK_NOTEBOOK(obj), val);
        else
                ign_cmd(type, whole_msg);
}

static void
update_nothing(GObject *obj, const char *action,
               const char *data, const char *whole_msg, GType type)
{
        (void) obj;
        (void) action;
        (void) data;
        (void) whole_msg;
        (void) type;
}

static void
update_print_dialog(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GtkPageSetup *page_setup;
        GtkPrintJob *job;
        GtkPrintSettings *settings;
        GtkPrintUnixDialog *dialog = GTK_PRINT_UNIX_DIALOG(obj);
        GtkPrinter *printer;
        gint response_id;

        if (eql(action, "print")) {
                response_id = gtk_dialog_run(GTK_DIALOG(dialog));
                switch (response_id) {
                case GTK_RESPONSE_OK:
                        printer = gtk_print_unix_dialog_get_selected_printer(dialog);
                        settings = gtk_print_unix_dialog_get_settings(dialog);
                        page_setup = gtk_print_unix_dialog_get_page_setup(dialog);
                        job = gtk_print_job_new(data, printer, settings, page_setup);
                        if (gtk_print_job_set_source_file(job, data, NULL))
                                gtk_print_job_send(job, NULL, NULL, NULL);
                        else
                                ign_cmd(type, whole_msg);
                        g_clear_object(&settings);
                        g_clear_object(&job);
                        break;
                case GTK_RESPONSE_CANCEL:
                case GTK_RESPONSE_DELETE_EVENT:
                        break;
                default:
                        fprintf(stderr, "%s sent an unexpected response id (%d)\n",
                                widget_name(GTK_BUILDABLE(dialog)), response_id);
                        break;
                }
                gtk_widget_hide(GTK_WIDGET(dialog));
        } else
                ign_cmd(type, whole_msg);
}

static void
update_progress_bar(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GtkProgressBar *progressbar = GTK_PROGRESS_BAR(obj);
        char dummy;
        double frac;

        if (eql(action, "set_text"))
                gtk_progress_bar_set_text(progressbar, *data == '\0' ? NULL : data);
        else if (eql(action, "set_fraction") &&
                 sscanf(data, "%lf %c", &frac, &dummy) == 1)
                gtk_progress_bar_set_fraction(progressbar, frac);
        else
                ign_cmd(type, whole_msg);
}

static void
update_scale(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        GtkRange *range = GTK_RANGE(obj);
        char dummy;
        double val1, val2;

        if (eql(action, "set_value") && sscanf(data, "%lf %c", &val1, &dummy) == 1)
                gtk_range_set_value(range, val1);
        else if (eql(action, "set_fill_level") &&
                 sscanf(data, "%lf %c", &val1, &dummy) == 1) {
                gtk_range_set_fill_level(range, val1);
                gtk_range_set_show_fill_level(range, TRUE);
        } else if (eql(action, "set_fill_level") &&
                   sscanf(data, " %c", &dummy) < 1)
                gtk_range_set_show_fill_level(range, FALSE);
        else if (eql(action, "set_range") &&
                 sscanf(data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_range_set_range(range, val1, val2);
        else if (eql(action, "set_increments") &&
                 sscanf(data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_range_set_increments(range, val1, val2);
        else
                ign_cmd(type, whole_msg);
}

static void
update_scrolled_window(GObject *obj, const char *action,
                       const char *data, const char *whole_msg, GType type)
{
        GtkScrolledWindow *window = GTK_SCROLLED_WINDOW(obj);
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(window);
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(window);
        char dummy;
        double d0, d1;

        if (eql(action, "hscroll") && sscanf(data, "%lf %c", &d0, &dummy) == 1)
                gtk_adjustment_set_value(hadj, d0);
        else if (eql(action, "vscroll") && sscanf(data, "%lf %c", &d0, &dummy) == 1)
                gtk_adjustment_set_value(vadj, d0);
        else if (eql(action, "hscroll_to_range") &&
                 sscanf(data, "%lf %lf %c", &d0, &d1, &dummy) == 2)
                gtk_adjustment_clamp_page(hadj, d0, d1);
        else if (eql(action, "vscroll_to_range") &&
                 sscanf(data, "%lf %lf %c", &d0, &d1, &dummy) == 2)
                gtk_adjustment_clamp_page(vadj, d0, d1);
        else
                ign_cmd(type, whole_msg);
}

static void
update_sensitivity(GObject *obj, const char *action,
                   const char *data, const char *whole_msg, GType type)
{
        char dummy;
        unsigned int val;

        (void) action;
        (void) whole_msg;
        (void) type;
        if (sscanf(data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_widget_set_sensitive(GTK_WIDGET(obj), val);
        else
                ign_cmd(type, whole_msg);
}

static void
update_size_request(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        char dummy;
        int x, y;

        (void) action;
        (void) whole_msg;
        (void) type;
        if (sscanf(data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_widget_set_size_request(GTK_WIDGET(obj), x, y);
        else if (sscanf(data, " %c", &dummy) < 1)
                gtk_widget_set_size_request(GTK_WIDGET(obj), -1, -1);
        else
                ign_cmd(type, whole_msg);
}

static void
update_socket(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkSocket *socket = GTK_SOCKET(obj);
        Window id;
        char str[BUFLEN], dummy;

        (void) data;
        if (eql(action, "id") && sscanf(data, " %c", &dummy) < 1) {
                id = gtk_socket_get_id(socket);
                snprintf(str, BUFLEN, "%lu", id);
                send_msg(GTK_BUILDABLE(socket), "id", str, NULL);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_spin_button(GObject *obj, const char *action,
                   const char *data, const char *whole_msg, GType type)
{
        GtkSpinButton *spinbutton = GTK_SPIN_BUTTON(obj);
        char dummy;
        double val1, val2;

        if (eql(action, "set_text") && /* TODO: rename to "set_value" */
            sscanf(data, "%lf %c", &val1, &dummy) == 1)
                gtk_spin_button_set_value(spinbutton, val1);
        else if (eql(action, "set_range") &&
                 sscanf(data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_spin_button_set_range(spinbutton, val1, val2);
        else if (eql(action, "set_increments") &&
                 sscanf(data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_spin_button_set_increments(spinbutton, val1, val2);
        else
                ign_cmd(type, whole_msg);
}

static void
update_spinner(GObject *obj, const char *action,
               const char *data, const char *whole_msg, GType type)
{
        GtkSpinner *spinner = GTK_SPINNER(obj);
        char dummy;

        (void) data;
        if (eql(action, "start") && sscanf(data, " %c", &dummy) < 1)
                gtk_spinner_start(spinner);
        else if (eql(action, "stop") && sscanf(data, " %c", &dummy) < 1)
                gtk_spinner_stop(spinner);
        else
                ign_cmd(type, whole_msg);
}

static void
update_statusbar(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        GtkStatusbar *statusbar = GTK_STATUSBAR(obj);
        char *ctx_msg, dummy;
        const char *msg;
        int ctx_len, t;

        /* TODO: remove "push", "pop", "remove_all"; rename "push_id" to "push", etc. */
        if ((ctx_msg = malloc(strlen(data) + 1)) == NULL)
                OOM_ABORT;
        t = sscanf(data, "%s %n%c", ctx_msg, &ctx_len, &dummy);
        msg = data + ctx_len;
        if (eql(action, "push"))
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, "0"),
                                   data);
        else if (eql(action, "push_id") && t >= 1)
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, ctx_msg),
                                   msg);
        else if (eql(action, "pop") && t < 1)
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(action, "pop_id") && t == 1)
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, ctx_msg));
        else if (eql(action, "remove_all") && t < 1)
                gtk_statusbar_remove_all(statusbar,
                                         gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(action, "remove_all_id") && t == 1)
                gtk_statusbar_remove_all(statusbar,
                                         gtk_statusbar_get_context_id(statusbar, ctx_msg));
        else
                ign_cmd(type, whole_msg);
        free(ctx_msg);
}

static void
update_switch(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        char dummy;
        unsigned int val;

        if (eql(action, "set_active") &&
            sscanf(data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_switch_set_active(GTK_SWITCH(obj), val);
        else
                ign_cmd(type, whole_msg);
}

static void
update_text_view(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        GtkTextView *view = GTK_TEXT_VIEW(obj);
        GtkTextBuffer *textbuf = gtk_text_view_get_buffer(view);
        GtkTextIter a, b;
        char dummy;
        int val;

        if (eql(action, "set_text"))
                gtk_text_buffer_set_text(textbuf, data, -1);
        else if (eql(action, "delete") && sscanf(data, " %c", &dummy) < 1) {
                gtk_text_buffer_get_bounds(textbuf, &a, &b);
                gtk_text_buffer_delete(textbuf, &a, &b);
        } else if (eql(action, "insert_at_cursor"))
                gtk_text_buffer_insert_at_cursor(textbuf, data, -1);
        else if (eql(action, "place_cursor") && eql(data, "end")) {
                gtk_text_buffer_get_end_iter(textbuf, &a);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "place_cursor") &&
                   sscanf(data, "%d %c", &val, &dummy) == 1) {
                gtk_text_buffer_get_iter_at_offset(textbuf, &a, val);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "place_cursor_at_line") &&
                   sscanf(data, "%d %c", &val, &dummy) == 1) {
                gtk_text_buffer_get_iter_at_line(textbuf, &a, val);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "scroll_to_cursor") &&
                   sscanf(data, " %c", &dummy) < 1)
                gtk_text_view_scroll_to_mark(view, gtk_text_buffer_get_insert(textbuf),
                                             0., 0, 0., 0.);
        else if (eql(action, "save") && data != NULL &&
                 (save = fopen(data, "w")) != NULL) {
                gtk_text_buffer_get_bounds(textbuf, &a, &b);
                save_msg(GTK_BUILDABLE(view), "insert_at_cursor",
                         gtk_text_buffer_get_text(textbuf, &a, &b, TRUE), NULL);
                fclose(save);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_toggle_button(GObject *obj, const char *action,
                     const char *data, const char *whole_msg, GType type)
{
        char dummy;
        unsigned int val;

        if (eql(action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(obj), data);
        else if (eql(action, "set_active") &&
                 sscanf(data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(obj), val);
        else
                ign_cmd(type, whole_msg);
}

static void
update_tooltip_text(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        (void) action;
        (void) whole_msg;
        (void) type;
        gtk_widget_set_tooltip_text(GTK_WIDGET(obj), data);
}

/*
 * update_tree_view() needs a few helper functions
 */

/*
 * Check if s is a valid string representation of a GtkTreePath
 */
static bool
is_path_string(char *s)
{
        return s != NULL &&
                strlen(s) == strspn(s, ":0123456789") &&
                strstr(s, "::") == NULL &&
                strcspn(s, ":") > 0;
}

static void
tree_model_insert_before(GtkTreeModel *model, GtkTreeIter *iter,
                         GtkTreeIter *parent, GtkTreeIter *sibling)
{
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_insert_before(GTK_TREE_STORE(model),
                                             iter, parent, sibling);
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_insert_before(GTK_LIST_STORE(model),
                                             iter, sibling);
        else
                ABORT;
}

static void
tree_model_insert_after(GtkTreeModel *model, GtkTreeIter *iter,
                        GtkTreeIter *parent, GtkTreeIter *sibling)
{
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_insert_after(GTK_TREE_STORE(model),
                                            iter, parent, sibling);
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_insert_after(GTK_LIST_STORE(model),
                                            iter, sibling);
        else
                ABORT;
}

static void
tree_model_move_before(GtkTreeModel *model, GtkTreeIter *iter,
                       GtkTreeIter *position)
{
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_move_before(GTK_TREE_STORE(model), iter, position);
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_move_before(GTK_LIST_STORE(model), iter, position);
        else
                ABORT;
}

static void
tree_model_remove(GtkTreeModel *model, GtkTreeIter *iter)
{
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_remove(GTK_LIST_STORE(model), iter);
        else
                ABORT;
}

static void
tree_model_clear(GtkTreeModel *model)
{
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_clear(GTK_TREE_STORE(model));
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_clear(GTK_LIST_STORE(model));
        else
                ABORT;
}

static void
tree_model_set(GtkTreeModel *model, GtkTreeIter *iter, ...)
{
        va_list ap;

        va_start(ap, iter);
        if (GTK_IS_TREE_STORE(model))
                gtk_tree_store_set_valist(GTK_TREE_STORE(model), iter, ap);
        else if (GTK_IS_LIST_STORE(model))
                gtk_list_store_set_valist(GTK_LIST_STORE(model), iter, ap);
        else
                ABORT;
        va_end(ap);
}

/*
 * Create an empty row at path if it doesn't yet exist.  Create older
 * siblings and parents as necessary.
 */
static void
create_subtree(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter)
{
        GtkTreeIter iter_1;     /* iter's predecessor */
        GtkTreePath *path_1;    /* path's predecessor */

        if (gtk_tree_model_get_iter(model, iter, path))
                return;
        path_1 = gtk_tree_path_copy(path);
        if (gtk_tree_path_prev(path_1)) { /* need an older sibling */
                create_subtree(model, path_1, iter);
                iter_1 = *iter;
                tree_model_insert_after(model, iter, NULL, &iter_1);
        } else if (gtk_tree_path_up(path_1)) { /* need a parent */
                create_subtree(model, path_1, iter);
                if (gtk_tree_path_get_depth(path_1) == 0)
                        /* first toplevel row */
                        tree_model_insert_after(model, iter, NULL, NULL);
                else {          /* first row in a lower level */
                        iter_1 = *iter;
                        tree_model_insert_after(model, iter, &iter_1, NULL);
                }
        } /* neither prev nor up mean we're at the root of an empty tree */
        gtk_tree_path_free(path_1);
}

static bool
set_tree_view_cell(GtkTreeModel *model, GtkTreeIter *iter,
                   const char *path_s, int col, const char *new_text)
{
        GType col_type = gtk_tree_model_get_column_type(model, col);
        GtkTreePath *path;
        bool ok = false;
        char *endptr;
        double d;
        long long int n;

        path = gtk_tree_path_new_from_string(path_s);
        create_subtree(model, path, iter);
        gtk_tree_path_free(path);
        switch (col_type) {
        case G_TYPE_BOOLEAN:
        case G_TYPE_INT:
        case G_TYPE_LONG:
        case G_TYPE_INT64:
        case G_TYPE_UINT:
        case G_TYPE_ULONG:
        case G_TYPE_UINT64:
                errno = 0;
                endptr = NULL;
                n = strtoll(new_text, &endptr, 10);
                if (!errno && endptr != new_text) {
                        tree_model_set(model, iter, col, n, -1);
                        ok = true;
                }
                break;
        case G_TYPE_FLOAT:
        case G_TYPE_DOUBLE:
                errno = 0;
                endptr = NULL;
                d = strtod(new_text, &endptr);
                if (!errno && endptr != new_text) {
                        tree_model_set(model, iter, col, d, -1);
                        ok = true;
                }
                break;
        case G_TYPE_STRING:
                tree_model_set(model, iter, col, new_text, -1);
                ok = true;
                break;
        default:
                fprintf(stderr, "column %d: %s not implemented\n",
                        col, g_type_name(col_type));
                ok = true;
                break;
        }
        return ok;
}

static void
tree_view_set_cursor(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col)
{
        /* GTK+ 3.14 requires this.  For 3.18, path = NULL */
        /* is just fine and this function need not exist. */
        if (path == NULL)
                path = gtk_tree_path_new();
        gtk_tree_view_set_cursor(view, path, col, false);
}

static void
update_tree_view(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        GtkTreeView *view = GTK_TREE_VIEW(obj);
        GtkTreeIter iter0, iter1;
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        GtkTreePath *path = NULL;
        bool iter0_valid, iter1_valid;
        char *tokens, *arg0, *arg1, *arg2;
        int col = -1;           /* invalid column number */

        if (!GTK_IS_LIST_STORE(model) && !GTK_IS_TREE_STORE(model))
        {
                fprintf(stderr, "missing model/");
                ign_cmd(type, whole_msg);
                return;
        }
        if ((tokens = malloc(strlen(data) + 1)) == NULL)
                OOM_ABORT;
        strcpy(tokens, data);
        arg0 = strtok(tokens, WHITESPACE);
        arg1 = strtok(NULL, WHITESPACE);
        arg2 = strtok(NULL, "");
        iter0_valid = is_path_string(arg0) &&
                gtk_tree_model_get_iter_from_string(model, &iter0, arg0);
        iter1_valid = is_path_string(arg1) &&
                gtk_tree_model_get_iter_from_string(model, &iter1, arg1);
        if (is_path_string(arg1))
                col = strtol(arg1, NULL, 10);
        if (eql(action, "set") &&
            col > -1 &&
            col < gtk_tree_model_get_n_columns(model) &&
            is_path_string(arg0)) {
                if (set_tree_view_cell(model, &iter0, arg0, col, arg2) == false)
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "scroll") && iter0_valid && iter1_valid &&
                   arg2 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_scroll_to_cell (view,
                                              path,
                                              gtk_tree_view_get_column(view, col),
                                              0, 0., 0.);
        } else if (eql(action, "expand") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, false);
        } else if (eql(action, "expand_all") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, true);
        } else if (eql(action, "expand_all") && arg0 == NULL)
                gtk_tree_view_expand_all(view);
        else if (eql(action, "collapse") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_collapse_row(view, path);
        } else if (eql(action, "collapse") && arg0 == NULL)
                gtk_tree_view_collapse_all(view);
        else if (eql(action, "set_cursor") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                tree_view_set_cursor(view, path, NULL);
        } else if (eql(action, "set_cursor") && arg0 == NULL) {
                tree_view_set_cursor(view, NULL, NULL);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
        } else if (eql(action, "insert_row") &&
                   eql(arg0, "end") && arg1 == NULL)
                tree_model_insert_before(model, &iter1, NULL, NULL);
        else if (eql(action, "insert_row") && iter0_valid &&
                 eql(arg1, "as_child") && arg2 == NULL)
                tree_model_insert_after(model, &iter1, &iter0, NULL);
        else if (eql(action, "insert_row") && iter0_valid && arg1 == NULL)
                tree_model_insert_before(model, &iter1, NULL, &iter0);
        else if (eql(action, "move_row") && iter0_valid &&
                 eql(arg1, "end") && arg2 == NULL)
                tree_model_move_before(model, &iter0, NULL);
        else if (eql(action, "move_row") && iter0_valid && iter1_valid && arg2 == NULL)
                tree_model_move_before(model, &iter0, &iter1);
        else if (eql(action, "remove_row") && iter0_valid && arg1 == NULL)
                tree_model_remove(model, &iter0);
        else if (eql(action, "clear") && arg0 == NULL) {
                tree_view_set_cursor(view, NULL, NULL);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
                tree_model_clear(model);
        } else if (eql(action, "save") && arg0 != NULL &&
                   (save = fopen(arg0, "w")) != NULL) {
                gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc) save_tree_row_msg, view);
                fclose(save);
        } else
                ign_cmd(type, whole_msg);
        free(tokens);
        gtk_tree_path_free(path);
}

static void
update_visibility(GObject *obj, const char *action,
                  const char *data, const char *whole_msg, GType type)
{
        char dummy;
        unsigned int val;

        (void) action;
        (void) whole_msg;
        (void) type;
        if (sscanf(data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_widget_set_visible(GTK_WIDGET(obj), val);
        else
                ign_cmd(type, whole_msg);
}

/*
 * Change the style of the widget passed
 */
static void
update_widget_style(GObject *obj, const char *name,
                    const char *data, const char *whole_msg, GType type)
{
        GtkStyleContext *context;
        GtkStyleProvider *style_provider;
        char *style_decl;
        const char *prefix = "* {", *suffix = "}";
        size_t sz;

        (void) name;
        (void) whole_msg;
        (void) type;
        style_provider = g_object_get_data(obj, "style_provider");
        sz = strlen(prefix) + strlen(suffix) + strlen(data) + 1;
        context = gtk_widget_get_style_context(GTK_WIDGET(obj));
        gtk_style_context_remove_provider(context, style_provider);
        if ((style_decl = malloc(sz)) == NULL)
                OOM_ABORT;
        strcpy(style_decl, prefix);
        strcat(style_decl, data);
        strcat(style_decl, suffix);
        gtk_style_context_add_provider(context, style_provider,
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_data(GTK_CSS_PROVIDER(style_provider),
                                        style_decl, -1, NULL);
        free(style_decl);
}

static void
update_window(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        if (!update_class_window(obj, action, data, whole_msg, type))
                ign_cmd(type, whole_msg);
}

/*
 * Simulate user activity on various widgets
 */
static void
fake_ui_activity(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        char dummy;

        (void) action;
        (void) data;
        if (!GTK_IS_WIDGET(obj) || sscanf(data, " %c", &dummy) > 0)
                ign_cmd(type, whole_msg);
        else if (GTK_IS_SPIN_BUTTON(obj))
                cb_spin_button(GTK_BUILDABLE(obj), "text"); /* TODO: rename to "value" */
        else if (GTK_IS_SCALE(obj))
                cb_range(GTK_BUILDABLE(obj), "value");
        else if (GTK_IS_ENTRY(obj))
                cb_editable(GTK_BUILDABLE(obj), "text");
        else if (GTK_IS_CALENDAR(obj))
                cb_calendar(GTK_BUILDABLE(obj), "clicked");
        else if (GTK_IS_FILE_CHOOSER_BUTTON(obj))
                cb_file_chooser_button(GTK_BUILDABLE(obj), "file");
        else if (!gtk_widget_activate(GTK_WIDGET(obj)))
                ign_cmd(type, whole_msg);
}

/*
 * The final UI update
 */
static void
main_quit(GObject *obj, const char *action,
          const char *data, const char *whole_msg, GType type)
{
        char dummy;

        (void) obj;
        (void) action;
        (void) data;
        (void) whole_msg;
        (void) type;
        if (sscanf(data, " %c", &dummy) < 1)
                gtk_main_quit();
        else
                ign_cmd(type, whole_msg);
}

/*
 * Don't update anything; just complain
 */
static void
complain(GObject *obj, const char *action,
         const char *data, const char *whole_msg, GType type)
{
        (void) obj;
        (void) action;
        (void) data;
        ign_cmd(type, whole_msg);
}

/*
 * Data to be passed to and from the GTK main loop
 */
struct ui_data {
        void (*fn)(GObject *, const char *action,
                   const char *data, const char *msg, GType type);
        GObject *obj;
        char *action;
        char *data;
        char *msg;
        char *msg_tokens;
        GType type;
};

/*
 * Parse command pointed to by ud, and act on ui accordingly.  Runs
 * once per command inside gtk_main_loop().
 */
static gboolean
update_ui(struct ui_data *ud)
{
        char *lc = lc_numeric();

        (ud->fn)(ud->obj, ud->action, ud->data, ud->msg, ud->type);
        free(ud->msg_tokens);
        free(ud->msg);
        free(ud);
        lc_numeric_free(lc);
        return G_SOURCE_REMOVE;
}

/*
 * Keep track of loading files to avoid recursive loading of the same
 * file.  If filename = NULL, forget the most recently remembered file.
 */
static bool
remember_loading_file(char *filename)
{
        static char *filenames[BUFLEN];
        static size_t latest = 0;
        size_t i;

        if (filename == NULL) {  /* pop */
                if (latest < 1)
                        ABORT;
                latest--;
                return false;
        } else {                /* push */
                for (i = 1; i <= latest; i++)
                        if (eql(filename, filenames[i]))
                                return false;
                if (latest > BUFLEN -2)
                        return false;
                filenames[++latest] = filename;
                return true;
        }
}

/*
 * Read lines from stream cmd and perform appropriate actions on the
 * GUI
 */
static void *
digest_msg(FILE *cmd)
{
        FILE *load;             /* restoring user data */
        char *name;
        static int recursion = -1; /* > 0 means this is a recursive call */

        recursion++;
        for (;;) {
                struct ui_data *ud;
                char first_char = '\0';
                size_t msg_size = 32;
                int name_start = 0, name_end = 0;
                int action_start = 0, action_end = 0;
                int data_start;

                if (feof(cmd))
                        break;
                if ((ud = malloc(sizeof(*ud))) == NULL)
                        OOM_ABORT;
                if ((ud->msg = malloc(msg_size)) == NULL)
                        OOM_ABORT;
                ud->type = G_TYPE_INVALID;
                pthread_testcancel();
                if (recursion == 0)
                        log_msg(NULL);
                read_buf(cmd, &ud->msg, &msg_size);
                if (recursion == 0)
                        log_msg(ud->msg);
                data_start = strlen(ud->msg);
                if ((ud->msg_tokens = malloc(strlen(ud->msg) + 1)) == NULL)
                        OOM_ABORT;
                strcpy(ud->msg_tokens, ud->msg);
                sscanf(ud->msg, " %c", &first_char);
                if (strlen(ud->msg) == 0 || first_char == '#') { /* comment */
                        ud->fn = update_nothing;
                        goto exec;
                }
                sscanf(ud->msg_tokens,
                       " %n%*[0-9a-zA-Z_]%n:%n%*[0-9a-zA-Z_]%n%*1[ \t]%n",
                       &name_start, &name_end, &action_start, &action_end, &data_start);
                ud->msg_tokens[name_end] = ud->msg_tokens[action_end] = '\0';
                name = ud->msg_tokens + name_start;
                ud->action = ud->msg_tokens + action_start;
                ud->data = ud->msg_tokens + data_start;
                if (eql(ud->action, "main_quit")) {
                        ud->fn = main_quit;
                        goto exec;
                }
                if (eql(ud->action, "load") && strlen(ud->data) > 0 &&
                    (load = fopen(ud->data, "r")) != NULL &&
                    remember_loading_file(ud->data)) {
                        digest_msg(load);
                        fclose(load);
                        remember_loading_file(NULL);
                        ud->fn = update_nothing;
                        goto exec;
                }
                if ((ud->obj = (gtk_builder_get_object(builder, name))) == NULL) {
                        ud->fn = complain;
                        goto exec;
                }
                ud->type = G_TYPE_FROM_INSTANCE(ud->obj);
                if (eql(ud->action, "force"))
                        ud->fn = fake_ui_activity;
                else if (eql(ud->action, "set_sensitive"))
                        ud->fn = update_sensitivity;
                else if (eql(ud->action, "set_visible"))
                        ud->fn = update_visibility;
                else if (eql(ud->action, "set_size_request"))
                        ud->fn = update_size_request;
                else if (eql(ud->action, "set_tooltip_text"))
                        ud->fn = update_tooltip_text;
                else if (eql(ud->action, "grab_focus"))
                        ud->fn = update_focus;
                else if (eql(ud->action, "style")) {
                        ud->action = name;
                        ud->fn = update_widget_style;
                } else if (ud->type == GTK_TYPE_DRAWING_AREA)
                        ud->fn = update_drawing_area;
                else if (ud->type == GTK_TYPE_TREE_VIEW)
                        ud->fn = update_tree_view;
                else if (ud->type == GTK_TYPE_COMBO_BOX_TEXT)
                        ud->fn = update_combo_box_text;
                else if (ud->type == GTK_TYPE_LABEL)
                        ud->fn = update_label;
                else if (ud->type == GTK_TYPE_IMAGE)
                        ud->fn = update_image;
                else if (ud->type == GTK_TYPE_TEXT_VIEW)
                        ud->fn = update_text_view;
                else if (ud->type == GTK_TYPE_NOTEBOOK)
                        ud->fn = update_notebook;
                else if (ud->type == GTK_TYPE_EXPANDER)
                        ud->fn = update_expander;
                else if (ud->type == GTK_TYPE_FRAME)
                        ud->fn = update_frame;
                else if (ud->type == GTK_TYPE_SCROLLED_WINDOW)
                        ud->fn = update_scrolled_window;
                else if (ud->type == GTK_TYPE_BUTTON)
                        ud->fn = update_button;
                else if (ud->type == GTK_TYPE_FILE_CHOOSER_DIALOG)
                        ud->fn = update_file_chooser_dialog;
                else if (ud->type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                        ud->fn = update_file_chooser_button;
                else if (ud->type == GTK_TYPE_COLOR_BUTTON)
                        ud->fn = update_color_button;
                else if (ud->type == GTK_TYPE_FONT_BUTTON)
                        ud->fn = update_font_button;
                else if (ud->type == GTK_TYPE_PRINT_UNIX_DIALOG)
                        ud->fn = update_print_dialog;
                else if (ud->type == GTK_TYPE_SWITCH)
                        ud->fn = update_switch;
                else if (ud->type == GTK_TYPE_TOGGLE_BUTTON ||
                         ud->type == GTK_TYPE_RADIO_BUTTON ||
                         ud->type == GTK_TYPE_CHECK_BUTTON)
                        ud->fn = update_toggle_button;
                else if (ud->type == GTK_TYPE_ENTRY)
                        ud->fn = update_entry;
                else if (ud->type == GTK_TYPE_SPIN_BUTTON)
                        ud->fn = update_spin_button;
                else if (ud->type == GTK_TYPE_SCALE)
                        ud->fn = update_scale;
                else if (ud->type == GTK_TYPE_PROGRESS_BAR)
                        ud->fn = update_progress_bar;
                else if (ud->type == GTK_TYPE_SPINNER)
                        ud->fn = update_spinner;
                else if (ud->type == GTK_TYPE_STATUSBAR)
                        ud->fn = update_statusbar;
                else if (ud->type == GTK_TYPE_CALENDAR)
                        ud->fn = update_calendar;
                else if (ud->type == GTK_TYPE_SOCKET)
                        ud->fn = update_socket;
                else if (ud->type == GTK_TYPE_WINDOW ||
                         ud->type == GTK_TYPE_DIALOG)
                        ud->fn = update_window;
                else
                        ud->fn = complain;
        exec:
                pthread_testcancel();
                gdk_threads_add_timeout(0, (GSourceFunc) update_ui, ud);
        }
        recursion--;
        return NULL;
}


/*
 * ============================================================
 *  Initialization
 * ============================================================
 */

/*
 * Attach to renderer key "col_number".  Associate "col_number" with
 * the corresponding column number in the underlying model.
 * Due to what looks like a gap in the GTK API, renderer id and column
 * number are taken directly from the XML .ui file.
 */
static bool
tree_view_column_get_renderer_column(const char *ui_file, GtkTreeViewColumn *t_col,
                                     int n, GtkCellRenderer **renderer)
{
        bool r = false;
        char *xpath_base1 = "//object[@class=\"GtkTreeViewColumn\" and @id=\"";
        char *xpath_base2 = "\"]/child[";
        char *xpath_base3 = "]/object[@class=\"GtkCellRendererText\""
                " or @class=\"GtkCellRendererToggle\"]/";
        char *xpath_renderer_id = "/@id";
        char *xpath_text_col = "../attributes/attribute[@name=\"text\""
                " or @name=\"active\"]";
        const char *xpath_id = widget_name(GTK_BUILDABLE(t_col));
        int i;
        size_t xpath_len;
        size_t xpath_n_len = 3;    /* Big Enough (TM) */
        xmlChar *xpath, *renderer_name = NULL, *m_col_s = NULL;
        xmlDocPtr doc;
        xmlNodePtr cur;
        xmlNodeSetPtr nodes;
        xmlXPathContextPtr xpath_ctx;
        xmlXPathObjectPtr xpath_obj;

        if ((doc = xmlParseFile(ui_file)) == NULL)
                return false;
        if ((xpath_ctx = xmlXPathNewContext(doc)) == NULL) {
                xmlFreeDoc(doc);
                return false;
        }
        xpath_len = 2 * (strlen(xpath_base1) + strlen(xpath_id) +
                         strlen(xpath_base2) + xpath_n_len +
                         strlen(xpath_base3)) +
                sizeof('|') +
                strlen(xpath_text_col) + strlen(xpath_renderer_id) +
                sizeof('\0');
        if ((xpath = malloc(xpath_len)) == NULL)
                OOM_ABORT;
        snprintf((char *) xpath, xpath_len, "%s%s%s%d%s%s|%s%s%s%d%s%s",
                 xpath_base1, xpath_id, xpath_base2, n, xpath_base3, xpath_text_col,
                 xpath_base1, xpath_id, xpath_base2, n, xpath_base3, xpath_renderer_id);
        if ((xpath_obj = xmlXPathEvalExpression(xpath, xpath_ctx)) == NULL) {
                xmlXPathFreeContext(xpath_ctx);
                free(xpath);
                xmlFreeDoc(doc);
                return false;
        }
        if ((nodes = xpath_obj->nodesetval) != NULL) {
                for (i = 0; i < nodes->nodeNr; ++i) {
                        if (nodes->nodeTab[i]->type == XML_ELEMENT_NODE) {
                                cur = nodes->nodeTab[i];
                                m_col_s = xmlNodeGetContent(cur);
                        } else {
                                cur = nodes->nodeTab[i];
                                renderer_name = xmlNodeGetContent(cur);
                        }
                }
        }
        if (renderer_name) {
                *renderer = GTK_CELL_RENDERER(
                        gtk_builder_get_object(builder, (char *) renderer_name));
                if (m_col_s) {
                        g_object_set_data(G_OBJECT(*renderer), "col_number",
                                          GINT_TO_POINTER(strtol((char *) m_col_s,
                                                                 NULL, 10)));
                        xmlFree(m_col_s);
                        r = true;
                }
                xmlFree(renderer_name);
        }
        xmlXPathFreeObject(xpath_obj);
        xmlXPathFreeContext(xpath_ctx);
        free(xpath);
        xmlFreeDoc(doc);
        return r;
}

/*
 * Callbacks that forward a modification of a tree view cell to the
 * underlying model
 */
static void
cb_tree_model_edit(GtkCellRenderer *renderer, const gchar *path_s,
                   const gchar *new_text, gpointer view)
{
        GtkTreeIter iter;
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(renderer),
                                                    "col_number"));

        gtk_tree_model_get_iter_from_string(model, &iter, path_s);
        set_tree_view_cell(model, &iter, path_s, col,
                           new_text);
        send_tree_cell_msg_by(send_msg, model, path_s, &iter, col,
                              GTK_BUILDABLE(view));
}

static void
cb_tree_model_toggle(GtkCellRenderer *renderer, gchar *path_s, gpointer view)
{
        GtkTreeIter iter;
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        bool toggle_state;
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(renderer),
                                                    "col_number"));

        gtk_tree_model_get_iter_from_string(model, &iter, path_s);
        gtk_tree_model_get(model, &iter, col, &toggle_state, -1);
        set_tree_view_cell(model, &iter, path_s, col,
                           toggle_state? "0" : "1");
}

static void
connect_widget_signals(gpointer *obj, char *ui_file)
{
        GObject *obj2;
        GType type = G_TYPE_INVALID;
        char *suffix = NULL;
        const char *name = NULL;

        type = G_TYPE_FROM_INSTANCE(obj);
        if (GTK_IS_BUILDABLE(obj))
                name = widget_name(GTK_BUILDABLE(obj));
        if (type == GTK_TYPE_TREE_VIEW_COLUMN) {
                GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(obj));
                GtkTreeViewColumn *tv_col = GTK_TREE_VIEW_COLUMN(obj);
                unsigned int i, n_cells = g_list_length(cells);

                g_list_free(cells);
                g_signal_connect(obj, "clicked", G_CALLBACK(cb_simple), "clicked");
                for (i = 1; i <= n_cells; i++) {
                        GtkCellRenderer *renderer;
                        GtkTreeView *view = GTK_TREE_VIEW(
                                gtk_tree_view_column_get_tree_view(tv_col));
                        gboolean editable = FALSE;

                        if (!tree_view_column_get_renderer_column(ui_file, tv_col,
                                                                  i, &renderer))
                                continue;
                        if (GTK_IS_CELL_RENDERER_TEXT(renderer)) {
                                g_object_get(renderer, "editable", &editable, NULL);
                                if (editable)
                                        g_signal_connect(renderer, "edited",
                                                         G_CALLBACK(cb_tree_model_edit), view);
                        } else if (GTK_IS_CELL_RENDERER_TOGGLE(renderer)) {
                                g_object_get(renderer, "activatable", &editable, NULL);
                                if (editable)
                                        g_signal_connect(renderer, "toggled",
                                                         G_CALLBACK(cb_tree_model_toggle), view);
                        }
                }
        } else if (type == GTK_TYPE_BUTTON) {
                /* Button associated with a GtkTextView. */
                if ((suffix = strstr(name, "_send_text")) != NULL &&
                    GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                else if ((suffix = strstr(name, "_send_selection")) != NULL &&
                         GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text_selection),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                else {
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_simple), "clicked");
                        /* Buttons associated with (and part of) a GtkDialog.
                         * (We shun response ids which could be returned from
                         * gtk_dialog_run() because that would require the
                         * user to define those response ids in Glade,
                         * numerically */
                        if ((suffix = strstr(name, "_cancel")) != NULL &&
                            GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                                if (eql(widget_name(GTK_BUILDABLE(obj2)), MAIN_WIN))
                                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                                else
                                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                        else if ((suffix = strstr(name, "_ok")) != NULL &&
                                 GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name))) {
                                if (GTK_IS_FILE_CHOOSER_DIALOG(obj2))
                                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), GTK_FILE_CHOOSER(obj2));
                                if (eql(widget_name(GTK_BUILDABLE(obj2)), MAIN_WIN))
                                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                                else
                                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                        } else if ((suffix = strstr(name, "_apply")) != NULL &&
                                   GTK_IS_FILE_CHOOSER_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                }
        } else if (GTK_IS_MENU_ITEM(obj))
                if ((suffix = strstr(name, "_invoke")) != NULL &&
                    GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                        g_signal_connect_swapped(obj, "activate", G_CALLBACK(gtk_widget_show), obj2);
                else
                        g_signal_connect(obj, "activate", G_CALLBACK(cb_menu_item), "active");
        else if (GTK_IS_WINDOW(obj)) {
                g_signal_connect(obj, "delete-event", G_CALLBACK(cb_event_simple), "closed");
                if (eql(name, MAIN_WIN))
                        g_signal_connect_swapped(obj, "delete-event", G_CALLBACK(gtk_main_quit), NULL);
                else
                        g_signal_connect(obj, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        } else if (type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                g_signal_connect(obj, "file-set", G_CALLBACK(cb_file_chooser_button), "file");
        else if (type == GTK_TYPE_COLOR_BUTTON)
                g_signal_connect(obj, "color-set", G_CALLBACK(cb_color_button), "color");
        else if (type == GTK_TYPE_FONT_BUTTON)
                g_signal_connect(obj, "font-set", G_CALLBACK(cb_font_button), "font");
        else if (type == GTK_TYPE_SWITCH)
                g_signal_connect(obj, "notify::active", G_CALLBACK(cb_switch), NULL);
        else if (type == GTK_TYPE_TOGGLE_BUTTON || type == GTK_TYPE_RADIO_BUTTON || type == GTK_TYPE_CHECK_BUTTON)
                g_signal_connect(obj, "toggled", G_CALLBACK(cb_toggle_button), NULL);
        else if (type == GTK_TYPE_ENTRY)
                g_signal_connect(obj, "changed", G_CALLBACK(cb_editable), "text");
        else if (type == GTK_TYPE_SPIN_BUTTON)
                g_signal_connect(obj, "value_changed", G_CALLBACK(cb_spin_button), "text"); /* TODO: rename to "value" */
        else if (type == GTK_TYPE_SCALE)
                g_signal_connect(obj, "value-changed", G_CALLBACK(cb_range), "value");
        else if (type == GTK_TYPE_CALENDAR) {
                g_signal_connect(obj, "day-selected-double-click", G_CALLBACK(cb_calendar), "doubleclicked");
                g_signal_connect(obj, "day-selected", G_CALLBACK(cb_calendar), "clicked");
        } else if (type == GTK_TYPE_TREE_SELECTION)
                g_signal_connect(obj, "changed", G_CALLBACK(cb_tree_selection), "clicked");
        else if (type == GTK_TYPE_SOCKET) {
                g_signal_connect(obj, "plug-added", G_CALLBACK(cb_simple), "plug-added");
                g_signal_connect(obj, "plug-removed", G_CALLBACK(cb_simple), "plug-removed");
        } else if (type == GTK_TYPE_DRAWING_AREA)
                g_signal_connect(obj, "draw", G_CALLBACK(cb_draw), NULL);
        else if (type == GTK_TYPE_EVENT_BOX) {
                gtk_widget_set_can_focus(GTK_WIDGET(obj), true);
                g_signal_connect(obj, "button-press-event", G_CALLBACK(cb_event_box_button), "button_press");
                g_signal_connect(obj, "button-release-event", G_CALLBACK(cb_event_box_button), "button_release");
                g_signal_connect(obj, "motion-notify-event", G_CALLBACK(cb_event_box_motion), "motion");
                g_signal_connect(obj, "key-press-event", G_CALLBACK(cb_event_box_key), "key_press");
        }
}

/*
 * We keep a style provider with each widget
 */
static void
add_widget_style_provider(gpointer *obj, void *data)
{
        GtkCssProvider *style_provider;
        GtkStyleContext *context;

        (void) data;
        if (!GTK_IS_WIDGET(obj))
                return;
        style_provider = gtk_css_provider_new();
        context = gtk_widget_get_style_context(GTK_WIDGET(obj));
        gtk_style_context_add_provider(context,
                                       GTK_STYLE_PROVIDER(style_provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_set_data(G_OBJECT(obj), "style_provider", style_provider);
}

static void
prepare_widgets(char *ui_file)
{
        GSList *objects = NULL;

        objects = gtk_builder_get_objects(builder);
        g_slist_foreach(objects, (GFunc) connect_widget_signals, ui_file);
        g_slist_foreach(objects, (GFunc) add_widget_style_provider, NULL);
        g_slist_free(objects);
}

int
main(int argc, char *argv[])
{
        FILE *in = NULL;        /* command input */
        GObject *main_window = NULL;
        bool bg = false;
        char *in_fifo = NULL, *out_fifo = NULL;
        char *ui_file = "pipeglade.ui", *log_file = NULL, *err_file = NULL;
        char *xid = NULL;
        char opt;
        pthread_t receiver;

        /* Disable runtime GLIB deprecation warnings: */
        setenv("G_ENABLE_DIAGNOSTIC", "0", 0);
        out = NULL;
        save = NULL;
        log_out = NULL;
        gtk_init(&argc, &argv);
        while ((opt = getopt(argc, argv, "bGhe:i:l:o:O:u:V")) != -1) {
                switch (opt) {
                case 'b': bg = true; break;
                case 'e': xid = optarg; break;
                case 'G': show_lib_versions(); break;
                case 'h': bye(EXIT_SUCCESS, stdout, USAGE); break;
                case 'i': in_fifo = optarg; break;
                case 'l': log_file = optarg; break;
                case 'o': out_fifo = optarg; break;
                case 'O': err_file = optarg; break;
                case 'u': ui_file = optarg; break;
                case 'V': bye(EXIT_SUCCESS, stdout, "%s\n", VERSION); break;
                case '?':
                default: bye(EXIT_FAILURE, stderr, USAGE); break;
                }
        }
        if (argv[optind] != NULL)
                bye(EXIT_FAILURE, stderr,
                    "illegal parameter '%s'\n" USAGE, argv[optind]);
        redirect_stderr(err_file);
        in = open_in_fifo(in_fifo);
        out = open_out_fifo(out_fifo);
        go_bg_if(bg, in, out, err_file);
        builder = builder_from_file(ui_file);
        log_out = open_log(log_file);
        pthread_create(&receiver, NULL, (void *(*)(void *)) digest_msg, in);
        main_window = find_main_window();
        xmlInitParser();
        LIBXML_TEST_VERSION;
        prepare_widgets(ui_file);
        xembed_if(xid, main_window);
        gtk_main();
        rm_unless(stdin, in, in_fifo);
        rm_unless(stdout, out, out_fifo);
        pthread_cancel(receiver);
        pthread_join(receiver, NULL);
        xmlCleanupParser();
        exit(EXIT_SUCCESS);
}
