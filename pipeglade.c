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
                          "[--display X-server]]  |  "  \
                         "[-h | "                       \
                          "-G | "                       \
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
        }
        else
                name = g_type_name(type);
        fprintf(stderr, "ignoring %s%scommand \"%s\"\n", name, pad, msg);
}

/*
 * Create a fifo if necessary, and open it.  Give up if the file
 * exists but is not a fifo
 */
static FILE *
fifo(const char *name, const char *mode)
{
        struct stat sb;
        int fd;
        FILE *s = NULL;
        int bufmode;

        if (name != NULL) {
                stat(name, &sb);
                if (S_ISFIFO(sb.st_mode)) {
                        if (chmod(name, 0600) != 0)
                                bye(EXIT_FAILURE, stderr,
                                    "using pre-existing fifo %s: %s\n",
                                    name, strerror(errno));
                } else if (mkfifo(name, 0600) != 0)
                        bye(EXIT_FAILURE, stderr,
                            "making fifo %s: %s\n", name, strerror(errno));
        }
        switch (mode[0]) {
        case 'r':
                bufmode = _IONBF;
                if (name == NULL)
                        s = stdin;
                else {
                        if ((fd = open(name, O_RDWR | O_NONBLOCK)) < 0)
                                bye(EXIT_FAILURE, stderr,
                                    "opening fifo %s (%s): %s\n",
                                    name, mode, strerror(errno));
                        s = fdopen(fd, "r");
                }
                break;
        case 'w':
                bufmode = _IOLBF;
                if (name == NULL)
                        s = stdout;
                else
                        s = fopen(name, "w+");
                break;
        default:
                ABORT;
                break;
        }
        if (s == NULL)
                bye(EXIT_FAILURE, stderr, "opening fifo %s (%s): %s\n",
                    name, mode, strerror(errno));
        else
                setvbuf(s, NULL, bufmode, 0);
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

        if (eql(name, "-"))
                s = stderr;
        else if (name && (s = fopen(name, "a")) == NULL)
                bye(EXIT_FAILURE, stderr,
                    "opening log file %s: %s\n", name, strerror(errno));
        return s;
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
        static struct timespec start;
        static char *old_msg;

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

/*
 * Remove suffix from name; find the object named like this
 */
static GObject *
obj_sans_suffix(const char *suffix, const char *name)
{
        int str_l;
        char str[BUFLEN + 1] = {'\0'};

        str_l = suffix - name;
        strncpy(str, name, str_l < BUFLEN ? str_l : BUFLEN);
        return gtk_builder_get_object(builder, str);
}

static const char *
widget_name(GtkBuildable *obj)
{
        return gtk_buildable_get_name(obj);
}

/*
 * Store a line from stream s into buf, which should have been malloc'd
 * to bufsize.  Enlarge buf and bufsize if necessary.
 */
static size_t
read_buf(FILE *s, char **buf, size_t *bufsize)
{
        size_t i = 0;
        int c;
        fd_set rfds;
        int ifd = fileno(s);
        bool esc = false;

        FD_ZERO(&rfds);
        FD_SET(ifd, &rfds);
        for (;;) {
                select(ifd + 1, &rfds, NULL, NULL, NULL);
                c = getc(s);
                if (c == '\n' || feof(s))
                        break;
                if (i >= *bufsize - 1)
                        if ((*buf = realloc(*buf, *bufsize = *bufsize * 2)) == NULL)
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
        GValue value = G_VALUE_INIT;
        GType col_type;
        char str[BUFLEN];

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
        char data[BUFLEN];

        snprintf(data, BUFLEN, "%d %.1lf %.1lf",
                 e->button.button, e->button.x, e->button.y);
        send_msg(obj, user_data, data, NULL);
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
        char data[BUFLEN];

        snprintf(data, BUFLEN, "%.1lf %.1lf", e->button.x, e->button.y);
        send_msg(obj, user_data, data, NULL);
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
        char str[BUFLEN];

        snprintf(str, BUFLEN, "%f", gtk_range_get_value(GTK_RANGE(obj)));
        send_msg(obj, tag, str, NULL);
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
        GtkTreeView *view;

        view = gtk_tree_selection_get_tree_view(GTK_TREE_SELECTION(obj));
        send_msg(GTK_BUILDABLE(view), tag, NULL);
        gtk_tree_selection_selected_foreach(GTK_TREE_SELECTION(obj),
                                            (GtkTreeSelectionForeachFunc) send_tree_row_msg,
                                            view);
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
};

/*
 * One single element of a drawing
 */
struct draw_op {
        struct draw_op *next;
        struct draw_op *prev;
        unsigned int id;
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

struct set_dash_args {
        int num_dashes;
        double dashes[];
};

struct set_font_face_args {
        cairo_font_slant_t slant;
        cairo_font_weight_t weight;
        char *family;
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
        int year = 0, month = 0, day = 0;

        if (eql(action, "select_date")) {
                sscanf(data, "%d-%d-%d", &year, &month, &day);
                if (month > -1 && month <= 11 && day > 0 && day <= 31) {
                        gtk_calendar_select_month(calendar, --month, year);
                        gtk_calendar_select_day(calendar, day);
                } else
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "mark_day")) {
                day = strtol(data, NULL, 10);
                if (day > 0 && day <= 31)
                        gtk_calendar_mark_day(calendar, day);
                else
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "clear_marks"))
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
        int x, y;
        bool result = true;

        if (eql(action, "set_title"))
                gtk_window_set_title(window, data);
        else if (eql(action, "fullscreen"))
                gtk_window_fullscreen(window);
        else if (eql(action, "unfullscreen"))
                gtk_window_unfullscreen(window);
        else if (eql(action, "resize")) {
                if (sscanf(data, "%d %d", &x, &y) != 2)
                        gtk_window_get_default_size(window, &x, &y);
                gtk_window_resize(window, x, y);
        } else if (eql(action, "move")) {
                if (sscanf(data, "%d %d", &x, &y) == 2)
                        gtk_window_move(window, x, y);
                else
                        ign_cmd(type, whole_msg);
        } else
                result = false;
        return result;
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

        strcpy(data1, data);
        if (eql(action, "prepend_text"))
                gtk_combo_box_text_prepend_text(combobox, data1);
        else if (eql(action, "append_text"))
                gtk_combo_box_text_append_text(combobox, data1);
        else if (eql(action, "remove"))
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

/*
 * Fill structure *op with the drawing operation according to action
 * and with the appropriate set of arguments
 */
static bool
set_draw_op(struct draw_op *op, const char *action, const char *data)
{
        if (eql(action, "line_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = LINE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf", &op->id, &args->x, &args->y) != 3)
                        return false;
        } else if (eql(action, "rel_line_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_LINE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf", &op->id, &args->x, &args->y) != 3)
                        return false;
        } else if (eql(action, "move_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = MOVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf", &op->id, &args->x, &args->y) != 3)
                        return false;
        } else if (eql(action, "rel_move_to")) {
                struct move_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_MOVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf", &op->id, &args->x, &args->y) != 3)
                        return false;
        } else if (eql(action, "arc")) {
                struct arc_args *args;
                double deg1, deg2;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = ARC;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf", &op->id,
                           &args->x, &args->y, &args->radius, &deg1, &deg2) != 6)
                        return false;
                args->angle1 = deg1 * (M_PI / 180.);
                args->angle2 = deg2 * (M_PI / 180.);
        } else if (eql(action, "arc_negative")) {
                struct arc_args *args;
                double deg1, deg2;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = ARC_NEGATIVE;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf", &op->id,
                           &args->x, &args->y, &args->radius, &deg1, &deg2) != 6)
                        return false;
                args->angle1 = deg1 * (M_PI / 180.);
                args->angle2 = deg2 * (M_PI / 180.);
        } else if (eql(action, "curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = CURVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf %lf", &op->id,
                           &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3) != 7)
                        return false;
        } else if (eql(action, "rel_curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_CURVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf %lf", &op->id,
                           &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3) != 7)
                        return false;
        } else if (eql(action, "rectangle")) {
                struct rectangle_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = RECTANGLE;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf", &op->id,
                           &args->x, &args->y, &args->width, &args->height) != 5)
                        return false;
        } else if (eql(action, "close_path")) {
                op->op = CLOSE_PATH;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
        } else if (eql(action, "show_text")) {
                struct show_text_args *args;
                int start, len;

                if (sscanf(data, "%u %n", &op->id, &start) < 1)
                        return false;
                len = strlen(data + start) + 1;
                if ((args = malloc(sizeof(*args) + len * sizeof(args->text[0]))) == NULL)
                        OOM_ABORT;
                op->op = SHOW_TEXT;
                op->op_args = args;
                args->len = len; /* not used */
                strncpy(args->text, (data + start), len);
        } else if (eql(action, "stroke")) {
                op->op = STROKE;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
        } else if (eql(action, "stroke_preserve")) {
                op->op = STROKE_PRESERVE;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
        } else if (eql(action, "fill")) {
                op->op = FILL;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
        } else if (eql(action, "fill_preserve")) {
                op->op = FILL_PRESERVE;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
        } else if (eql(action, "set_dash")) {
                struct set_dash_args *args;
                int d_start, n, i;
                char data1[strlen(data) + 1];
                char *next, *end;

                strcpy(data1, data);
                if (sscanf(data1, "%u %n", &op->id, &d_start) < 1)
                        return false;
                next = end = data1 + d_start;
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
                for (i = 0, next = data1 + d_start; i < n; i++, next = end) {
                        args->dashes[i] = strtod(next, &end);
                }
        } else if (eql(action, "set_font_face")) {
                struct set_font_face_args *args;
                const char *family;
                int family_start;
                char slant[7 + 1];
                char weight[6 + 1];

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_FONT_FACE;
                op->op_args = args;
                if (sscanf(data, "%u %s %s %n%*s", &op->id, slant, weight, &family_start) != 3)
                        return false;
                if (eql(slant, "normal"))
                        args->slant = CAIRO_FONT_SLANT_NORMAL;
                else if (eql(slant, "italic"))
                        args->slant = CAIRO_FONT_SLANT_ITALIC;
                else if (eql(slant, "oblique"))
                        args->slant = CAIRO_FONT_SLANT_OBLIQUE;
                else
                        return false;
                if (eql(weight, "normal"))
                        args->weight = CAIRO_FONT_WEIGHT_NORMAL;
                else if (eql(weight, "bold"))
                        args->weight = CAIRO_FONT_WEIGHT_BOLD;
                else
                        return false;
                family = data + family_start;
                if ((args->family = malloc(strlen(family) + 1)) == NULL)
                        OOM_ABORT;
                strcpy(args->family, family);
        } else if (eql(action, "set_font_size")) {
                struct set_font_size_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_FONT_SIZE;
                op->op_args = args;
                if (sscanf(data, "%u %lf", &op->id, &args->size) != 2)
                        return false;
        } else if (eql(action, "set_line_cap")) {
                struct set_line_cap_args *args;
                char str[6 + 1];

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_CAP;
                op->op_args = args;
                if (sscanf(data, "%u %6s", &op->id, str) != 2)
                        return false;
                if (eql(str, "butt"))
                        args->line_cap = CAIRO_LINE_CAP_BUTT;
                else if (eql(str, "round"))
                        args->line_cap = CAIRO_LINE_CAP_ROUND;
                else if (eql(str, "square"))
                        args->line_cap = CAIRO_LINE_CAP_SQUARE;
                else
                        return false;
        } else if (eql(action, "set_line_join")) {
                struct set_line_join_args *args;
                char str[5 + 1];

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_JOIN;
                op->op_args = args;
                if (sscanf(data, "%u %5s", &op->id, str) != 2)
                        return false;
                if (eql(str, "miter"))
                        args->line_join = CAIRO_LINE_JOIN_MITER;
                else if (eql(str, "round"))
                        args->line_join = CAIRO_LINE_JOIN_ROUND;
                else if (eql(str, "bevel"))
                        args->line_join = CAIRO_LINE_JOIN_BEVEL;
                else
                        return false;
        } else if (eql(action, "set_line_width")) {
                struct set_line_width_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_LINE_WIDTH;
                op->op_args = args;
                if (sscanf(data, "%u %lf", &op->id, &args->width) != 2)
                        return false;
        } else if (eql(action, "set_source_rgba")) {
                struct set_source_rgba_args *args;
                int c_start;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_SOURCE_RGBA;
                op->op_args = args;
                if ((sscanf(data, "%u %n", &op->id, &c_start) < 1))
                        return false;;
                gdk_rgba_parse(&args->color, data + c_start);
        } else
                return false;
        return true;
}

