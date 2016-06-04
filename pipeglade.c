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
        do {                                            \
                fprintf(stderr,                         \
                        "In %s (%s:%d): ",              \
                        __func__, __FILE__, __LINE__);  \
                abort();                                \
        } while (0)

#define OOM_ABORT                                               \
        do {                                                    \
                fprintf(stderr,                                 \
                        "Out of memory in %s (%s:%d): ",        \
                        __func__, __FILE__, __LINE__);          \
                abort();                                        \
        } while (0)


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
        setvbuf(stderr, NULL, _IOLBF, 0);
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
 * Print a warning about a malformed command to stderr.  Runs inside
 * gtk_main().
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
 * Check if n is, or can be made, the name of a fifo, and put its
 * struct stat into sb.  Give up if n exists but is not a fifo.
 */
static void
find_fifo(const char *n, struct stat *sb)
{
        int fd;

        if ((fd = open(n, O_RDONLY | O_NONBLOCK)) > -1) {
                if (fstat(fd, sb) == 0 &&
                    S_ISFIFO(sb->st_mode) &&
                    fchmod(fd, 0600) == 0) {
                        fstat(fd, sb);
                        close(fd);
                        return;
                }
                bye(EXIT_FAILURE, stderr, "using pre-existing fifo %s: %s\n",
                    n, strerror(errno));
        }
        if (mkfifo(n, 0600) != 0)
                bye(EXIT_FAILURE, stderr, "making fifo %s: %s\n",
                    n, strerror(errno));
        find_fifo(n, sb);
}

static FILE *
open_fifo(const char *name, const char *fmode, FILE *fallback, int bmode)
{
        FILE *s = NULL;
        int fd;
        struct stat sb1, sb2;

        if (name == NULL)
                s = fallback;
        else {
                find_fifo(name, &sb1);
                /* TODO: O_RDWR on fifo is undefined in POSIX */
                if (!((fd = open(name, O_RDWR)) > -1 &&
                      fstat(fd, &sb2) == 0 &&
                      sb1.st_mode == sb2.st_mode &&
                      sb1.st_ino == sb2.st_ino &&
                      sb1.st_dev == sb2.st_dev &&
                      (s = fdopen(fd, fmode)) != NULL))
                        bye(EXIT_FAILURE, stderr, "opening fifo %s (%s): %s\n",
                            name, fmode, strerror(errno));
        }
        setvbuf(s, NULL, bmode, 0);
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

/*
 * Delete fifo fn if streams s and forbidden are distinct
 */
static void
rm_unless(FILE *forbidden, FILE *s, char *fn)
{
        if (s == forbidden)
                return;
        fclose(s);
        remove(fn);
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
 * Write string s to stream o, escaping newlines and backslashes
 */
static void
fputs_escaped(const char *s, FILE *o)
{
        size_t i = 0;
        char c;

        while ((c = s[i++]) != '\0')
                switch (c) {
                case '\\': fputs("\\\\", o); break;
                case '\n': fputs("\\n", o); break;
                default: putc(c, o); break;
                }
}

/*
 * Write log file
 */
static void
log_msg(FILE *l, char *msg)
{
        static char *old_msg;
        static struct timespec start;

        if (l == NULL)    /* no logging */
                return;
        if (msg == NULL && old_msg == NULL)
                fprintf(l, "##########\t##### (New Pipeglade session) #####\n");
        else if (msg == NULL && old_msg != NULL) {
                /* command done; start idle */
                fprintf(l, "%10ld\t", usec_since(&start));
                fputs_escaped(old_msg, l);
                putc('\n', l);
                free(old_msg);
                old_msg = NULL;
        } else if (msg != NULL && old_msg == NULL) {
                /* idle done; start command */
                fprintf(l, "%10ld\t### (Idle) ###\n", usec_since(&start));
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
obj_sans_suffix(GtkBuilder *builder, const char *suffix, const char *name)
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

/*
 * Return the id attribute of widget
 */
static const char *
widget_id(GtkBuildable *widget)
{
        return gtk_buildable_get_name(widget);
}

/*
 * Get the main window; give up on errors
 */
static GObject *
find_main_window(GtkBuilder *builder)
{
        GObject *mw;

        if (GTK_IS_WINDOW(mw = gtk_builder_get_object(builder, MAIN_WIN)))
                return mw;
        bye(EXIT_FAILURE, stderr, "no toplevel window with id \'" MAIN_WIN "\'\n");
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
        const char *w_id = widget_id(obj);
        fd_set wfds;
        int ofd = fileno(o);
        struct timeval timeout = {1, 0};

        FD_ZERO(&wfds);
        FD_SET(ofd, &wfds);
        if (select(ofd + 1, NULL, &wfds, NULL, &timeout) == 1) {
                fprintf(o, "%s:%s ", w_id, tag);
                while ((data = va_arg(ap, char *)) != NULL)
                        fputs_escaped(data, o);
                putc('\n', o);
        } else
                fprintf(stderr,
                        "send error; discarding feedback message %s:%s\n",
                        w_id, tag);
}

/*
 * Send GUI feedback to stream o.  The message format is
 * "<origin>:<tag> <data ...>".  The variadic arguments are strings;
 * last argument must be NULL.
 */
static void
send_msg(FILE *o, GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(o, obj, tag, ap);
        va_end(ap);
}

/*
 * Send message from GUI to stream o.  The message format is
 * "<origin>:set <data ...>", which happens to be a legal command.
 * The variadic arguments are strings; last argument must be NULL.
 */
static void
send_msg_as_cmd(FILE *o, GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(o, obj, "set", ap);
        va_end(ap);
}

/*
 * Stuff to pass around
 */
struct info {
        FILE *fout;             /* UI feedback messages */
        FILE *fin;              /* command input */
        FILE *flog;             /* logging output */
        GtkBuilder *builder;    /* to be read from .ui file */
        GObject *obj;
        GtkTreeModel *model;
        char *txt;
};

/*
 * Data to be passed to and from the GTK main loop
 */
struct ui_data {
        void (*fn)(struct ui_data *);
        GObject *obj;
        char *action;
        char *data;
        char *cmd;
        char *cmd_tokens;
        GType type;
        struct info *args;
};

/*
 * Return pointer to a newly allocated struct info
 */
struct info *
info_new_full(FILE *stream, GObject *obj, GtkTreeModel *model, char *txt)
{
        struct info *ar;

        if ((ar = malloc(sizeof(struct info))) == NULL)
                OOM_ABORT;
        ar->fout = stream;
        ar->fin = NULL;
        ar->flog = NULL;
        ar->builder = NULL;
        ar->obj = obj;
        ar->model = model;
        ar->txt = txt;
        return ar;
}

struct info *
info_txt_new(FILE *stream, char *txt)
{
        return info_new_full(stream, NULL, NULL, txt);
}

struct info *
info_obj_new(FILE *stream, GObject *obj, GtkTreeModel *model)
{
        return info_new_full(stream, obj, model, NULL);
}

/*
 * Use msg_sender() to send a message describing a particular cell
 */
static void
send_tree_cell_msg_by(void msg_sender(FILE *, GtkBuildable *, const char *, ...),
                      const char *path_s,
                      GtkTreeIter *iter, int col, struct info *ar)
{
        GtkBuildable *obj = GTK_BUILDABLE(ar->obj);
        GtkTreeModel *model = ar->model;
        GType col_type;
        GValue value = G_VALUE_INIT;
        char str[BUFLEN], *lc = lc_numeric();

        gtk_tree_model_get_value(model, iter, col, &value);
        col_type = gtk_tree_model_get_column_type(model, col);
        switch (col_type) {
        case G_TYPE_INT:
                snprintf(str, BUFLEN, " %d %d", col, g_value_get_int(&value));
                msg_sender(ar->fout, obj, "gint", path_s, str, NULL);
                break;
        case G_TYPE_LONG:
                snprintf(str, BUFLEN, " %d %ld", col, g_value_get_long(&value));
                msg_sender(ar->fout, obj, "glong", path_s, str, NULL);
                break;
        case G_TYPE_INT64:
                snprintf(str, BUFLEN, " %d %" PRId64, col, g_value_get_int64(&value));
                msg_sender(ar->fout, obj, "gint64", path_s, str, NULL);
                break;
        case G_TYPE_UINT:
                snprintf(str, BUFLEN, " %d %u", col, g_value_get_uint(&value));
                msg_sender(ar->fout, obj, "guint", path_s, str, NULL);
                break;
        case G_TYPE_ULONG:
                snprintf(str, BUFLEN, " %d %lu", col, g_value_get_ulong(&value));
                msg_sender(ar->fout, obj, "gulong", path_s, str, NULL);
                break;
        case G_TYPE_UINT64:
                snprintf(str, BUFLEN, " %d %" PRIu64, col, g_value_get_uint64(&value));
                msg_sender(ar->fout, obj, "guint64", path_s, str, NULL);
                break;
        case G_TYPE_BOOLEAN:
                snprintf(str, BUFLEN, " %d %d", col, g_value_get_boolean(&value));
                msg_sender(ar->fout, obj, "gboolean", path_s, str, NULL);
                break;
        case G_TYPE_FLOAT:
                snprintf(str, BUFLEN, " %d %f", col, g_value_get_float(&value));
                msg_sender(ar->fout, obj, "gfloat", path_s, str, NULL);
                break;
        case G_TYPE_DOUBLE:
                snprintf(str, BUFLEN, " %d %f", col, g_value_get_double(&value));
                msg_sender(ar->fout, obj, "gdouble", path_s, str, NULL);
                break;
        case G_TYPE_STRING:
                snprintf(str, BUFLEN, " %d ", col);
                msg_sender(ar->fout, obj, "gchararray", path_s, str, g_value_get_string(&value), NULL);
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
send_tree_row_msg_by(void msg_sender(FILE *, GtkBuildable *, const char *, ...),
                     char *path_s, GtkTreeIter *iter, struct info *ar)
{
        int col;

        for (col = 0; col < gtk_tree_model_get_n_columns(ar->model); col++)
                send_tree_cell_msg_by(msg_sender, path_s, iter, col, ar);
}

/*
 * send_tree_row_msg serves as an argument for
 * gtk_tree_selection_selected_foreach()
 */
static gboolean
send_tree_row_msg(GtkTreeModel *model,
                  GtkTreePath *path, GtkTreeIter *iter, struct info *ar)
{
        char *path_s = gtk_tree_path_to_string(path);

        ar->model = model;
        send_tree_row_msg_by(send_msg, path_s, iter, ar);
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
                  GtkTreePath *path, GtkTreeIter *iter, struct info *ar)
{
        char *path_s = gtk_tree_path_to_string(path);

        ar->model = model;
        send_tree_row_msg_by(send_msg_as_cmd, path_s, iter, ar);
        g_free(path_s);
        return FALSE;
}

static void
cb_calendar(GtkBuildable *obj, struct info *ar)
{
        char str[BUFLEN];
        unsigned int year = 0, month = 0, day = 0;

        gtk_calendar_get_date(GTK_CALENDAR(obj), &year, &month, &day);
        snprintf(str, BUFLEN, "%04u-%02u-%02u", year, ++month, day);
        send_msg(ar->fout, obj, ar->txt, str, NULL);
}

static void
cb_color_button(GtkBuildable *obj, struct info *ar)
{
        GdkRGBA color;

        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &color);
        send_msg(ar->fout, obj, ar->txt, gdk_rgba_to_string(&color), NULL);
}

static void
cb_editable(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt, gtk_entry_get_text(GTK_ENTRY(obj)), NULL);
}

/*
 * Callback that sends a message about a pointer device button press
 * in a GtkEventBox
 */
static bool
cb_event_box_button(GtkBuildable *obj, GdkEvent *e, struct info *ar)
{
        char data[BUFLEN], *lc = lc_numeric();

        snprintf(data, BUFLEN, "%d %.1lf %.1lf",
                 e->button.button, e->button.x, e->button.y);
        send_msg(ar->fout, obj, ar->txt, data, NULL);
        lc_numeric_free(lc);
        return true;
}

/*
 * Callback that sends in a message the name of the key pressed when
 * a GtkEventBox is focused
 */
static bool
cb_event_box_key(GtkBuildable *obj, GdkEvent *e, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt, gdk_keyval_name(e->key.keyval), NULL);
        return true;
}

/*
 * Callback that sends a message about pointer device motion in a
 * GtkEventBox
 */
static bool
cb_event_box_motion(GtkBuildable *obj, GdkEvent *e, struct info *ar)
{
        char data[BUFLEN], *lc = lc_numeric();

        snprintf(data, BUFLEN, "%.1lf %.1lf", e->button.x, e->button.y);
        send_msg(ar->fout, obj, ar->txt, data, NULL);
        lc_numeric_free(lc);
        return true;
}

/*
 * Callback that only sends "name:tag" and returns false
 */
static bool
cb_event_simple(GtkBuildable *obj, GdkEvent *e, struct info *ar)
{
        (void) e;
        send_msg(ar->fout, obj, ar->txt, NULL);
        return false;
}

static void
cb_file_chooser_button(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt,
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(obj)), NULL);
}

static void
cb_font_button(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt,
                 gtk_font_button_get_font_name(GTK_FONT_BUTTON(obj)), NULL);
}

