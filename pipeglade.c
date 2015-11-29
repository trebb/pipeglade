/*
 * Copyright (c) 2014, 2015 Bert Burgemeister <trebbu@googlemail.com>
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
#include <search.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "4.2.0"
#define BUFLEN 256
#define WHITESPACE " \t\n"
#define MAIN_WIN "main"
#define USAGE                                   \
        "usage: pipeglade "                     \
        "[-h] "                                 \
        "[-e xid] "                             \
        "[-i in-fifo] "                         \
        "[-o out-fifo] "                        \
        "[-u glade-file.ui] "                   \
        "[-G] "                                 \
        "[-V]\n"

#define ABORT                                                   \
        {                                                       \
                fprintf(stderr,                                 \
                        "In %s (%s:%d): ",                      \
                        __func__, __FILE__, __LINE__);          \
                abort();                                        \
        }

#define OOM_ABORT                                               \
        {                                                       \
                fprintf(stderr,                                 \
                        "Out of memory in %s (%s:%d): ",        \
                        __func__, __FILE__, __LINE__);          \
                abort();                                        \
        }                                                       \

static FILE *in;                /* commands */
static FILE *out;               /* UI feedback messages */
static FILE *save;              /* saving user data */
static char *loading_files[BUFLEN]; /* Keep track of */
static size_t newest_loading_file;  /* loading files */
struct ui_data {
        void (*fn)(GObject *, const char *action,
                   const char *data, const char *msg, GType type);
        GObject *obj;
        char *action;
        char *data;
        char *msg;
        char *msg_tokens;
        GType type;
        sem_t msg_digested;
};
static GtkBuilder *builder;     /* to be read from .ui file */

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
 * Check if string s1 and s2 are equal
 */
static bool
eql(const char *s1, const char *s2)
{
        return s1 != NULL && s2 != NULL && strcmp(s1, s2) == 0;
}

static const char *
widget_name(void *obj)
{
        return gtk_buildable_get_name(GTK_BUILDABLE(obj));
}

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
 * Send GUI feedback to global stream "out" or "save", respectively.
 * The message format is "<origin>:<tag> <data ...>".  The variadic
 * arguments are strings; last argument must be NULL.
 */
static void
send_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(out, obj, tag, ap);
        va_end(ap);
}

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
treeview_save_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;

        va_start(ap, tag);
        send_msg_to(save, obj, "set", ap);
        va_end(ap);
}

/*
 * Callback that sends user's selection from a file dialog
 */
static void
cb_send_file_chooser_dialog_selection(gpointer user_data)
{
        send_msg(user_data, "file",
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(user_data)), NULL);
        send_msg(user_data, "folder",
                 gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(user_data)), NULL);
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
 * Use msg_sender() to send a message describing a particular cell.
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

static void
send_tree_cell_msg(GtkTreeModel *model, const char *path_s,
                   GtkTreeIter *iter, int col, GtkBuildable *obj)
{
        send_tree_cell_msg_by(send_msg, model, path_s, iter, col, obj);
}

/*
 * Use msg_sender() to send one message per column for a single row.
 */
static void
send_tree_row_msg_by(void msg_sender(GtkBuildable *, const char *, ...),
                     GtkTreeModel *model, GtkTreePath *path,
                     GtkTreeIter *iter, GtkBuildable *obj)
{
        char *path_s = gtk_tree_path_to_string(path);
        int col;

        for (col = 0; col < gtk_tree_model_get_n_columns(model); col++)
                send_tree_cell_msg_by(msg_sender, model, path_s, iter, col, obj);
        g_free(path_s);
}

/*
 * send_tree_row_msg serves as an argument for
 * gtk_tree_selection_selected_foreach()
*/
static void
send_tree_row_msg(GtkTreeModel *model,
                  GtkTreePath *path, GtkTreeIter *iter, GtkBuildable *obj)
{
        send_tree_row_msg_by(send_msg, model, path, iter, obj);
}

/*
 * save_tree_row_msg serves as an argument for
 * gtk_tree_model_foreach()
 */
static gboolean
save_tree_row_msg(GtkTreeModel *model,
                  GtkTreePath *path, GtkTreeIter *iter, gpointer obj)
{
        send_tree_row_msg_by(treeview_save_msg, model, path, iter, GTK_BUILDABLE(obj));
        return FALSE;
}

/*
 * Callback that sends message(s) whose nature depends on the
 * arguments passed.  A call to this function will also be initiated
 * by the user command ...:force.
 */