/*
 * Append another element to widget's "draw_ops" list
 */
static bool
ins_draw_op(GObject *widget, const char *action, const char *data)
{
        struct draw_op *op, *draw_ops, *last_op;

        if ((op = malloc(sizeof(*op))) == NULL)
                OOM_ABORT;
        op->op_args = NULL;
        op->next = NULL;
        if (!set_draw_op(op, action, data)) {
                free(op->op_args);
                free(op);
                return false;
        }
        if ((draw_ops = g_object_get_data(widget, "draw_ops")) == NULL)
                g_object_set_data(widget, "draw_ops", op);
        else {
                for (last_op = draw_ops;
                     last_op->next != NULL;
                     last_op = last_op->next);
                last_op->next = op;
        }
        return true;
}

/*
 * Remove all elements with the given id from widget's "draw_ops" list
 */
static bool
rem_draw_op(GObject *widget, const char *data)
{
        struct draw_op *op, *next_op, *prev_op = NULL;
        unsigned int id;

        if (sscanf(data, "%u", &id) != 1)
                return false;
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
        return true;
}

static void
update_drawing_area(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "remove")) {
                if (!rem_draw_op(obj, data))
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "refresh")) {
                gint width = gtk_widget_get_allocated_width(GTK_WIDGET(obj));
                gint height = gtk_widget_get_allocated_height(GTK_WIDGET(obj));

                gtk_widget_queue_draw_area(GTK_WIDGET(obj), 0, 0, width, height);
        } else if (ins_draw_op(obj, action, data));
        else
                ign_cmd(type, whole_msg);
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

        if (eql(action, "set_expanded"))
                gtk_expander_set_expanded(expander, strtol(data, NULL, 10));
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
        (void) action;
        (void) data;
        if (gtk_widget_get_can_focus(GTK_WIDGET(obj)))
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
        GtkImage *image = GTK_IMAGE(obj);
        GtkIconSize size;

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
        if (eql(action, "set_current_page"))
                gtk_notebook_set_current_page(GTK_NOTEBOOK(obj), strtol(data, NULL, 10));
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
        GtkPrintUnixDialog *dialog = GTK_PRINT_UNIX_DIALOG(obj);
        gint response_id;
        GtkPrinter *printer;
        GtkPrintSettings *settings;
        GtkPageSetup *page_setup;
        GtkPrintJob *job;

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

        if (eql(action, "set_text"))
                gtk_progress_bar_set_text(progressbar, *data == '\0' ? NULL : data);
        else if (eql(action, "set_fraction"))
                gtk_progress_bar_set_fraction(progressbar, strtod(data, NULL));
        else
                ign_cmd(type, whole_msg);
}