static void
cb_menu_item(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt,
                 gtk_menu_item_get_label(GTK_MENU_ITEM(obj)), NULL);
}

static void
cb_range(GtkBuildable *obj, struct info *ar)
{
        char str[BUFLEN], *lc = lc_numeric();

        snprintf(str, BUFLEN, "%f", gtk_range_get_value(GTK_RANGE(obj)));
        send_msg(ar->fout, obj, ar->txt, str, NULL);
        lc_numeric_free(lc);
}

/*
 * Callback that sends user's selection from a file dialog
 */
static void
cb_send_file_chooser_dialog_selection(struct info *ar)
{
        send_msg(ar->fout, GTK_BUILDABLE(ar->obj), "file",
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ar->obj)),
                 NULL);
        send_msg(ar->fout, GTK_BUILDABLE(ar->obj), "folder",
                 gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(ar->obj)),
                 NULL);
}

/*
 * Callback that sends in a message the content of the text buffer
 * passed in user_data
 */
static void
cb_send_text(GtkBuildable *obj, struct info *ar)
{
        GtkTextIter a, b;

        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(ar->obj), &a, &b);
        send_msg(ar->fout, obj, "text",
                 gtk_text_buffer_get_text(GTK_TEXT_BUFFER(ar->obj), &a, &b, TRUE),
                 NULL);
}

/*
 * Callback that sends in a message the highlighted text from the text
 * buffer which was passed in user_data
 */
static void
cb_send_text_selection(GtkBuildable *obj, struct info *ar)
{
        GtkTextIter a, b;

        gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(ar->obj), &a, &b);
        send_msg(ar->fout, obj, "text",
                 gtk_text_buffer_get_text(GTK_TEXT_BUFFER(ar->obj), &a, &b, TRUE),
                 NULL);
}

/*
 * Callback that only sends "name:tag" and returns true
 */
static bool
cb_simple(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj, ar->txt, NULL);
        return true;
}

static void
cb_spin_button(GtkBuildable *obj, struct info *ar)
{
        char str[BUFLEN], *lc = lc_numeric();

        snprintf(str, BUFLEN, "%f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(obj)));
        send_msg(ar->fout, obj, ar->txt, str, NULL);
        lc_numeric_free(lc);
}

static void
cb_switch(GtkBuildable *obj, void *pspec, struct info *ar)
{
        (void) pspec;
        send_msg(ar->fout, obj,
                 gtk_switch_get_active(GTK_SWITCH(obj)) ? "1" : "0",
                 NULL);
}

static void
cb_toggle_button(GtkBuildable *obj, struct info *ar)
{
        send_msg(ar->fout, obj,
                 gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(obj)) ? "1" : "0",
                 NULL);
}