static void
cb(GtkBuildable *obj, gpointer user_data)
{
        char str[BUFLEN];
        GdkRGBA color;
        GtkTreeView *tree_view;
        unsigned int year = 0, month = 0, day = 0;

        if (GTK_IS_ENTRY(obj))
                send_msg(obj, user_data, gtk_entry_get_text(GTK_ENTRY(obj)), NULL);
        else if (GTK_IS_MENU_ITEM(obj))
                send_msg(obj, user_data, gtk_menu_item_get_label(GTK_MENU_ITEM(obj)), NULL);
        else if (GTK_IS_RANGE(obj)) {
                snprintf(str, BUFLEN, "%f", gtk_range_get_value(GTK_RANGE(obj)));
                send_msg(obj, user_data, str, NULL);
        } else if (GTK_IS_SWITCH(obj))
                send_msg(obj, gtk_switch_get_active(GTK_SWITCH(obj)) ? "1" : "0", NULL);
        else if (GTK_IS_TOGGLE_BUTTON(obj))
                send_msg(obj, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(obj)) ? "1" : "0", NULL);
        else if (GTK_IS_COLOR_BUTTON(obj)) {
                gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &color);
                send_msg(obj, user_data, gdk_rgba_to_string(&color), NULL);
        } else if (GTK_IS_FONT_BUTTON(obj))
                send_msg(obj, user_data, gtk_font_button_get_font_name(GTK_FONT_BUTTON(obj)), NULL);
        else if (GTK_IS_FILE_CHOOSER_BUTTON(obj))
                send_msg(obj, user_data, gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(obj)), NULL);
        else if (GTK_IS_BUTTON(obj) || GTK_IS_TREE_VIEW_COLUMN(obj) || GTK_IS_SOCKET(obj))
                send_msg(obj, user_data, NULL);
        else if (GTK_IS_CALENDAR(obj)) {
                gtk_calendar_get_date(GTK_CALENDAR(obj), &year, &month, &day);
                snprintf(str, BUFLEN, "%04u-%02u-%02u", year, ++month, day);
                send_msg(obj, user_data, str, NULL);
        } else if (GTK_IS_TREE_SELECTION(obj)) {
                tree_view = gtk_tree_selection_get_tree_view(GTK_TREE_SELECTION(obj));
                send_msg(GTK_BUILDABLE(tree_view), user_data, NULL);
                gtk_tree_selection_selected_foreach(GTK_TREE_SELECTION(obj),
                                                    (GtkTreeSelectionForeachFunc)send_tree_row_msg,
                                                    GTK_BUILDABLE(tree_view));
        } else
                fprintf(stderr, "ignoring callback from %s\n", widget_name(obj));
}

/*
 * Callback like cb(), but returning true.
 */