static void
update_scale(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_value"))
                gtk_range_set_value(GTK_RANGE(obj), strtod(data, NULL));
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
        double d0, d1;

        if (eql(action, "hscroll") && sscanf(data, "%lf", &d0) == 1)
                gtk_adjustment_set_value(hadj, d0);
        else if (eql(action, "vscroll") && sscanf(data, "%lf", &d0) == 1)
                gtk_adjustment_set_value(vadj, d0);
        else if (eql(action, "hscroll_to_range") &&
                 sscanf(data, "%lf %lf", &d0, &d1) == 2)
                gtk_adjustment_clamp_page(hadj, d0, d1);
        else if (eql(action, "vscroll_to_range") &&
                 sscanf(data, "%lf %lf", &d0, &d1) == 2)
                gtk_adjustment_clamp_page(vadj, d0, d1);
        else
                ign_cmd(type, whole_msg);
}

static void
update_sensitivity(GObject *obj, const char *action,
                   const char *data, const char *whole_msg, GType type)
{
        (void) action;
        (void) whole_msg;
        (void) type;
        gtk_widget_set_sensitive(GTK_WIDGET(obj), strtol(data, NULL, 10));
}

static void
update_size_request(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        int x, y;

        (void) action;
        (void) whole_msg;
        (void) type;
        if (sscanf(data, "%d %d", &x, &y) == 2)
                gtk_widget_set_size_request(GTK_WIDGET(obj), x, y);
        else
                gtk_widget_set_size_request(GTK_WIDGET(obj), -1, -1);
}