static void
cb_tree_selection(GtkBuildable *obj, struct info *ar)
{
        GtkTreeSelection *sel = GTK_TREE_SELECTION(obj);
        GtkTreeView *view = gtk_tree_selection_get_tree_view(sel);

        ar->obj = G_OBJECT(view);
        send_msg(ar->fout, GTK_BUILDABLE(view), ar->txt, NULL);
        gtk_tree_selection_selected_foreach(
                sel, (GtkTreeSelectionForeachFunc) send_tree_row_msg, ar);
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
update_button(struct ui_data *ud)
{
        if (eql(ud->action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(ud->obj), ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_calendar(struct ui_data *ud)
{
        GtkCalendar *calendar = GTK_CALENDAR(ud->obj);
        char dummy;
        int year = 0, month = 0, day = 0;

        if (eql(ud->action, "select_date") &&
            sscanf(ud->data, "%d-%d-%d %c", &year, &month, &day, &dummy) == 3) {
                if (month > -1 && month <= 11 && day > 0 && day <= 31) {
                        gtk_calendar_select_month(calendar, --month, year);
                        gtk_calendar_select_day(calendar, day);
                } else
                        ign_cmd(ud->type, ud->cmd);
        } else if (eql(ud->action, "mark_day") &&
                   sscanf(ud->data, "%d %c", &day, &dummy) == 1) {
                if (day > 0 && day <= 31)
                        gtk_calendar_mark_day(calendar, day);
                else
                        ign_cmd(ud->type, ud->cmd);
        } else if (eql(ud->action, "clear_marks") && sscanf(ud->data, " %c", &dummy) < 1)
                gtk_calendar_clear_marks(calendar);
        else
                ign_cmd(ud->type, ud->cmd);
}

/*
 * Common actions for various kinds of window.  Return false if
 * command is ignored.  Runs inside gtk_main().
 */
static bool
update_class_window(struct ui_data *ud)
{
        GtkWindow *window = GTK_WINDOW(ud->obj);
        char dummy;
        int x, y;

        if (eql(ud->action, "set_title"))
                gtk_window_set_title(window, ud->data);
        else if (eql(ud->action, "fullscreen") && sscanf(ud->data, " %c", &dummy) < 1)
                gtk_window_fullscreen(window);
        else if (eql(ud->action, "unfullscreen") && sscanf(ud->data, " %c", &dummy) < 1)
                gtk_window_unfullscreen(window);
        else if (eql(ud->action, "resize") &&
                 sscanf(ud->data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_window_resize(window, x, y);
        else if (eql(ud->action, "resize") && sscanf(ud->data, " %c", &dummy) < 1) {
                gtk_window_get_default_size(window, &x, &y);
                gtk_window_resize(window, x, y);
        } else if (eql(ud->action, "move") &&
                   sscanf(ud->data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_window_move(window, x, y);
        else
                return false;
        return true;
}

static void
update_color_button(struct ui_data *ud)
{
        GdkRGBA color;

        if (eql(ud->action, "set_color")) {
                gdk_rgba_parse(&color, ud->data);
                gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ud->obj), &color);
        } else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_combo_box_text(struct ui_data *ud)
{
        GtkComboBoxText *combobox = GTK_COMBO_BOX_TEXT(ud->obj);
        char data1[strlen(ud->data) + 1];
        char dummy;
        int val;

        strcpy(data1, ud->data);
        if (eql(ud->action, "prepend_text"))
                gtk_combo_box_text_prepend_text(combobox, data1);
        else if (eql(ud->action, "append_text"))
                gtk_combo_box_text_append_text(combobox, data1);
        else if (eql(ud->action, "remove") && sscanf(ud->data, "%d %c", &val, &dummy) == 1)
                gtk_combo_box_text_remove(combobox, strtol(data1, NULL, 10));
        else if (eql(ud->action, "insert_text")) {
                char *position = strtok(data1, WHITESPACE);
                char *text = strtok(NULL, WHITESPACE);

                gtk_combo_box_text_insert_text(combobox,
                                               strtol(position, NULL, 10), text);
        } else
                ign_cmd(ud->type, ud->cmd);
}

/*
 * update_drawing_area(), which runs inside gtk_main(), maintains a
 * list of drawing operations.  It needs a few helper functions.  It
 * is the responsibility of cb_draw() to actually execute the list.
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

static void
update_drawing_area(struct ui_data *ud)
{
        enum draw_op_stat dost;

        if (eql(ud->action, "remove"))
                dost = rem_draw_op(ud->obj, ud->data);
        else
                dost = ins_draw_op(ud->obj, ud->action, ud->data);
        switch (dost) {
        case NEED_REDRAW:
                gdk_threads_add_idle_full(G_PRIORITY_LOW,
                                          (GSourceFunc) refresh_widget,
                                          GTK_WIDGET(ud->obj), NULL);
                break;
        case FAILURE:
                ign_cmd(ud->type, ud->cmd);
                break;
        case SUCCESS:
                break;
        default:
                ABORT;
                break;
        }
}

static void
update_entry(struct ui_data *ud)
{
        GtkEntry *entry = GTK_ENTRY(ud->obj);

        if (eql(ud->action, "set_text"))
                gtk_entry_set_text(entry, ud->data);
        else if (eql(ud->action, "set_placeholder_text"))
                gtk_entry_set_placeholder_text(entry, ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_expander(struct ui_data *ud)
{
        GtkExpander *expander = GTK_EXPANDER(ud->obj);
        char dummy;
        unsigned int val;

        if (eql(ud->action, "set_expanded") &&
            sscanf(ud->data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_expander_set_expanded(expander, val);
        else if (eql(ud->action, "set_label"))
                gtk_expander_set_label(expander, ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_file_chooser_button(struct ui_data *ud)
{
        if (eql(ud->action, "set_filename"))
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(ud->obj), ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_file_chooser_dialog(struct ui_data *ud)
{
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(ud->obj);

        if (eql(ud->action, "set_filename"))
                gtk_file_chooser_set_filename(chooser, ud->data);
        else if (eql(ud->action, "set_current_name"))
                gtk_file_chooser_set_current_name(chooser, ud->data);
        else if (update_class_window(ud));
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_focus(struct ui_data *ud){
        char dummy;

        if (GTK_IS_WIDGET(ud->obj) &&
            sscanf(ud->data, " %c", &dummy) < 1 &&
            gtk_widget_get_can_focus(GTK_WIDGET(ud->obj)))
                gtk_widget_grab_focus(GTK_WIDGET(ud->obj));
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_font_button(struct ui_data *ud){
        GtkFontButton *font_button = GTK_FONT_BUTTON(ud->obj);

        if (eql(ud->action, "set_font_name"))
                gtk_font_button_set_font_name(font_button, ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_frame(struct ui_data *ud)
{
        if (eql(ud->action, "set_label"))
                gtk_frame_set_label(GTK_FRAME(ud->obj), ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_image(struct ui_data *ud)
{
        GtkIconSize size;
        GtkImage *image = GTK_IMAGE(ud->obj);

        gtk_image_get_icon_name(image, NULL, &size);
        if (eql(ud->action, "set_from_file"))
                gtk_image_set_from_file(image, ud->data);
        else if (eql(ud->action, "set_from_icon_name"))
                gtk_image_set_from_icon_name(image, ud->data, size);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_label(struct ui_data *ud)
{
        if (eql(ud->action, "set_text"))
                gtk_label_set_text(GTK_LABEL(ud->obj), ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

struct handler_id {
        unsigned int id; /* returned by g_signal_connect() and friends */
        bool blocked;    /* we avoid multiple blocking/unblocking */
        struct handler_id *next;
};

static void
update_blocked(struct ui_data *ud)
{
        char dummy;
        struct handler_id *hid;
        unsigned long int val;

        if (sscanf(ud->data, "%lu %c", &val, &dummy) == 1 && val < 2) {
                for (hid = g_object_get_data(ud->obj, "signal-id");
                     hid != NULL; hid = hid->next) {
                        if (val == 0 && hid->blocked == true) {
                                g_signal_handler_unblock(ud->obj, hid->id);
                                hid->blocked = false;
                        } else if (val == 1 && hid->blocked == false) {
                                g_signal_handler_block(ud->obj, hid->id);
                                hid->blocked = true;
                        }
                }
        } else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_notebook(struct ui_data *ud)
{
        char dummy;
        int val, n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(ud->obj));

        if (eql(ud->action, "set_current_page") &&
            sscanf(ud->data, "%d %c", &val, &dummy) == 1 &&
            val >= 0 && val < n_pages)
                gtk_notebook_set_current_page(GTK_NOTEBOOK(ud->obj), val);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_nothing(struct ui_data *ud)
{
        (void) ud;
}

static void
update_print_dialog(struct ui_data *ud)
{
        GtkPageSetup *page_setup;
        GtkPrintJob *job;
        GtkPrintSettings *settings;
        GtkPrintUnixDialog *dialog = GTK_PRINT_UNIX_DIALOG(ud->obj);
        GtkPrinter *printer;
        gint response_id;

        if (eql(ud->action, "print")) {
                response_id = gtk_dialog_run(GTK_DIALOG(dialog));
                switch (response_id) {
                case GTK_RESPONSE_OK:
                        printer = gtk_print_unix_dialog_get_selected_printer(dialog);
                        settings = gtk_print_unix_dialog_get_settings(dialog);
                        page_setup = gtk_print_unix_dialog_get_page_setup(dialog);
                        job = gtk_print_job_new(ud->data, printer, settings, page_setup);
                        if (gtk_print_job_set_source_file(job, ud->data, NULL))
                                gtk_print_job_send(job, NULL, NULL, NULL);
                        else
                                ign_cmd(ud->type, ud->cmd);
                        g_clear_object(&settings);
                        g_clear_object(&job);
                        break;
                case GTK_RESPONSE_CANCEL:
                case GTK_RESPONSE_DELETE_EVENT:
                        break;
                default:
                        fprintf(stderr, "%s sent an unexpected response id (%d)\n",
                                widget_id(GTK_BUILDABLE(dialog)), response_id);
                        break;
                }
                gtk_widget_hide(GTK_WIDGET(dialog));
        } else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_progress_bar(struct ui_data *ud)
{
        GtkProgressBar *progressbar = GTK_PROGRESS_BAR(ud->obj);
        char dummy;
        double frac;

        if (eql(ud->action, "set_text"))
                gtk_progress_bar_set_text(progressbar, *(ud->data) == '\0' ? NULL : ud->data);
        else if (eql(ud->action, "set_fraction") &&
                 sscanf(ud->data, "%lf %c", &frac, &dummy) == 1)
                gtk_progress_bar_set_fraction(progressbar, frac);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_scale(struct ui_data *ud)
{
        GtkRange *range = GTK_RANGE(ud->obj);
        char dummy;
        double val1, val2;

        if (eql(ud->action, "set_value") && sscanf(ud->data, "%lf %c", &val1, &dummy) == 1)
                gtk_range_set_value(range, val1);
        else if (eql(ud->action, "set_fill_level") &&
                 sscanf(ud->data, "%lf %c", &val1, &dummy) == 1) {
                gtk_range_set_fill_level(range, val1);
                gtk_range_set_show_fill_level(range, TRUE);
        } else if (eql(ud->action, "set_fill_level") &&
                   sscanf(ud->data, " %c", &dummy) < 1)
                gtk_range_set_show_fill_level(range, FALSE);
        else if (eql(ud->action, "set_range") &&
                 sscanf(ud->data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_range_set_range(range, val1, val2);
        else if (eql(ud->action, "set_increments") &&
                 sscanf(ud->data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_range_set_increments(range, val1, val2);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_scrolled_window(struct ui_data *ud)
{
        GtkScrolledWindow *window = GTK_SCROLLED_WINDOW(ud->obj);
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(window);
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(window);
        char dummy;
        double d0, d1;

        if (eql(ud->action, "hscroll") && sscanf(ud->data, "%lf %c", &d0, &dummy) == 1)
                gtk_adjustment_set_value(hadj, d0);
        else if (eql(ud->action, "vscroll") && sscanf(ud->data, "%lf %c", &d0, &dummy) == 1)
                gtk_adjustment_set_value(vadj, d0);
        else if (eql(ud->action, "hscroll_to_range") &&
                 sscanf(ud->data, "%lf %lf %c", &d0, &d1, &dummy) == 2)
                gtk_adjustment_clamp_page(hadj, d0, d1);
        else if (eql(ud->action, "vscroll_to_range") &&
                 sscanf(ud->data, "%lf %lf %c", &d0, &d1, &dummy) == 2)
                gtk_adjustment_clamp_page(vadj, d0, d1);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_sensitivity(struct ui_data *ud)
{
        char dummy;
        unsigned int val;

        if (GTK_IS_WIDGET(ud->obj) &&
            sscanf(ud->data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_widget_set_sensitive(GTK_WIDGET(ud->obj), val);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_size_request(struct ui_data *ud)
{
        char dummy;
        int x, y;

        if (GTK_IS_WIDGET(ud->obj) &&
            sscanf(ud->data, "%d %d %c", &x, &y, &dummy) == 2)
                gtk_widget_set_size_request(GTK_WIDGET(ud->obj), x, y);
        else if (GTK_IS_WIDGET(ud->obj) &&
                 sscanf(ud->data, " %c", &dummy) < 1)
                gtk_widget_set_size_request(GTK_WIDGET(ud->obj), -1, -1);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_socket(struct ui_data *ud)
{
        GtkSocket *socket = GTK_SOCKET(ud->obj);
        Window id;
        char str[BUFLEN], dummy;

        if (eql(ud->action, "id") && sscanf(ud->data, " %c", &dummy) < 1) {
                id = gtk_socket_get_id(socket);
                snprintf(str, BUFLEN, "%lu", id);
                send_msg(ud->args->fout, GTK_BUILDABLE(socket), "id", str, NULL);
        } else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_spin_button(struct ui_data *ud)
{
        GtkSpinButton *spinbutton = GTK_SPIN_BUTTON(ud->obj);
        char dummy;
        double val1, val2;

        if (eql(ud->action, "set_text") && /* TODO: rename to "set_value" */
            sscanf(ud->data, "%lf %c", &val1, &dummy) == 1)
                gtk_spin_button_set_value(spinbutton, val1);
        else if (eql(ud->action, "set_range") &&
                 sscanf(ud->data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_spin_button_set_range(spinbutton, val1, val2);
        else if (eql(ud->action, "set_increments") &&
                 sscanf(ud->data, "%lf %lf %c", &val1, &val2, &dummy) == 2)
                gtk_spin_button_set_increments(spinbutton, val1, val2);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_spinner(struct ui_data *ud)
{
        GtkSpinner *spinner = GTK_SPINNER(ud->obj);
        char dummy;

        if (eql(ud->action, "start") && sscanf(ud->data, " %c", &dummy) < 1)
                gtk_spinner_start(spinner);
        else if (eql(ud->action, "stop") && sscanf(ud->data, " %c", &dummy) < 1)
                gtk_spinner_stop(spinner);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_statusbar(struct ui_data *ud)
{
        GtkStatusbar *statusbar = GTK_STATUSBAR(ud->obj);
        char *ctx_msg, dummy;
        const char *status_msg;
        int ctx_len, t;

        /* TODO: remove "push", "pop", "remove_all"; rename "push_id" to "push", etc. */
        if ((ctx_msg = malloc(strlen(ud->data) + 1)) == NULL)
                OOM_ABORT;
        t = sscanf(ud->data, "%s %n%c", ctx_msg, &ctx_len, &dummy);
        status_msg = ud->data + ctx_len;
        if (eql(ud->action, "push"))
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, "0"),
                                   ud->data);
        else if (eql(ud->action, "push_id") && t >= 1)
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, ctx_msg),
                                   status_msg);
        else if (eql(ud->action, "pop") && t < 1)
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(ud->action, "pop_id") && t == 1)
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, ctx_msg));
        else if (eql(ud->action, "remove_all") && t < 1)
                gtk_statusbar_remove_all(statusbar,
                                         gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(ud->action, "remove_all_id") && t == 1)
                gtk_statusbar_remove_all(statusbar,
                                         gtk_statusbar_get_context_id(statusbar, ctx_msg));
        else
                ign_cmd(ud->type, ud->cmd);
        free(ctx_msg);
}

static void
update_switch(struct ui_data *ud)
{
        char dummy;
        unsigned int val;

        if (eql(ud->action, "set_active") &&
            sscanf(ud->data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_switch_set_active(GTK_SWITCH(ud->obj), val);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_text_view(struct ui_data *ud)
{
        FILE *sv;
        GtkTextView *view = GTK_TEXT_VIEW(ud->obj);
        GtkTextBuffer *textbuf = gtk_text_view_get_buffer(view);
        GtkTextIter a, b;
        char dummy;
        int val;

        if (eql(ud->action, "set_text"))
                gtk_text_buffer_set_text(textbuf, ud->data, -1);
        else if (eql(ud->action, "delete") && sscanf(ud->data, " %c", &dummy) < 1) {
                gtk_text_buffer_get_bounds(textbuf, &a, &b);
                gtk_text_buffer_delete(textbuf, &a, &b);
        } else if (eql(ud->action, "insert_at_cursor"))
                gtk_text_buffer_insert_at_cursor(textbuf, ud->data, -1);
        else if (eql(ud->action, "place_cursor") && eql(ud->data, "end")) {
                gtk_text_buffer_get_end_iter(textbuf, &a);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(ud->action, "place_cursor") &&
                   sscanf(ud->data, "%d %c", &val, &dummy) == 1) {
                gtk_text_buffer_get_iter_at_offset(textbuf, &a, val);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(ud->action, "place_cursor_at_line") &&
                   sscanf(ud->data, "%d %c", &val, &dummy) == 1) {
                gtk_text_buffer_get_iter_at_line(textbuf, &a, val);
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(ud->action, "scroll_to_cursor") &&
                   sscanf(ud->data, " %c", &dummy) < 1)
                gtk_text_view_scroll_to_mark(view, gtk_text_buffer_get_insert(textbuf),
                                             0., 0, 0., 0.);
        else if (eql(ud->action, "save") && ud->data != NULL &&
                 (sv = fopen(ud->data, "w")) != NULL) {
                gtk_text_buffer_get_bounds(textbuf, &a, &b);
                send_msg(sv, GTK_BUILDABLE(view), "insert_at_cursor",
                         gtk_text_buffer_get_text(textbuf, &a, &b, TRUE), NULL);
                fclose(sv);
        } else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_toggle_button(struct ui_data *ud)
{
        char dummy;
        unsigned int val;

        if (eql(ud->action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(ud->obj), ud->data);
        else if (eql(ud->action, "set_active") &&
                 sscanf(ud->data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->obj), val);
        else
                ign_cmd(ud->type, ud->cmd);
}

static void
update_tooltip_text(struct ui_data *ud)
{
        if (GTK_IS_WIDGET(ud->obj))
                gtk_widget_set_tooltip_text(GTK_WIDGET(ud->obj), ud->data);
        else
                ign_cmd(ud->type, ud->cmd);
}

/*
 * update_tree_view(), which runs inside gtk_main(), needs a few
 * helper functions
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

        if (gtk_tree_path_get_depth(path) > 0 &&
            gtk_tree_model_get_iter(model, iter, path))
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
        char dummy;
        double d;
        long long int n;

        path = gtk_tree_path_new_from_string(path_s);
        switch (col_type) {
        case G_TYPE_BOOLEAN:
        case G_TYPE_INT:
        case G_TYPE_LONG:
        case G_TYPE_INT64:
        case G_TYPE_UINT:
        case G_TYPE_ULONG:
        case G_TYPE_UINT64:
                if (new_text != NULL &&
                    sscanf(new_text, "%lld %c", &n, &dummy) == 1) {
                        create_subtree(model, path, iter);
                        tree_model_set(model, iter, col, n, -1);
                        ok = true;
                }
                break;
        case G_TYPE_FLOAT:
        case G_TYPE_DOUBLE:
                if (new_text != NULL &&
                    sscanf(new_text, "%lf %c", &d, &dummy) == 1) {
                        create_subtree(model, path, iter);
                        tree_model_set(model, iter, col, d, -1);
                        ok = true;
                }
                break;
        case G_TYPE_STRING:
                create_subtree(model, path, iter);
                tree_model_set(model, iter, col, new_text, -1);
                ok = true;
                break;
        default:
                fprintf(stderr, "column %d: %s not implemented\n",
                        col, g_type_name(col_type));
                ok = true;
                break;
        }
        gtk_tree_path_free(path);
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
update_tree_view(struct ui_data *ud)
{
        GtkTreeView *view = GTK_TREE_VIEW(ud->obj);
        GtkTreeIter iter0, iter1;
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        GtkTreePath *path = NULL;
        bool iter0_valid, iter1_valid;
        char *tokens, *arg0, *arg1, *arg2;
        int col = -1;           /* invalid column number */
        struct info ar;

        if (!GTK_IS_LIST_STORE(model) && !GTK_IS_TREE_STORE(model))
        {
                fprintf(stderr, "missing model/");
                ign_cmd(ud->type, ud->cmd);
                return;
        }
        if ((tokens = malloc(strlen(ud->data) + 1)) == NULL)
                OOM_ABORT;
        strcpy(tokens, ud->data);
        arg0 = strtok(tokens, WHITESPACE);
        arg1 = strtok(NULL, WHITESPACE);
        arg2 = strtok(NULL, "");
        iter0_valid = is_path_string(arg0) &&
                gtk_tree_model_get_iter_from_string(model, &iter0, arg0);
        iter1_valid = is_path_string(arg1) &&
                gtk_tree_model_get_iter_from_string(model, &iter1, arg1);
        if (is_path_string(arg1))
                col = strtol(arg1, NULL, 10);
        if (eql(ud->action, "set") &&
            col > -1 &&
            col < gtk_tree_model_get_n_columns(model) &&
            is_path_string(arg0)) {
                if (set_tree_view_cell(model, &iter0, arg0, col, arg2) == false)
                        ign_cmd(ud->type, ud->cmd);
        } else if (eql(ud->action, "scroll") && iter0_valid && iter1_valid &&
                   arg2 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_scroll_to_cell (view,
                                              path,
                                              gtk_tree_view_get_column(view, col),
                                              0, 0., 0.);
        } else if (eql(ud->action, "expand") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, false);
        } else if (eql(ud->action, "expand_all") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, true);
        } else if (eql(ud->action, "expand_all") && arg0 == NULL)
                gtk_tree_view_expand_all(view);
        else if (eql(ud->action, "collapse") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_collapse_row(view, path);
        } else if (eql(ud->action, "collapse") && arg0 == NULL)
                gtk_tree_view_collapse_all(view);
        else if (eql(ud->action, "set_cursor") && iter0_valid && arg1 == NULL) {
                path = gtk_tree_path_new_from_string(arg0);
                tree_view_set_cursor(view, path, NULL);
        } else if (eql(ud->action, "set_cursor") && arg0 == NULL) {
                tree_view_set_cursor(view, NULL, NULL);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
        } else if (eql(ud->action, "insert_row") &&
                   eql(arg0, "end") && arg1 == NULL)
                tree_model_insert_before(model, &iter1, NULL, NULL);
        else if (eql(ud->action, "insert_row") && iter0_valid &&
                 eql(arg1, "as_child") && arg2 == NULL)
                tree_model_insert_after(model, &iter1, &iter0, NULL);
        else if (eql(ud->action, "insert_row") && iter0_valid && arg1 == NULL)
                tree_model_insert_before(model, &iter1, NULL, &iter0);
        else if (eql(ud->action, "move_row") && iter0_valid &&
                 eql(arg1, "end") && arg2 == NULL)
                tree_model_move_before(model, &iter0, NULL);
        else if (eql(ud->action, "move_row") && iter0_valid && iter1_valid && arg2 == NULL)
                tree_model_move_before(model, &iter0, &iter1);
        else if (eql(ud->action, "remove_row") && iter0_valid && arg1 == NULL)
                tree_model_remove(model, &iter0);
        else if (eql(ud->action, "clear") && arg0 == NULL) {
                tree_view_set_cursor(view, NULL, NULL);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
                tree_model_clear(model);
        } else if (eql(ud->action, "save") && arg0 != NULL &&
                   (ar.fout = fopen(arg0, "w")) != NULL) {
                ar.obj = ud->obj;
                gtk_tree_model_foreach(model,
                                       (GtkTreeModelForeachFunc) save_tree_row_msg,
                                       &ar);
                fclose(ar.fout);
        } else
                ign_cmd(ud->type, ud->cmd);
        free(tokens);
        gtk_tree_path_free(path);
}

static void
update_visibility(struct ui_data *ud)
{
        char dummy;
        unsigned int val;

        if (GTK_IS_WIDGET(ud->obj) &&
            sscanf(ud->data, "%u %c", &val, &dummy) == 1 && val < 2)
                gtk_widget_set_visible(GTK_WIDGET(ud->obj), val);
        else
                ign_cmd(ud->type, ud->cmd);
}

/*
 * Change the style of the widget passed.  Runs inside gtk_main().
 */
static void
update_widget_style(struct ui_data *ud)
{
        GtkStyleContext *context;
        GtkStyleProvider *style_provider;
        char *style_decl;
        const char *prefix = "* {", *suffix = "}";
        size_t sz;

        if (!GTK_IS_WIDGET(ud->obj)) {
                ign_cmd(ud->type, ud->cmd);
                return;
        }
        style_provider = g_object_get_data(ud->obj, "style_provider");
        sz = strlen(prefix) + strlen(suffix) + strlen(ud->data) + 1;
        context = gtk_widget_get_style_context(GTK_WIDGET(ud->obj));
        gtk_style_context_remove_provider(context, style_provider);
        if ((style_decl = malloc(sz)) == NULL)
                OOM_ABORT;
        strcpy(style_decl, prefix);
        strcat(style_decl, ud->data);
        strcat(style_decl, suffix);
        gtk_style_context_add_provider(context, style_provider,
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_data(GTK_CSS_PROVIDER(style_provider),
                                        style_decl, -1, NULL);
        free(style_decl);
}

static void
update_window(struct ui_data *ud)
{
        if (!update_class_window(ud))
                ign_cmd(ud->type, ud->cmd);
}

/*
 * Simulate user activity on various widgets.  Runs inside gtk_main().
 */
static void
fake_ui_activity(struct ui_data *ud)
{
        char dummy;

        if (!GTK_IS_WIDGET(ud->obj) || sscanf(ud->data, " %c", &dummy) > 0)
                ign_cmd(ud->type, ud->cmd);
        else if (GTK_IS_SPIN_BUTTON(ud->obj)) {
                ud->args->txt = "text";
                cb_spin_button(GTK_BUILDABLE(ud->obj), ud->args); /* TODO: rename to "value" */
        } else if (GTK_IS_SCALE(ud->obj)) {
                ud->args->txt = "value";
                cb_range(GTK_BUILDABLE(ud->obj), ud->args);
        } else if (GTK_IS_ENTRY(ud->obj)) {
                ud->args->txt = "text";
                cb_editable(GTK_BUILDABLE(ud->obj), ud->args);
        } else if (GTK_IS_CALENDAR(ud->obj)) {
                ud->args->txt = "clicked";
                cb_calendar(GTK_BUILDABLE(ud->obj), ud->args);
        } else if (GTK_IS_FILE_CHOOSER_BUTTON(ud->obj)) {
                ud->args->txt = "file";
                cb_file_chooser_button(GTK_BUILDABLE(ud->obj), ud->args);
        } else if (!gtk_widget_activate(GTK_WIDGET(ud->obj)))
                ign_cmd(ud->type, ud->cmd);
}

/*
 * The final UI update.  Runs inside gtk_main().
 */
static void
main_quit(struct ui_data *ud)
{
        char dummy;

        if (sscanf(ud->data, " %c", &dummy) < 1)
                gtk_main_quit();
        else
                ign_cmd(ud->type, ud->cmd);
}

/*
 * Write snapshot of widget in an appropriate format to file
 */
static void
take_snapshot(struct ui_data *ud) 
{
        cairo_surface_t *sur = NULL;
        cairo_t *cr = NULL;
        int height;
        int width;

        if (!GTK_IS_WIDGET(ud->obj)) {
                ign_cmd(ud->type, ud->cmd);
                return;
        }
        height = gtk_widget_get_allocated_height(GTK_WIDGET(ud->obj));
        width = gtk_widget_get_allocated_width(GTK_WIDGET(ud->obj));
        if (has_suffix(ud->data, ".epsf") || has_suffix(ud->data, ".eps")) {
                sur = cairo_ps_surface_create(ud->data, width, height);
                cairo_ps_surface_set_eps(sur, TRUE);
        } else if (has_suffix(ud->data, ".pdf"))
                sur = cairo_pdf_surface_create(ud->data, width, height);
        else if (has_suffix(ud->data, ".ps"))
                sur = cairo_ps_surface_create(ud->data, width, height);
        else if (has_suffix(ud->data, ".svg"))
                sur = cairo_svg_surface_create(ud->data, width, height);
        else
                ign_cmd(ud->type, ud->cmd);
        cr = cairo_create(sur);
        gtk_widget_draw(GTK_WIDGET(ud->obj), cr);
        cairo_destroy(cr);
        cairo_surface_destroy(sur);
}

/*
 * Don't update anything; just complain from inside gtk_main()
 */
static void
complain(struct ui_data *ud)
{
        ign_cmd(ud->type, ud->cmd);
}

/*
 * Parse command pointed to by ud, and act on ui accordingly.  Runs
 * once per command inside gtk_main().
 */
static gboolean
update_ui(struct ui_data *ud)
{
        char *lc = lc_numeric();

        (ud->fn)(ud);
        free(ud->cmd_tokens);
        free(ud->cmd);
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
 * GUI.  Runs inside receiver thread.
 */
static void *
digest_cmd(struct info *ar)
{
        static int recursion = -1; /* > 0 means this is a recursive call */

        recursion++;
        for (;;) {
                FILE *cmd = ar->fin;
                struct ui_data *ud = NULL;
                char first_char = '\0';
                char *id;       /* widget id */
                size_t msg_size = 32;
                int id_start = 0, id_end = 0;
                int action_start = 0, action_end = 0;
                int data_start = 0;

                if (feof(cmd))
                        break;
                if ((ud = malloc(sizeof(*ud))) == NULL)
                        OOM_ABORT;
                if ((ud->cmd = malloc(msg_size)) == NULL)
                        OOM_ABORT;
                ud->args = ar;
                ud->type = G_TYPE_INVALID;
                pthread_testcancel();
                if (recursion == 0)
                        log_msg(ar->flog, NULL);
                data_start = read_buf(cmd, &ud->cmd, &msg_size);
                if (recursion == 0)
                        log_msg(ar->flog, ud->cmd);
                if ((ud->cmd_tokens = malloc(strlen(ud->cmd) + 1)) == NULL)
                        OOM_ABORT;
                sscanf(ud->cmd, " %c", &first_char);
                if (data_start == 0 ||   /* empty line */
                    first_char == '#') { /* comment */
                        ud->fn = update_nothing;
                        goto exec;
                }
                strcpy(ud->cmd_tokens, ud->cmd);
                sscanf(ud->cmd_tokens,
                       " %n%*[0-9a-zA-Z_]%n:%n%*[0-9a-zA-Z_]%n%*1[ \t]%n",
                       &id_start, &id_end, &action_start, &action_end, &data_start);
                ud->cmd_tokens[id_end] = ud->cmd_tokens[action_end] = '\0';
                id = ud->cmd_tokens + id_start;
                ud->action = ud->cmd_tokens + action_start;
                ud->data = ud->cmd_tokens + data_start;
                if (eql(ud->action, "main_quit")) {
                        ud->fn = main_quit;
                        goto exec;
                }
                if (eql(ud->action, "load") && strlen(ud->data) > 0 &&
                    remember_loading_file(ud->data)) {
                        struct info a = *ar;

                        if ((a.fin = fopen(ud->data, "r")) != NULL) {
                                digest_cmd(&a);
                                fclose(a.fin);
                                ud->fn = update_nothing;
                        } else
                                ud->fn = complain;
                        remember_loading_file(NULL);
                        goto exec;
                }
                if ((ud->obj = (gtk_builder_get_object(ar->builder, id))) == NULL) {
                        ud->fn = complain;
                        goto exec;
                }
                ud->type = G_TYPE_FROM_INSTANCE(ud->obj);
                if (eql(ud->action, "force"))
                        ud->fn = fake_ui_activity;
                else if (eql(ud->action, "snapshot"))
                        ud->fn = take_snapshot;
                else if (eql(ud->action, "block"))
                        ud->fn = update_blocked;
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
                        ud->action = id;
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
 * Return the first string xpath obtains from ui_file.
 * xmlFree(string) must be called when done
 */
static xmlChar *
xpath1(xmlChar *xpath, const char *ui_file)
{
        xmlChar *r = NULL;
        xmlDocPtr doc = NULL;
        xmlNodeSetPtr nodes = NULL;
        xmlXPathContextPtr ctx = NULL;
        xmlXPathObjectPtr xpath_obj = NULL;

        if ((doc = xmlParseFile(ui_file)) == NULL)
                goto ret0;
        if ((ctx = xmlXPathNewContext(doc)) == NULL)
                goto ret1;
        if ((xpath_obj = xmlXPathEvalExpression(xpath, ctx)) == NULL)
                goto ret2;
        if ((nodes = xpath_obj->nodesetval) != NULL && nodes->nodeNr > 0)
                r = xmlNodeGetContent(nodes->nodeTab[0]);
        xmlXPathFreeObject(xpath_obj);
ret2:
        xmlXPathFreeContext(ctx);
ret1:
        xmlFreeDoc(doc);
ret0:
        return r;
}

/*
 * Attach key "col_number" to renderer.  Associate "col_number" with
 * the corresponding column number in the underlying model.
 * Due to what looks like a gap in the GTK API, renderer id and column
 * number are taken directly from the XML .ui file.
 */
static bool
tree_view_column_get_renderer_column(GtkBuilder *builder, const char *ui_file,
                                     GtkTreeViewColumn *t_col, int n,
                                     GtkCellRenderer **rnd)
{
        bool r = false;
        char *xp_bas1 = "//object[@class=\"GtkTreeViewColumn\" and @id=\"";
        char *xp_bas2 = "\"]/child[";
        char *xp_bas3 = "]/object[@class=\"GtkCellRendererText\""
                " or @class=\"GtkCellRendererToggle\"]/";
        char *xp_rnd_id = "@id";
        char *xp_text_col = "../attributes/attribute[@name=\"text\""
                " or @name=\"active\"]/text()";
        const char *tree_col_id = widget_id(GTK_BUILDABLE(t_col));
        size_t xp_rnd_nam_len, xp_mod_col_len;
        size_t xp_n_len = 3;    /* Big Enough (TM) */
        xmlChar *xp_rnd_nam = NULL, *xp_mod_col = NULL;
        xmlChar *rnd_nam = NULL, *mod_col = NULL;

        /* find name of nth cell renderer under the GtkTreeViewColumn */
        /* tree_col_id */
        xp_rnd_nam_len = strlen(xp_bas1) + strlen(tree_col_id) +
                strlen(xp_bas2) + xp_n_len + strlen(xp_bas3) +
                strlen(xp_rnd_id) + sizeof('\0');
        if ((xp_rnd_nam = malloc(xp_rnd_nam_len)) == NULL)
                OOM_ABORT;
        snprintf((char *) xp_rnd_nam, xp_rnd_nam_len, "%s%s%s%d%s%s",
                 xp_bas1, tree_col_id, xp_bas2, n,
                 xp_bas3, xp_rnd_id);
        rnd_nam = xpath1(xp_rnd_nam, ui_file);
        /* find the model column that is attached to the nth cell */
        /* renderer under GtkTreeViewColumn tree_col_id */
        xp_mod_col_len = strlen(xp_bas1) + strlen(tree_col_id) +
                strlen(xp_bas2) + xp_n_len + strlen(xp_bas3) +
                strlen(xp_text_col) + sizeof('\0');
        if ((xp_mod_col = malloc(xp_mod_col_len)) == NULL)
                OOM_ABORT;
        snprintf((char *) xp_mod_col, xp_mod_col_len, "%s%s%s%d%s%s",
                 xp_bas1, tree_col_id, xp_bas2, n, xp_bas3, xp_text_col);
        mod_col = xpath1(xp_mod_col, ui_file);
        if (rnd_nam) {
                *rnd = GTK_CELL_RENDERER(
                        gtk_builder_get_object(builder, (char *) rnd_nam));
                if (mod_col) {
                        g_object_set_data(G_OBJECT(*rnd), "col_number",
                                          GINT_TO_POINTER(strtol((char *) mod_col,
                                                                 NULL, 10)));
                        r = true;
                }
        }
        free(xp_rnd_nam);
        free(xp_mod_col);
        xmlFree(rnd_nam);
        xmlFree(mod_col);
        return r;
}

/*
 * Callbacks that forward a modification of a tree view cell to the
 * underlying model
 */
static void
cb_tree_model_edit(GtkCellRenderer *renderer, const gchar *path_s,
                   const gchar *new_text, struct info *ar)
{
        GtkTreeIter iter;
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(renderer),
                                                    "col_number"));

        gtk_tree_model_get_iter_from_string(ar->model, &iter, path_s);
        set_tree_view_cell(ar->model, &iter, path_s, col,
                           new_text);
        send_tree_cell_msg_by(send_msg, path_s, &iter, col, ar);
}

static void
cb_tree_model_toggle(GtkCellRenderer *renderer, gchar *path_s, struct info *ar)
{
        GtkTreeIter iter;
        bool toggle_state;
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(renderer),
                                                    "col_number"));

        gtk_tree_model_get_iter_from_string(ar->model, &iter, path_s);
        gtk_tree_model_get(ar->model, &iter, col, &toggle_state, -1);
        set_tree_view_cell(ar->model, &iter, path_s, col,
                           toggle_state? "0" : "1");
}

/*
 * Add new element containing id to the list of callback-handler ids
 * stored in obj's field named "signal_id"
 */
static void
push_handler_id(gpointer *obj, unsigned int id)
{
        struct handler_id *prev_hid, *hid;

        prev_hid = g_object_get_data(G_OBJECT(obj), "signal-id");
        if ((hid = malloc(sizeof(struct handler_id))) == NULL)
                OOM_ABORT;
        hid->next = prev_hid;
        hid->id = id;
        hid->blocked = false;
        g_object_set_data(G_OBJECT(obj), "signal-id", hid);
}

/*
 * Connect function cb to obj's widget signal sig, remembering the
 * handler id in a list in obj's field named "signal-id"
 */
static void
sig_conn(gpointer *obj, char *sig, GCallback cb, struct info *ar)
{
        unsigned int handler_id = g_signal_connect(obj, sig, cb, ar);

        push_handler_id(obj, handler_id);
}

static void
sig_conn_swapped(gpointer *obj, char *sig, GCallback cb, void *data)
{
        unsigned int handler_id = g_signal_connect_swapped(obj, sig, cb, data);

        push_handler_id(obj, handler_id);
}

static void
connect_widget_signals(gpointer *obj, struct info *ar)
{
        GObject *obj2;
        GType type = G_TYPE_INVALID;
        char *suffix = NULL;
        const char *w_id = NULL;
        FILE *o = ar->fout;

        type = G_TYPE_FROM_INSTANCE(obj);
        if (GTK_IS_BUILDABLE(obj))
                w_id = widget_id(GTK_BUILDABLE(obj));
        if (type == GTK_TYPE_TREE_VIEW_COLUMN) {
                GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(obj));
                GtkTreeViewColumn *tv_col = GTK_TREE_VIEW_COLUMN(obj);
                GObject *view = G_OBJECT(
                        gtk_tree_view_column_get_tree_view(tv_col));
                unsigned int i, n_cells = g_list_length(cells);

                g_list_free(cells);
                sig_conn(obj, "clicked", G_CALLBACK(cb_simple), info_txt_new(o, "clicked"));
                for (i = 1; i <= n_cells; i++) {
                        GtkCellRenderer *renderer;
                        gboolean editable = FALSE;

                        if (!tree_view_column_get_renderer_column(ar->builder, ar->txt, tv_col,
                                                                  i, &renderer))
                                continue;
                        if (GTK_IS_CELL_RENDERER_TEXT(renderer)) {
                                g_object_get(renderer, "editable", &editable, NULL);
                                if (editable) {
                                        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));

                                        g_signal_connect(renderer, "edited",
                                                         G_CALLBACK(cb_tree_model_edit),
                                                         info_obj_new(o, view, model));
                                }
                        } else if (GTK_IS_CELL_RENDERER_TOGGLE(renderer)) {
                                g_object_get(renderer, "activatable", &editable, NULL);
                                if (editable) {
                                        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));

                                        g_signal_connect(renderer, "toggled",
                                                         G_CALLBACK(cb_tree_model_toggle),
                                                         info_obj_new(o, NULL, model));
                                }
                        }
                }
        } else if (type == GTK_TYPE_BUTTON)
                /* Button associated with a GtkTextView. */
                if ((suffix = strstr(w_id, "_send_text")) != NULL &&
                    GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(ar->builder, suffix, w_id)))
                        sig_conn(obj, "clicked", G_CALLBACK(cb_send_text),
                                 info_obj_new(o, G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2))), NULL));
                else if ((suffix = strstr(w_id, "_send_selection")) != NULL &&
                         GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(ar->builder, suffix, w_id)))
                        sig_conn(obj, "clicked", G_CALLBACK(cb_send_text_selection),
                                 info_obj_new(o, G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2))), NULL));
                else {
                        sig_conn(obj, "clicked", G_CALLBACK(cb_simple), info_txt_new(o, "clicked"));
                        /* Buttons associated with (and part of) a GtkDialog.
                         * (We shun response ids which could be returned from
                         * gtk_dialog_run() because that would require the
                         * user to define those response ids in Glade,
                         * numerically */
                        if ((suffix = strstr(w_id, "_cancel")) != NULL &&
                            GTK_IS_DIALOG(obj2 = obj_sans_suffix(ar->builder, suffix, w_id)))
                                if (eql(widget_id(GTK_BUILDABLE(obj2)), MAIN_WIN))
                                        sig_conn_swapped(obj, "clicked",
                                                         G_CALLBACK(gtk_main_quit), NULL);
                                else
                                        sig_conn_swapped(obj, "clicked",
                                                         G_CALLBACK(gtk_widget_hide), obj2);
                        else if ((suffix = strstr(w_id, "_ok")) != NULL &&
                                 GTK_IS_DIALOG(obj2 = obj_sans_suffix(ar->builder, suffix, w_id))) {
                                if (GTK_IS_FILE_CHOOSER_DIALOG(obj2))
                                        sig_conn_swapped(obj, "clicked",
                                                         G_CALLBACK(cb_send_file_chooser_dialog_selection),
                                                         info_obj_new(o, obj2, NULL));
                                if (eql(widget_id(GTK_BUILDABLE(obj2)), MAIN_WIN))
                                        sig_conn_swapped(obj, "clicked",
                                                         G_CALLBACK(gtk_main_quit), NULL);
                                else
                                        sig_conn_swapped(obj, "clicked",
                                                         G_CALLBACK(gtk_widget_hide), obj2);
                        } else if ((suffix = strstr(w_id, "_apply")) != NULL &&
                                   GTK_IS_FILE_CHOOSER_DIALOG(obj2 = obj_sans_suffix(ar->builder, suffix, w_id)))
                                sig_conn_swapped(obj, "clicked",
                                                 G_CALLBACK(cb_send_file_chooser_dialog_selection),
                                                 info_obj_new(o, obj2, NULL));
                }
        else if (GTK_IS_MENU_ITEM(obj))
                if ((suffix = strstr(w_id, "_invoke")) != NULL &&
                    GTK_IS_DIALOG(obj2 = obj_sans_suffix(ar->builder, suffix, w_id)))
                        sig_conn_swapped(obj, "activate",
                                         G_CALLBACK(gtk_widget_show), obj2);
                else
                        sig_conn(obj, "activate",
                                 G_CALLBACK(cb_menu_item), info_txt_new(o, "active"));
        else if (GTK_IS_WINDOW(obj)) {
                sig_conn(obj, "delete-event",
                         G_CALLBACK(cb_event_simple), info_txt_new(o, "closed"));
                if (eql(w_id, MAIN_WIN))
                        sig_conn_swapped(obj, "delete-event",
                                         G_CALLBACK(gtk_main_quit), NULL);
                else
                        sig_conn(obj, "delete-event",
                                 G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        } else if (type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                sig_conn(obj, "file-set",
                         G_CALLBACK(cb_file_chooser_button), info_txt_new(o, "file"));
        else if (type == GTK_TYPE_COLOR_BUTTON)
                sig_conn(obj, "color-set",
                         G_CALLBACK(cb_color_button), info_txt_new(o, "color"));
        else if (type == GTK_TYPE_FONT_BUTTON)
                sig_conn(obj, "font-set",
                         G_CALLBACK(cb_font_button), info_txt_new(o, "font"));
        else if (type == GTK_TYPE_SWITCH)
                sig_conn(obj, "notify::active",
                         G_CALLBACK(cb_switch), info_txt_new(o, NULL));
        else if (type == GTK_TYPE_TOGGLE_BUTTON ||
                 type == GTK_TYPE_RADIO_BUTTON ||
                 type == GTK_TYPE_CHECK_BUTTON)
                sig_conn(obj, "toggled",
                         G_CALLBACK(cb_toggle_button), info_txt_new(o, NULL));
        else if (type == GTK_TYPE_ENTRY)
                sig_conn(obj, "changed",
                         G_CALLBACK(cb_editable), info_txt_new(o, "text"));
        else if (type == GTK_TYPE_SPIN_BUTTON)
                sig_conn(obj, "value_changed",
                         G_CALLBACK(cb_spin_button), info_txt_new(o, "text")); /* TODO: rename to "value" */
        else if (type == GTK_TYPE_SCALE)
                sig_conn(obj, "value-changed",
                         G_CALLBACK(cb_range), info_txt_new(o, "value"));
        else if (type == GTK_TYPE_CALENDAR) {
                sig_conn(obj, "day-selected-double-click",
                         G_CALLBACK(cb_calendar), info_txt_new(o, "doubleclicked"));
                sig_conn(obj, "day-selected",
                         G_CALLBACK(cb_calendar), info_txt_new(o, "clicked"));
        } else if (type == GTK_TYPE_TREE_SELECTION)
                sig_conn(obj, "changed",
                         G_CALLBACK(cb_tree_selection), info_txt_new(o, "clicked"));
        else if (type == GTK_TYPE_SOCKET) {
                sig_conn(obj, "plug-added",
                         G_CALLBACK(cb_simple), info_txt_new(o, "plug-added"));
                sig_conn(obj, "plug-removed",
                         G_CALLBACK(cb_simple), info_txt_new(o, "plug-removed"));
                /* TODO: rename to plug_added, plug_removed */
        } else if (type == GTK_TYPE_DRAWING_AREA)
                sig_conn(obj, "draw", G_CALLBACK(cb_draw), NULL);
        else if (type == GTK_TYPE_EVENT_BOX) {
                gtk_widget_set_can_focus(GTK_WIDGET(obj), true);
                sig_conn(obj, "button-press-event",
                         G_CALLBACK(cb_event_box_button),
                         info_txt_new(o, "button_press"));
                sig_conn(obj, "button-release-event",
                         G_CALLBACK(cb_event_box_button),
                         info_txt_new(o, "button_release"));
                sig_conn(obj, "motion-notify-event",
                         G_CALLBACK(cb_event_box_motion),
                         info_txt_new(o, "motion"));
                sig_conn(obj, "key-press-event",
                         G_CALLBACK(cb_event_box_key),
                         info_txt_new(o, "key_press"));
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
prepare_widgets(GtkBuilder *builder, char *ui_file, FILE *out)
{
        GSList *objects = NULL;
        struct info ar = {.builder = builder, .fout = out, .txt = ui_file};

        objects = gtk_builder_get_objects(builder);
        g_slist_foreach(objects, (GFunc) connect_widget_signals, &ar);
        g_slist_foreach(objects, (GFunc) add_widget_style_provider, NULL);
        g_slist_free(objects);
}

int
main(int argc, char *argv[])
{
        GObject *main_window = NULL;
        bool bg = false;
        char *in_fifo = NULL, *out_fifo = NULL;
        char *ui_file = "pipeglade.ui", *log_file = NULL, *err_file = NULL;
        char *xid = NULL;
        char opt;
        pthread_t receiver;
        struct info ar;

        /* Disable runtime GLIB deprecation warnings: */
        setenv("G_ENABLE_DIAGNOSTIC", "0", 0);
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
        ar.fin = open_fifo(in_fifo, "r", stdin, _IONBF);
        ar.fout = open_fifo(out_fifo, "w", stdout, _IOLBF);
        go_bg_if(bg, ar.fin, ar.fout, err_file);
        ar.builder = builder_from_file(ui_file);
        ar.flog = open_log(log_file);
        pthread_create(&receiver, NULL, (void *(*)(void *)) digest_cmd, &ar);
        main_window = find_main_window(ar.builder);
        xmlInitParser();
        LIBXML_TEST_VERSION;
        prepare_widgets(ar.builder, ui_file, ar.fout);
        xembed_if(xid, main_window);
        gtk_main();
        pthread_cancel(receiver);
        pthread_join(receiver, NULL);
        xmlCleanupParser();
        rm_unless(stdin, ar.fin, in_fifo);
        rm_unless(stdout, ar.fout, out_fifo);
        exit(EXIT_SUCCESS);
}