static bool
cb_true(GtkBuildable *obj, gpointer user_data)
{
        cb(obj, user_data);
        return true;
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
 * Warning message
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
 * Drawing on a GtkDrawingArea
 */
enum cairo_fn {
        RECTANGLE,
        ARC,
        ARC_NEGATIVE,
        CURVE_TO,
        REL_CURVE_TO,
        LINE_TO,
        REL_LINE_TO,
        MOVE_TO,
        REL_MOVE_TO,
        CLOSE_PATH,
        SET_SOURCE_RGBA,
        SET_DASH,
        SET_LINE_CAP,
        SET_LINE_JOIN,
        SET_LINE_WIDTH,
        FILL,
        FILL_PRESERVE,
        STROKE,
        STROKE_PRESERVE,
        SHOW_TEXT,
        SET_FONT_SIZE,
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
 * The content of all GtkDrawingAreas
 */
static struct drawing {
        struct drawing *next;
        struct drawing *prev;
        GtkWidget *widget;
        struct draw_op *draw_ops;
} *drawings = NULL;

/*
 * Sets of arguments for various drawing functions
 */
struct rectangle_args {
        double x;
        double y;
        double width;
        double height;
};

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

struct set_source_rgba_args {
        GdkRGBA color;
};

struct set_dash_args {
        int num_dashes;
        double dashes[];
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

struct show_text_args {
        int len;
        char text[];
};

struct set_font_size_args {
        double size;
};

static void
draw(cairo_t *cr, enum cairo_fn op, void *op_args)
{
        switch (op) {
        case RECTANGLE: {
                struct rectangle_args *args = op_args;

                cairo_rectangle(cr, args->x, args->y, args->width, args->height);
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
        case CLOSE_PATH:
                cairo_close_path(cr);
                break;
        case SET_SOURCE_RGBA: {
                struct set_source_rgba_args *args = op_args;

                gdk_cairo_set_source_rgba(cr, &args->color);
                break;
        }
        case SET_DASH: {
                struct set_dash_args *args = op_args;

                cairo_set_dash(cr, args->dashes, args->num_dashes, 0);
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
        case FILL:
                cairo_fill(cr);
                break;
        case FILL_PRESERVE:
                cairo_fill_preserve(cr);
                break;
        case STROKE:
                cairo_stroke(cr);
                break;
        case STROKE_PRESERVE:
                cairo_stroke_preserve(cr);
                break;
        case SHOW_TEXT: {
                struct show_text_args *args = op_args;

                cairo_show_text(cr, args->text);
                break;
        }
        case SET_FONT_SIZE: {
                struct set_font_size_args *args = op_args;

                cairo_set_font_size(cr, args->size);
                break;
        }
        default:
                ABORT;
                break;
        }
}

static bool
set_draw_op(struct draw_op *op, const char *action, const char *data)
{
        if (eql(action, "rectangle")) {
                struct rectangle_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = RECTANGLE;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf", &op->id, &args->x, &args->y, &args->width, &args->height) != 5)
                        return false;
        } else if (eql(action, "arc")) {
                struct arc_args *args;
                double deg1, deg2;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = ARC;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf", &op->id, &args->x, &args->y, &args->radius, &deg1, &deg2) != 6)
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
                if (sscanf(data, "%u %lf %lf %lf %lf %lf", &op->id, &args->x, &args->y, &args->radius, &deg1, &deg2) != 6)
                        return false;
                args->angle1 = deg1 * (M_PI / 180.);
                args->angle2 = deg2 * (M_PI / 180.);
        } else if (eql(action, "curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = CURVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf %lf", &op->id, &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3) != 7)
                        return false;
        } else if (eql(action, "rel_curve_to")) {
                struct curve_to_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = REL_CURVE_TO;
                op->op_args = args;
                if (sscanf(data, "%u %lf %lf %lf %lf %lf %lf", &op->id, &args->x1, &args->y1, &args->x2, &args->y2, &args->x3, &args->y3) != 7)
                        return false;
        } else if (eql(action, "line_to")) {
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
        } else if (eql(action, "close_path")) {
                op->op = CLOSE_PATH;
                if (sscanf(data, "%u", &op->id) != 1)
                        return false;
                op->op_args = NULL;
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
        } else if (eql(action, "set_font_size")) {
                struct set_font_size_args *args;

                if ((args = malloc(sizeof(*args))) == NULL)
                        OOM_ABORT;
                op->op = SET_FONT_SIZE;
                op->op_args = args;
                if (sscanf(data, "%u %lf", &op->id, &args->size) != 2)
                        return false;
        } else
                return false;
        return true;
}

/*
 * Add another element to widget's list of drawing operations
 */
static bool
ins_draw_op(GtkWidget *widget, const char *action, const char *data)
{
        struct draw_op *op, *last_op;
        struct drawing *d;

        if ((op = malloc(sizeof(*op))) == NULL)
                OOM_ABORT;
        op->op_args = NULL;
        if (!set_draw_op(op, action, data)) {
                free(op->op_args);
                free(op);
                return false;
        }
        for (d = drawings; d != NULL; d = d->next)
                if (d->widget == widget)
                        break;
        if (d == NULL) {
                if ((d = malloc(sizeof(*d))) == NULL)
                        OOM_ABORT;
                if (drawings == NULL) {
                        drawings = d;
                        insque(d, NULL);
                } else
                        insque(d, drawings);
                d->widget = widget;
                d->draw_ops = op;
                insque(op, NULL);
        } else if (d->draw_ops == NULL) {
                d->draw_ops = op;
                insque(op, NULL);
        } else {
                for (last_op = d->draw_ops; last_op->next != NULL; last_op = last_op->next);
                insque(op, last_op);
        }
        return true;
}

/*
 * Remove all elements with the given id from widget's list of drawing
 * operations
 */
static bool
rem_draw_op(GtkWidget *widget, const char *data)
{
        struct draw_op *op, *next_op;
        struct drawing *d;
        unsigned int id;

        if (sscanf(data, "%u", &id) != 1)
                return false;
        for (d = drawings; d != NULL; d = d->next)
                if (d->widget == widget)
                        break;
        if (d != NULL) {
                op = d->draw_ops;
                while (op != NULL) {
                        next_op = op->next;
                        if (op->id == id) {
                                if (op->prev == NULL)
                                        d->draw_ops = next_op;
                                remque(op);
                                free(op->op_args);
                                free(op);
                        }
                        op = next_op;
                }
        }
        return true;
}

/*
 * Callback that draws on a GtkDrawingArea
 */
static gboolean
cb_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
        struct draw_op *op;
        struct drawing *p;

        (void)data;
        for (p = drawings; p != NULL; p = p->next)
                if (p->widget == widget)
                        for (op = p->draw_ops; op != NULL; op = op->next)
                                draw(cr, op->op, op->op_args);
        return FALSE;
}

/*
 * Change the style of the widget passed
 */
static void
update_widget_style(GObject *obj, const char *name,
                    const char *data, const char *whole_msg, GType type)
{
        GtkWidget *widget = GTK_WIDGET(obj);
        GtkStyleContext *context;
        GtkCssProvider *style_provider;
        char *style_decl;
        const char *prefix = "* {", *suffix = "}";
        size_t sz;

        (void)name;
        (void)whole_msg;
        (void)type;
        style_provider = g_object_get_data(G_OBJECT(obj), "style_provider");
        sz = strlen(prefix) + strlen(suffix) + strlen(data) + 1;
        context = gtk_widget_get_style_context(widget);
        gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(style_provider));
        if ((style_decl = malloc(sz)) == NULL)
                OOM_ABORT;
        strcpy(style_decl, prefix);
        strcat(style_decl, data);
        strcat(style_decl, suffix);
        gtk_style_context_add_provider(context,
                                       GTK_STYLE_PROVIDER(style_provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_data(style_provider, style_decl, -1, NULL);
        free(style_decl);
}

/*
 * Update various kinds of widgets according to the respective action
 * parameter
 */
static void
update_button(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkButton *button = GTK_BUTTON(obj);

        if (eql(action, "set_label"))
                gtk_button_set_label(button, data);
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

static void
update_color_button(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GtkColorChooser *chooser = GTK_COLOR_CHOOSER(obj);
        GdkRGBA color;

        if (eql(action, "set_color")) {
                gdk_rgba_parse(&color, data);
                gtk_color_chooser_set_rgba(chooser, &color);
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
                gtk_combo_box_text_insert_text(combobox, strtol(position, NULL, 10), text);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_frame(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        GtkFrame *frame = GTK_FRAME(obj);

        if (eql(action, "set_label"))
                gtk_frame_set_label(frame, data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_drawing_area(GObject *obj, const char *action,
                    const char *data, const char *whole_msg, GType type)
{
        GtkWidget *widget = GTK_WIDGET(obj);

        if (eql(action, "remove")) {
                if (!rem_draw_op(widget, data))
                        ign_cmd(type, whole_msg);
        } else if (eql(action, "refresh")) {
                gint width = gtk_widget_get_allocated_width (widget);
                gint height = gtk_widget_get_allocated_height (widget);

                gtk_widget_queue_draw_area(widget, 0, 0, width, height);
        } else if (ins_draw_op(widget, action, data));
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
update_label(GObject *obj, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        GtkLabel *label = GTK_LABEL(obj);

        if (eql(action, "set_text"))
                gtk_label_set_text(label, data);
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
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(obj);

        if (eql(action, "set_filename"))
                gtk_file_chooser_set_filename(chooser, data);
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
                                widget_name(GTK_WIDGET(dialog)), response_id);
                        break;
                }
                gtk_widget_hide(GTK_WIDGET(dialog));
        } else
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
update_notebook(GObject *obj, const char *action,
                const char *data, const char *whole_msg, GType type)
{
        GtkNotebook *notebook = GTK_NOTEBOOK(obj);

        if (eql(action, "set_current_page"))
                gtk_notebook_set_current_page(notebook, strtol(data, NULL, 10));
        else
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
        GtkRange *range = GTK_RANGE(obj);

        if (eql(action, "set_value"))
                gtk_range_set_value(range, strtod(data, NULL));
        else
                ign_cmd(type, whole_msg);
}

static void
update_spinner(GObject *obj, const char *action,
               const char *data, const char *whole_msg, GType type)
{
        GtkSpinner *spinner = GTK_SPINNER(obj);

        (void)data;
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

        if (eql(action, "push"))
                gtk_statusbar_push(statusbar, 0, data);
        else if (eql(action, "pop"))
                gtk_statusbar_pop(statusbar, 0);
        else if (eql(action, "remove_all"))
                gtk_statusbar_remove_all(statusbar, 0);
        else
                ign_cmd(type, whole_msg);
}

static void
update_switch(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkSwitch *switcher = GTK_SWITCH(obj);

        if (eql(action, "set_active"))
                gtk_switch_set_active(switcher, strtol(data, NULL, 10));
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
        GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(obj);
        if (eql(action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(toggle), data);
        else if (eql(action, "set_active"))
                gtk_toggle_button_set_active(toggle, strtol(data, NULL, 10));
        else
                ign_cmd(type, whole_msg);
}

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
        if (eql(action, "set")
            && col > -1 && col < gtk_tree_model_get_n_columns(model) &&
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
                gtk_tree_view_set_cursor(view, path, NULL, false);
        } else if (eql(action, "set_cursor") && arg0 == NULL) {
                gtk_tree_view_set_cursor(view, NULL, NULL, false);
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
                gtk_tree_view_set_cursor(view, NULL, NULL, false);
                gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(view));
                tree_model_clear(model);
        } else if (eql(action, "save") && arg0 != NULL &&
                 (save = fopen(arg0, "w")) != NULL) {
                gtk_tree_model_foreach(model, save_tree_row_msg, view);
                fclose(save);
        } else
                ign_cmd(type, whole_msg);
        free(tokens);
        gtk_tree_path_free(path);
}

static void
update_socket(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkSocket *socket = GTK_SOCKET(obj);
        Window id;
        char str[BUFLEN];

        (void)data;
        if (eql(action, "id")) {
                id = gtk_socket_get_id(socket);
                snprintf(str, BUFLEN, "%lu", id);
                send_msg(GTK_BUILDABLE(socket), "id", str, NULL);
        } else
                ign_cmd(type, whole_msg);
}

static void
update_window(GObject *obj, const char *action,
              const char *data, const char *whole_msg, GType type)
{
        GtkWindow *window = GTK_WINDOW(obj);
        int x, y;

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
                ign_cmd(type, whole_msg);
}

static void
update_sensitivity(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        (void)action;
        (void)whole_msg;
        (void)type;

        gtk_widget_set_sensitive(GTK_WIDGET(obj), strtol(data, NULL, 10));
}

static void
update_visibility(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        (void)action;
        (void)whole_msg;
        (void)type;

        gtk_widget_set_visible(GTK_WIDGET(obj), strtol(data, NULL, 10));
}

static void
fake_ui_activity(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        (void)action;
        (void)data;
        if (!GTK_IS_WIDGET(obj))
                ign_cmd(type, whole_msg);
        else if (GTK_IS_ENTRY(obj) || GTK_IS_SPIN_BUTTON(obj))
                cb(GTK_BUILDABLE(obj), "text");
        else if (GTK_IS_SCALE(obj))
                cb(GTK_BUILDABLE(obj), "value");
        else if (GTK_IS_CALENDAR(obj))
                cb(GTK_BUILDABLE(obj), "clicked");
        else if (GTK_IS_FILE_CHOOSER_BUTTON(obj))
                cb(GTK_BUILDABLE(obj), "file");
        else if (!gtk_widget_activate(GTK_WIDGET(obj)))
                ign_cmd(type, whole_msg);
}

static void
main_quit(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        (void)obj;
        (void)action;
        (void)data;
        (void)whole_msg;
        (void)type;

        gtk_main_quit();
}

static void
complain(GObject *obj, const char *action,
                 const char *data, const char *whole_msg, GType type)
{
        (void)obj;
        (void)action;
        (void)data;

        ign_cmd(type, whole_msg);
}

/*
 * Parse command pointed to by ud, and act on ui accordingly
 */
static void
update_ui(struct ui_data *ud)
{
        (ud->fn)(ud->obj, ud->action, ud->data, ud->msg, ud->type);
}

static void
free_at(void **mem)
{
        free(*mem);
}

/*
 * Keep track of loading files to avoid recursive loading of the same
 * file
 */
static bool
push_loading_file(char *fn)
{
        size_t i;

        for (i = 1; i <= newest_loading_file; i++)
                if (eql(fn, loading_files[i]))
                        return false;
        if (newest_loading_file > BUFLEN -2)
                return false;
        loading_files[++newest_loading_file] = fn;
        return true;
}

static void
pop_loading_file()
{
        if (newest_loading_file < 1)
                ABORT;
        newest_loading_file--;
}

/*
 * Parse command pointed to by ud, and act on ui accordingly; post
 * semaphore ud.msg_digested if done.  Runs once per command inside
 * gtk_main_loop()
 */
static gboolean
update_ui_in(struct ui_data *ud)
{
        update_ui(ud);
        sem_post(&ud->msg_digested);
        return G_SOURCE_REMOVE;
}

/*
 * Read lines from stream cmd and perform appropriate actions on the
 * GUI
 */
static void *
digest_msg(FILE *cmd)
{
        struct ui_data ud;
        FILE *load;             /* restoring user data */
        char *name;

        sem_init(&ud.msg_digested, 0, 0);
        for (;;) {
                char first_char = '\0';
                size_t msg_size = 32;
                int name_start = 0, name_end = 0;
                int action_start = 0, action_end = 0;
                int data_start;

                ud.type = G_TYPE_INVALID;
                if (feof(cmd))
                        break;
                if ((ud.msg = malloc(msg_size)) == NULL)
                        OOM_ABORT;
                pthread_cleanup_push((void(*)(void *))free_at, &ud.msg);
                pthread_testcancel();
                read_buf(cmd, &ud.msg, &msg_size);
                data_start = strlen(ud.msg);
                if ((ud.msg_tokens = malloc(strlen(ud.msg) + 1)) == NULL)
                        OOM_ABORT;
                pthread_cleanup_push((void(*)(void *))free_at, &ud.msg_tokens);
                strcpy(ud.msg_tokens, ud.msg);
                sscanf(ud.msg, " %c", &first_char);
                if (strlen(ud.msg) == 0 || first_char == '#') /* comment */
                        goto cleanup;
                sscanf(ud.msg_tokens,
                       " %n%*[0-9a-zA-Z_]%n:%n%*[0-9a-zA-Z_]%n%*1[ \t]%n",
                       &name_start, &name_end, &action_start, &action_end, &data_start);
                ud.msg_tokens[name_end] = ud.msg_tokens[action_end] = '\0';
                name = ud.msg_tokens + name_start;
                ud.action = ud.msg_tokens + action_start;
                if (eql(ud.action, "main_quit")) {
                        ud.fn = main_quit;
                        goto exec;
                }
                ud.data = ud.msg_tokens + data_start;
                if (eql(ud.action, "load") && strlen(ud.data) > 0 &&
                    (load = fopen(ud.data, "r")) != NULL &&
                    push_loading_file(ud.data)) {
                        digest_msg(load);
                        fclose(load);
                        pop_loading_file();
                        goto cleanup;
                }
                if ((ud.obj = (gtk_builder_get_object(builder, name))) == NULL) {
                        ud.fn = complain;
                        goto exec;
                }
                ud.type = G_TYPE_FROM_INSTANCE(ud.obj);
                if (eql(ud.action, "force"))
                        ud.fn = fake_ui_activity;
                else if (eql(ud.action, "set_sensitive"))
                        ud.fn = update_sensitivity;
                else if (eql(ud.action, "set_visible"))
                        ud.fn = update_visibility;
                else if (eql(ud.action, "style")) {
                        ud.action = name;
                        ud.fn = update_widget_style;
                } else if (ud.type == GTK_TYPE_TREE_VIEW)
                        ud.fn = update_tree_view;
                else if (ud.type == GTK_TYPE_DRAWING_AREA)
                        ud.fn = update_drawing_area;
                else if (ud.type == GTK_TYPE_LABEL)
                        ud.fn = update_label;
                else if (ud.type == GTK_TYPE_IMAGE)
                        ud.fn = update_image;
                else if (ud.type == GTK_TYPE_TEXT_VIEW)
                        ud.fn = update_text_view;
                else if (ud.type == GTK_TYPE_NOTEBOOK)
                        ud.fn = update_notebook;
                else if (ud.type == GTK_TYPE_EXPANDER)
                        ud.fn = update_expander;
                else if (ud.type == GTK_TYPE_FRAME)
                        ud.fn = update_frame;
                else if (ud.type == GTK_TYPE_BUTTON)
                        ud.fn = update_button;
                else if (ud.type == GTK_TYPE_FILE_CHOOSER_DIALOG)
                        ud.fn = update_file_chooser_dialog;
                else if (ud.type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                        ud.fn = update_file_chooser_button;
                else if (ud.type == GTK_TYPE_COLOR_BUTTON)
                        ud.fn = update_color_button;
                else if (ud.type == GTK_TYPE_FONT_BUTTON)
                        ud.fn = update_font_button;
                else if (ud.type == GTK_TYPE_PRINT_UNIX_DIALOG)
                        ud.fn = update_print_dialog;
                else if (ud.type == GTK_TYPE_SWITCH)
                        ud.fn = update_switch;
                else if (ud.type == GTK_TYPE_TOGGLE_BUTTON || ud.type == GTK_TYPE_RADIO_BUTTON || ud.type == GTK_TYPE_CHECK_BUTTON)
                        ud.fn = update_toggle_button;
                else if (ud.type == GTK_TYPE_SPIN_BUTTON || ud.type == GTK_TYPE_ENTRY)
                        ud.fn = update_entry;
                else if (ud.type == GTK_TYPE_SCALE)
                        ud.fn = update_scale;
                else if (ud.type == GTK_TYPE_PROGRESS_BAR)
                        ud.fn = update_progress_bar;
                else if (ud.type == GTK_TYPE_SPINNER)
                        ud.fn = update_spinner;
                else if (ud.type == GTK_TYPE_COMBO_BOX_TEXT)
                        ud.fn = update_combo_box_text;
                else if (ud.type == GTK_TYPE_STATUSBAR)
                        ud.fn = update_statusbar;
                else if (ud.type == GTK_TYPE_CALENDAR)
                        ud.fn = update_calendar;
                else if (ud.type == GTK_TYPE_SOCKET)
                        ud.fn = update_socket;
                else if (ud.type == GTK_TYPE_WINDOW)
                        ud.fn = update_window;
                else
                        ud.fn = complain;
        exec:
                pthread_testcancel();
                gdk_threads_add_timeout(1, (GSourceFunc)update_ui_in, &ud);
                sem_wait(&ud.msg_digested);
        cleanup:
                pthread_cleanup_pop(1); /* free ud.msg_tokens */
                pthread_cleanup_pop(1); /* free ud.msg */
        }
        return NULL;
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

        if (name != NULL && (stat(name, &sb), !S_ISFIFO(sb.st_mode)))
                if (mkfifo(name, 0666) != 0)
                        bye(EXIT_FAILURE, stderr,
                            "making fifo: %s\n", strerror(errno));
        switch (mode[0]) {
        case 'r':
                bufmode = _IONBF;
                if (name == NULL)
                        s = stdin;
                else {
                        fd = open(name, O_RDWR | O_NONBLOCK);
                        if (fd < 0)
                                bye(EXIT_FAILURE, stderr,
                                    "opening fifo: %s\n", strerror(errno));
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
                bye(EXIT_FAILURE, stderr, "opening fifo: %s\n", strerror(errno));
        else
                setvbuf(s, NULL, bufmode, 0);
        return s;
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

/*
 * Callback that forwards a modification of a tree view cell to the
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
        send_tree_cell_msg(model, path_s, &iter, GPOINTER_TO_INT(col),
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
        const char *xpath_id = widget_name(t_col);
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
        snprintf((char *)xpath, xpath_len, "%s%s%s%d%s%s|%s%s%s%d%s%s",
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
                        gtk_builder_get_object(builder, (char *)renderer_name));
                if (m_col_s) {
                        g_object_set_data(G_OBJECT(*renderer), "col_number",
                                          GINT_TO_POINTER(strtol((char *)m_col_s,
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
                name = widget_name(obj);
        if (type == GTK_TYPE_TREE_VIEW_COLUMN) {
                gboolean editable = FALSE;
                GtkTreeView *view;
                GtkTreeModel *model;
                GtkCellRenderer *renderer;
                int i;

                g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
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
                /* Buttons associated with (and part of) a GtkDialog.
                 * (We shun response ids which could be returned from
                 * gtk_dialog_run() because that would require the
                 * user to define those response ids in Glade,
                 * numerically */
                else if ((suffix = strstr(name, "_cancel")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                        if (eql(widget_name(obj2), MAIN_WIN))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                else if ((suffix = strstr(name, "_ok")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name))) {
                        if (GTK_IS_FILE_CHOOSER_DIALOG(obj2))
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                        else  /* generic button */
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
                        if (eql(widget_name(obj2), MAIN_WIN))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                } else if ((suffix = strstr(name, "_apply")) != NULL &&
                           GTK_IS_FILE_CHOOSER_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                else  /* generic button */
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
        } else if (GTK_IS_MENU_ITEM(obj))
                if ((suffix = strstr(name, "_invoke")) != NULL &&
                    GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name)))
                        g_signal_connect_swapped(obj, "activate", G_CALLBACK(gtk_widget_show), obj2);
                else
                        g_signal_connect(obj, "activate", G_CALLBACK(cb), "active");
        else if (GTK_IS_WINDOW(obj))
                if (eql(name, MAIN_WIN))
                        g_signal_connect(obj, "delete-event", G_CALLBACK(gtk_main_quit), NULL);
                else
                        g_signal_connect(obj, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        else if (type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                g_signal_connect(obj, "file-set", G_CALLBACK(cb), "file");
        else if (type == GTK_TYPE_COLOR_BUTTON)
                g_signal_connect(obj, "color-set", G_CALLBACK(cb), "color");
        else if (type == GTK_TYPE_FONT_BUTTON)
                g_signal_connect(obj, "font-set", G_CALLBACK(cb), "font");
        else if (type == GTK_TYPE_SWITCH)
                g_signal_connect(obj, "notify::active", G_CALLBACK(cb), NULL);
        else if (type == GTK_TYPE_TOGGLE_BUTTON || type == GTK_TYPE_RADIO_BUTTON || type == GTK_TYPE_CHECK_BUTTON)
                g_signal_connect(obj, "toggled", G_CALLBACK(cb), NULL);
        else if (type == GTK_TYPE_SPIN_BUTTON || type == GTK_TYPE_ENTRY)
                g_signal_connect(obj, "changed", G_CALLBACK(cb), "text");
        else if (type == GTK_TYPE_SCALE)
                g_signal_connect(obj, "value-changed", G_CALLBACK(cb), "value");
        else if (type == GTK_TYPE_CALENDAR) {
                g_signal_connect(obj, "day-selected-double-click", G_CALLBACK(cb), "doubleclicked");
                g_signal_connect(obj, "day-selected", G_CALLBACK(cb), "clicked");
        } else if (type == GTK_TYPE_TREE_SELECTION)
                g_signal_connect(obj, "changed", G_CALLBACK(cb), "clicked");
        else if (type == GTK_TYPE_SOCKET) {
                g_signal_connect(obj, "plug-added", G_CALLBACK(cb), "plug-added");
                g_signal_connect(obj, "plug-removed", G_CALLBACK(cb_true), "plug-removed");
        } else if (type == GTK_TYPE_DRAWING_AREA)
                g_signal_connect(obj, "draw", G_CALLBACK(cb_draw), NULL);
}

/*
 * We keep a style provider with each widget
 */
static void
add_widget_style_provider(gpointer *obj, void *data)
{
        GtkStyleContext *context;
        GtkCssProvider *style_provider;

        (void)data;
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
        g_slist_foreach(objects, (GFunc)connect_widget_signals, ui_file);
        g_slist_foreach(objects, (GFunc)add_widget_style_provider, NULL);
        g_slist_free(objects);
}

int
main(int argc, char *argv[])
{
        char opt;
        char *in_fifo = NULL, *out_fifo = NULL, *ui_file = NULL;
        char *xid_s = NULL, xid_s2[BUFLEN];
        Window xid;
        GtkWidget *plug, *body;
        pthread_t receiver;
        GError *error = NULL;
        GObject *main_window = NULL;

        /* Disable runtime GLIB deprecation warnings: */
        setenv("G_ENABLE_DIAGNOSTIC", "0", 0);
        in = NULL;
        out = NULL;
        save = NULL;
        newest_loading_file = 0;
        while ((opt = getopt(argc, argv, "he:i:o:u:GV")) != -1) {
                switch (opt) {
                case 'e': xid_s = optarg; break;
                case 'i': in_fifo = optarg; break;
                case 'o': out_fifo = optarg; break;
                case 'u': ui_file = optarg; break;
                case 'G': bye(EXIT_SUCCESS, stdout, "GTK+ v%d.%d.%d (running v%d.%d.%d)\n",
                              GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
                              gtk_get_major_version(), gtk_get_minor_version(),
                              gtk_get_micro_version());
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
        if (ui_file == NULL)
                ui_file = "pipeglade.ui";
        gtk_init(0, NULL);
        builder = gtk_builder_new();
        if (gtk_builder_add_from_file(builder, ui_file, &error) == 0)
                bye(EXIT_FAILURE, stderr, "%s\n", error->message);
        in = fifo(in_fifo, "r");
        out = fifo(out_fifo, "w");
        pthread_create(&receiver, NULL, (void *(*)(void *))digest_msg, in);
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