static void
update_socket(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkSocket *socket = GTK_SOCKET(obj);
        Window id;
        char str[BUFLEN];

        (void) data;
        if (eql(action, "id")) {
                id = gtk_socket_get_id(socket);
                snprintf(str, BUFLEN, "%lu", id);
                send_msg(GTK_BUILDABLE(socket), "id", str, NULL);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_spinner(GObject *obj, const char *action,
               const char *data, const char *whole_msg, GType type)
{
        GtkSpinner *spinner = GTK_SPINNER(obj);

        (void) data;
        if (eql(action, "start"))
                gtk_spinner_start(spinner);
        else if (eql(action, "stop"))
                gtk_spinner_stop(spinner);
        else
                ign_cmd(type, whole_msg);
}

static void
update_statusbar(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        GtkStatusbar *statusbar = GTK_STATUSBAR(obj);
        char *ctx_msg, *msg;
        size_t ctx_len;

        if ((ctx_msg = malloc(strlen(data) + 1)) == NULL)
                OOM_ABORT;
        strcpy(ctx_msg, data);
        ctx_len = strcspn(ctx_msg, WHITESPACE);
        if (ctx_len > 0) {
                ctx_msg[ctx_len] = '\0';
                msg = ctx_msg + ctx_len + 1;
        } else
                msg = ctx_msg + strlen(ctx_msg);
        if (eql(action, "push"))
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, "0"),
                                    data);
        else if (eql(action, "push_id"))
                gtk_statusbar_push(statusbar,
                                   gtk_statusbar_get_context_id(statusbar, ctx_msg),
                                   msg);
        else if (eql(action, "pop"))
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(action, "pop_id"))
                gtk_statusbar_pop(statusbar,
                                  gtk_statusbar_get_context_id(statusbar, ctx_msg));
        else if (eql(action, "remove_all"))
                gtk_statusbar_remove_all(statusbar,
                                         gtk_statusbar_get_context_id(statusbar, "0"));
        else if (eql(action, "remove_all_id"))
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
        if (eql(action, "set_active"))
                gtk_switch_set_active(GTK_SWITCH(obj), strtol(data, NULL, 10));
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

        if (eql(action, "set_text"))
                gtk_text_buffer_set_text(textbuf, data, -1);
        else if (eql(action, "delete")) {
                gtk_text_buffer_get_bounds(textbuf, &a, &b);
                gtk_text_buffer_delete(textbuf, &a, &b);
        } else if (eql(action, "insert_at_cursor"))
                gtk_text_buffer_insert_at_cursor(textbuf, data, -1);
        else if (eql(action, "place_cursor")) {
                if (eql(data, "end"))
                        gtk_text_buffer_get_end_iter(textbuf, &a);
                else    /* numeric offset */
                        gtk_text_buffer_get_iter_at_offset(textbuf, &a,
                                                           strtol(data, NULL, 10));
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "place_cursor_at_line")) {
                gtk_text_buffer_get_iter_at_line(textbuf, &a, strtol(data, NULL, 10));
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "scroll_to_cursor"))
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
        if (eql(action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(obj), data);
        else if (eql(action, "set_active"))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(obj), strtol(data, NULL, 10));
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
        GtkTreePath *path_1;    /* path's predecessor */
        GtkTreeIter iter_1;     /* iter's predecessor */

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
        long long int n;
        double d;
        GtkTreePath *path;
        char *endptr;
        bool ok = false;

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
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        GtkTreeIter iter0, iter1;
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
            col > -1 && col < gtk_tree_model_get_n_columns(model) &&
            is_path_string(arg0)) {
                if (set_tree_view_cell(model, &iter0, arg0, col, arg2) == false)
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "scroll") && iter0_valid && iter1_valid) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_scroll_to_cell (view,
                                              path,
                                              gtk_tree_view_get_column(view, col),
                                              0, 0., 0.);
        } else if (eql(action, "expand") && iter0_valid) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, false);
        } else if (eql(action, "expand_all") && iter0_valid) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_expand_row(view, path, true);
        } else if (eql(action, "expand_all") && arg0 == NULL)
                gtk_tree_view_expand_all(view);
        else if (eql(action, "collapse") && iter0_valid) {
                path = gtk_tree_path_new_from_string(arg0);
                gtk_tree_view_collapse_row(view, path);
        } else if (eql(action, "collapse") && arg0 == NULL)
                gtk_tree_view_collapse_all(view);
        else if (eql(action, "set_cursor") && iter0_valid) {
                path = gtk_tree_path_new_from_string(arg0);
                tree_view_set_cursor(view, path, NULL);
        } else if (eql(action, "set_cursor") && arg0 == NULL) {
                tree_view_set_cursor(view, NULL, NULL);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
        } else if (eql(action, "insert_row") && eql(arg0, "end"))
                tree_model_insert_before(model, &iter1, NULL, NULL);
        else if (eql(action, "insert_row") && iter0_valid && eql(arg1, "as_child"))
                tree_model_insert_after(model, &iter1, &iter0, NULL);
        else if (eql(action, "insert_row") && iter0_valid)
                tree_model_insert_before(model, &iter1, NULL, &iter0);
        else if (eql(action, "move_row") && iter0_valid && eql(arg1, "end"))
                tree_model_move_before(model, &iter0, NULL);
        else if (eql(action, "move_row") && iter0_valid && iter1_valid)
                tree_model_move_before(model, &iter0, &iter1);
        else if (eql(action, "remove_row") && iter0_valid)
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
        (void) action;
        (void) whole_msg;
        (void) type;
        gtk_widget_set_visible(GTK_WIDGET(obj), strtol(data, NULL, 10));
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
        (void) action;
        (void) data;
        if (!GTK_IS_WIDGET(obj))
                ign_cmd(type, whole_msg);
        else if (GTK_IS_ENTRY(obj) || GTK_IS_SPIN_BUTTON(obj))
                cb_editable(GTK_BUILDABLE(obj), "text");
        else if (GTK_IS_SCALE(obj))
                cb_range(GTK_BUILDABLE(obj), "value");
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
        (void) obj;
        (void) action;
        (void) data;
        (void) whole_msg;
        (void) type;
        gtk_main_quit();
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
 * Parse command pointed to by ud, and act on ui accordingly; post
 * semaphore ud.msg_digested if done.  Runs once per command inside
 * gtk_main_loop().
 */
static gboolean
update_ui(struct ui_data *ud)
{
        (ud->fn)(ud->obj, ud->action, ud->data, ud->msg, ud->type);
        free(ud->msg_tokens);
        free(ud->msg);
        free(ud);
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
                if (eql(ud->action, "main_quit")) {
                        ud->fn = main_quit;
                        goto exec;
                }
                ud->data = ud->msg_tokens + data_start;
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
                else if (ud->type == GTK_TYPE_SPIN_BUTTON ||
                         ud->type == GTK_TYPE_ENTRY)
                        ud->fn = update_entry;
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
 * Callbacks that forward a modification of a tree view cell to the
 * underlying model
 */
static void
cb_tree_model_edit(GtkCellRenderer *renderer, const gchar *path_s,
                   const gchar *new_text, gpointer model)
{
        GtkTreeIter iter;
        GtkTreeView *view;
        void *col;

        gtk_tree_model_get_iter_from_string(model, &iter, path_s);
        view = g_object_get_data(G_OBJECT(renderer), "tree_view");
        col = g_object_get_data(G_OBJECT(renderer), "col_number");
        set_tree_view_cell(model, &iter, path_s, GPOINTER_TO_INT(col),
                           new_text);
        send_tree_cell_msg_by(send_msg, model, path_s, &iter, GPOINTER_TO_INT(col),
                              GTK_BUILDABLE(view));
}

static void
cb_tree_model_toggle(GtkCellRenderer *renderer, gchar *path_s, gpointer model)
{
        GtkTreeIter iter;
        void *col;
        bool toggle_state;

        gtk_tree_model_get_iter_from_string(model, &iter, path_s);
        col = g_object_get_data(G_OBJECT(renderer), "col_number");
        gtk_tree_model_get(model, &iter, col, &toggle_state, -1);
        set_tree_view_cell(model, &iter, path_s, GPOINTER_TO_INT(col),
                           toggle_state? "0" : "1");
}

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
        xmlDocPtr doc;
        xmlXPathContextPtr xpath_ctx;
        xmlXPathObjectPtr xpath_obj;
        xmlNodeSetPtr nodes;
        xmlNodePtr cur;
        int i;
        xmlChar *xpath, *renderer_name = NULL, *m_col_s = NULL;
        char *xpath_base1 = "//object[@class=\"GtkTreeViewColumn\" and @id=\"";
        const char *xpath_id = widget_name(GTK_BUILDABLE(t_col));
        char *xpath_base2 = "\"]/child[";
        size_t xpath_n_len = 3;    /* Big Enough (TM) */
        char *xpath_base3 = "]/object[@class=\"GtkCellRendererText\""
                " or @class=\"GtkCellRendererToggle\"]/";
        char *xpath_text_col = "../attributes/attribute[@name=\"text\""
                " or @name=\"active\"]";
        char *xpath_renderer_id = "/@id";
        size_t xpath_len;
        bool r = false;

        if ((doc = xmlParseFile(ui_file)) == NULL)
                return false;
        if ((xpath_ctx = xmlXPathNewContext(doc)) == NULL) {
                xmlFreeDoc(doc);
                return false;
        }
        xpath_len = 2 * (strlen(xpath_base1) + strlen(xpath_id) +
                         strlen(xpath_base2) + xpath_n_len +
                         strlen(xpath_base3))
                + 1             /* "|" */
                + strlen(xpath_text_col) + strlen(xpath_renderer_id)
                + 1;            /* '\0' */
        if ((xpath = malloc(xpath_len)) == NULL) {
                xmlFreeDoc(doc);
                return false;
        }
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

static void
connect_widget_signals(gpointer *obj, char *ui_file)
{
        const char *name = NULL;
        char *suffix = NULL;
        GObject *obj2;
        GType type = G_TYPE_INVALID;

        type = G_TYPE_FROM_INSTANCE(obj);
        if (GTK_IS_BUILDABLE(obj))
                name = widget_name(GTK_BUILDABLE(obj));
        if (type == GTK_TYPE_TREE_VIEW_COLUMN) {
                gboolean editable = FALSE;
                GtkTreeView *view;
                GtkTreeModel *model;
                GtkCellRenderer *renderer;
                int i;

                g_signal_connect(obj, "clicked", G_CALLBACK(cb_simple), "clicked");
                view = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(GTK_TREE_VIEW_COLUMN(obj)));
                model = gtk_tree_view_get_model(view);
                for (i = 1;; i++) {
                        if (!tree_view_column_get_renderer_column(ui_file, GTK_TREE_VIEW_COLUMN(obj), i, &renderer))
                                break;
                        g_object_set_data(G_OBJECT(renderer), "tree_view", view);
                        if (GTK_IS_CELL_RENDERER_TEXT(renderer)) {
                                g_object_get(renderer, "editable", &editable, NULL);
                                if (editable)
                                        g_signal_connect(renderer, "edited", G_CALLBACK(cb_tree_model_edit), model);
                        } else if (GTK_IS_CELL_RENDERER_TOGGLE(renderer)) {
                                g_object_get(renderer, "activatable", &editable, NULL);
                                if (editable)
                                        g_signal_connect(renderer, "toggled", G_CALLBACK(cb_tree_model_toggle), model);
                        }
                }
        }
        else if (type == GTK_TYPE_BUTTON) {
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
        else if (type == GTK_TYPE_SPIN_BUTTON || type == GTK_TYPE_ENTRY)
                g_signal_connect(obj, "changed", G_CALLBACK(cb_editable), "text");
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
        GtkStyleContext *context;
        GtkCssProvider *style_provider;

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
        char opt;
        char *in_fifo = NULL, *out_fifo = NULL, *ui_file = NULL;
        char *log_file = NULL;
        char *xid_s = NULL, xid_s2[BUFLEN];
        bool bg = false;
        pid_t pid = 0;
        Window xid;
        GtkWidget *plug, *body;
        pthread_t receiver;
        GError *error = NULL;
        GObject *main_window = NULL;
        FILE *in = NULL;        /* command input */

        /* Disable runtime GLIB deprecation warnings: */
        setenv("G_ENABLE_DIAGNOSTIC", "0", 0);
        out = NULL;
        save = NULL;
        log_out = NULL;
        gtk_init(&argc, &argv);
        while ((opt = getopt(argc, argv, "bhe:i:l:o:u:GV")) != -1) {
                switch (opt) {
                case 'b': bg = true; break;
                case 'e': xid_s = optarg; break;
                case 'i': in_fifo = optarg; break;
                case 'l': log_file = optarg; break;
                case 'o': out_fifo = optarg; break;
                case 'u': ui_file = optarg; break;
                case 'G': bye(EXIT_SUCCESS, stdout,
                              "GTK+  v%d.%d.%d (running v%d.%d.%d)\n"
                              "cairo v%s (running v%s)\n",
                              GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
                              gtk_get_major_version(), gtk_get_minor_version(),
                              gtk_get_micro_version(),
                              CAIRO_VERSION_STRING, cairo_version_string());
                        break;
                case 'V': bye(EXIT_SUCCESS, stdout, "%s\n", VERSION); break;
                case 'h': bye(EXIT_SUCCESS, stdout, USAGE); break;
                case '?':
                default: bye(EXIT_FAILURE, stderr, USAGE); break;
                }
        }
        if (argv[optind] != NULL)
                bye(EXIT_FAILURE, stderr,
                    "illegal parameter '%s'\n" USAGE, argv[optind]);
        in = fifo(in_fifo, "r");
        out = fifo(out_fifo, "w");
        if (bg) {
                if (in == stdin || out == stdout)
                        bye(EXIT_FAILURE, stderr,
                            "parameter -b requires both -i and -o\n");
                else if ((pid = fork()) > 0)
                        bye(EXIT_SUCCESS, stdout, "%d\n", pid);
                else if (pid < 0)
                        bye(EXIT_FAILURE, stderr,
                            "going to background: %s\n", strerror(errno));
        }
        if (ui_file == NULL)
                ui_file = "pipeglade.ui";
        builder = gtk_builder_new();
        if (gtk_builder_add_from_file(builder, ui_file, &error) == 0)
                bye(EXIT_FAILURE, stderr, "%s\n", error->message);
        log_out = open_log(log_file);
        pthread_create(&receiver, NULL, (void *(*)(void *)) digest_msg, in);
        main_window = gtk_builder_get_object(builder, MAIN_WIN);
        if (!GTK_IS_WINDOW(main_window))
                bye(EXIT_FAILURE, stderr,
                    "no toplevel window named \'" MAIN_WIN "\'\n");
        xmlInitParser();
        LIBXML_TEST_VERSION;
        prepare_widgets(ui_file);
        if (xid_s == NULL)      /* standalone */
                gtk_widget_show(GTK_WIDGET(main_window));
        else {                  /* We're being XEmbedded */
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
        gtk_main();
        if (in != stdin) {
                fclose(in);
                unlink(in_fifo);
        }
        if (out != stdout) {
                fclose(out);
                unlink(out_fifo);
        }
        pthread_cancel(receiver);
        pthread_join(receiver, NULL);
        xmlCleanupParser();
        exit(EXIT_SUCCESS);
}
