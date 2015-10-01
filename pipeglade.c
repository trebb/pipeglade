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
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <search.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "4.1.0"
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

#define OOM_ABORT                                       \
        {                                               \
                fprintf(stderr,                         \
                        "out of memory: %s (%s:%d)\n",  \
                        __func__, __FILE__, __LINE__);  \
                abort();                                \
        }

static FILE *in;
static FILE *out;
struct ui_data {
        GtkBuilder *builder;
        size_t msg_size;
        char *msg;
        bool msg_digested;
};

/*
 * Print a formatted message to stream s and give up with status
 */
static void
bye(int status, FILE *s, const char *fmt, ...)
{
        va_list ap0, ap;

        va_start(ap0, fmt);
        va_copy(ap, ap0);
        vfprintf(s, fmt, ap);
        va_end(ap);
        va_end(ap0);
        exit(status);
}

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

/*
 * Send GUI feedback to global stream "out".  The message format is
 * "<origin>:<tag> <data ...>".  The variadic arguments are
 * strings; last argument must be NULL.  We're being patient with
 * receivers which may intermittently close their end of the fifo, and
 * make a couple of retries if an error occurs.
 */
static void
send_msg(GtkBuildable *obj, const char *tag, ...)
{
        va_list ap;
        char *data;
        long nsec;

        for (nsec = 1e6; nsec < 1e9; nsec <<= 3) {
                va_start(ap, tag);
                fprintf(out, "%s:%s ", widget_name(obj), tag);
                while ((data = va_arg(ap, char *)) != NULL) {
                        size_t i = 0;
                        char c;

                        while ((c = data[i++]) != '\0')
                                if (c == '\\')
                                        fprintf(out, "\\\\");
                                else if (c == '\n')
                                        fprintf(out, "\\n");
                                else
                                        putc(c, out);
                }
                va_end(ap);
                putc('\n', out);
                if (ferror(out)) {
                        fprintf(stderr, "send error; retrying\n");
                        clearerr(out);
                        nanosleep(&(struct timespec){0, nsec}, NULL);
                        putc('\n', out);
                } else
                        break;
        }
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
        send_msg(obj, "text", gtk_text_buffer_get_text(user_data, &a, &b, FALSE), NULL);
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
        send_msg(obj, "text", gtk_text_buffer_get_text(user_data, &a, &b, FALSE), NULL);
}

/*
 * send_tree_row_msg serves as an argument for
 * gtk_tree_selection_selected_foreach()
 */
static void
send_tree_row_msg(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, GtkBuildable *obj)
{
        char *path_s = gtk_tree_path_to_string(path);
        int col;

        for (col = 0; col < gtk_tree_model_get_n_columns(model); col++) {
                GValue value = G_VALUE_INIT;
                GType col_type;
                char str[BUFLEN];

                gtk_tree_model_get_value(model, iter, col, &value);
                col_type = gtk_tree_model_get_column_type(model, col);
                switch (col_type) {
                case G_TYPE_INT:
                        snprintf(str, BUFLEN, " %d %d", col, g_value_get_int(&value));
                        send_msg(obj, "gint", path_s, str, NULL);
                        break;
                case G_TYPE_LONG:
                        snprintf(str, BUFLEN, " %d %ld", col, g_value_get_long(&value));
                        send_msg(obj, "glong", path_s, str, NULL);
                        break;
                case G_TYPE_INT64:
                        snprintf(str, BUFLEN, " %d %" PRId64, col, g_value_get_int64(&value));
                        send_msg(obj, "gint64", path_s, str, NULL);
                        break;
                case G_TYPE_UINT:
                        snprintf(str, BUFLEN, " %d %u", col, g_value_get_uint(&value));
                        send_msg(obj, "guint", path_s, str, NULL);
                        break;
                case G_TYPE_ULONG:
                        snprintf(str, BUFLEN, " %d %lu", col, g_value_get_ulong(&value));
                        send_msg(obj, "gulong", path_s, str, NULL);
                        break;
                case G_TYPE_UINT64:
                        snprintf(str, BUFLEN, " %d %" PRIu64, col, g_value_get_uint64(&value));
                        send_msg(obj, "guint64", path_s, str, NULL);
                        break;
                case G_TYPE_BOOLEAN:
                        snprintf(str, BUFLEN, " %d %d", col, g_value_get_boolean(&value));
                        send_msg(obj, "gboolean", path_s, str, NULL);
                        break;
                case G_TYPE_FLOAT:
                        snprintf(str, BUFLEN, " %d %f", col, g_value_get_float(&value));
                        send_msg(obj, "gfloat", path_s, str, NULL);
                        break;
                case G_TYPE_DOUBLE:
                        snprintf(str, BUFLEN, " %d %f", col, g_value_get_double(&value));
                        send_msg(obj, "gdouble", path_s, str, NULL);
                        break;
                case G_TYPE_STRING:
                        snprintf(str, BUFLEN, " %d ", col);
                        send_msg(obj, "gchararray", path_s, str, g_value_get_string(&value), NULL);
                        break;
                default:
                        fprintf(stderr, "column %d not implemented: %s\n", col, G_VALUE_TYPE_NAME(&value));
                        break;
                }
                g_value_unset(&value);
        }
        g_free(path_s);
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
 * Store a line from stream into buf, which should have been malloc'd
 * to bufsize.  Enlarge buf and bufsize if necessary.
 */
static size_t
read_buf(FILE *stream, char **buf, size_t *bufsize)
{
        size_t i = 0;
        int c;

        for (;;) {
                c = getc(stream);
                if (c == EOF) {
                        i = 0;
                        nanosleep(&(struct timespec){0, 1e7}, NULL);
                        continue;
                }
                if (c == '\n')
                        break;
                if (i >= *bufsize - 1)
                        if ((*buf = realloc(*buf, *bufsize = *bufsize * 2)) == NULL)
                                OOM_ABORT;
                if (c == '\\')
                        switch (c = getc(stream)) {
                        case 'n': (*buf)[i++] = '\n'; break;
                        case 'r': (*buf)[i++] = '\r'; break;
                        default: (*buf)[i++] = c; break;
                        }
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
                abort();
                break;
        }
}

static bool
set_draw_op(struct draw_op *op, char* action, char *data)
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
                char *next, *end;

                if (sscanf(data, "%u %n", &op->id, &d_start) < 1)
                        return false;
                next = end = data + d_start;
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
                for (i = 0, next = data + d_start; i < n; i++, next = end) {
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
ins_draw_op(GtkWidget *widget, char *action, char *data)
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
rem_draw_op(GtkWidget *widget, char *data)
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
 * One style provider for each widget
 */
struct style_provider {
        struct style_provider *next;
        struct style_provider *prev;
        const char *name;
        GtkCssProvider *provider;
        char *style_decl;
} *widget_style_providers = NULL;

/*
 * Change the style of the widget passed
 */
static void
update_widget_style(GtkWidget *widget, const char *name , const char *data)
{
        GtkStyleContext *context;
        struct style_provider *sp;
        const char *prefix = "* {", *suffix = "}";
        size_t sz;

        sz = strlen(prefix) + strlen(suffix) + strlen(data) + 1;
        context = gtk_widget_get_style_context(widget);
        for (sp = widget_style_providers; !eql(name, sp->name); sp = sp->next);
        gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(sp->provider));
        free(sp->style_decl);
        if ((sp->style_decl = malloc(sz)) == NULL)
                OOM_ABORT;
        strcpy(sp->style_decl, prefix);
        strcat(sp->style_decl, data);
        strcat(sp->style_decl, suffix);
        gtk_style_context_add_provider(context,
                                       GTK_STYLE_PROVIDER(sp->provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_data(sp->provider, sp->style_decl, -1, NULL);
}

/*
 * Update various kinds of widgets according to the respective action
 * parameter
 */
static void
update_button(GtkButton *button, const char *action,
              const char *data, const char *whole_msg)
{
        if (eql(action, "set_label"))
                gtk_button_set_label(button, data);
        else
                ign_cmd(GTK_TYPE_BUTTON, whole_msg);
}

static void
update_calendar(GtkCalendar *calendar, const char *action,
                const char *data, const char *whole_msg)
{
        int year = 0, month = 0, day = 0;

        if (eql(action, "select_date")) {
                sscanf(data, "%d-%d-%d", &year, &month, &day);
                if (month > -1 && month <= 11 && day > 0 && day <= 31) {
                        gtk_calendar_select_month(calendar, --month, year);
                        gtk_calendar_select_day(calendar, day);
                } else
                        ign_cmd(GTK_TYPE_CALENDAR, whole_msg);
        } else if (eql(action, "mark_day")) {
                day = strtol(data, NULL, 10);
                if (day > 0 && day <= 31)
                        gtk_calendar_mark_day(calendar, day);
                else
                        ign_cmd(GTK_TYPE_CALENDAR, whole_msg);
        } else if (eql(action, "clear_marks"))
                gtk_calendar_clear_marks(calendar);
        else
                ign_cmd(GTK_TYPE_CALENDAR, whole_msg);
}

static void
update_color_button(GtkColorChooser *chooser, const char *action,
                    const char *data, const char *whole_msg)
{
        GdkRGBA color;

        if (eql(action, "set_color")) {
                gdk_rgba_parse(&color, data);
                gtk_color_chooser_set_rgba(chooser, &color);
        } else
                ign_cmd(GTK_TYPE_COLOR_BUTTON, whole_msg);
}

static void
update_combo_box_text(GtkComboBoxText *combobox, const char *action,
                      char *data, const char *whole_msg)
{
        if (eql(action, "prepend_text"))
                gtk_combo_box_text_prepend_text(combobox, data);
        else if (eql(action, "append_text"))
                gtk_combo_box_text_append_text(combobox, data);
        else if (eql(action, "remove"))
                gtk_combo_box_text_remove(combobox, strtol(data, NULL, 10));
        else if (eql(action, "insert_text")) {
                char *position = strtok(data, WHITESPACE);
                char *text = strtok(NULL, WHITESPACE);

                gtk_combo_box_text_insert_text(combobox, strtol(position, NULL, 10), text);
        } else
                ign_cmd(GTK_TYPE_COMBO_BOX_TEXT, whole_msg);
}

static void
update_frame(GtkFrame *frame, const char *action,
             const char *data, const char *whole_msg)
{
        if (eql(action, "set_label"))
                gtk_frame_set_label(frame, data);
        else
                ign_cmd(GTK_TYPE_FRAME, whole_msg);
}

static void
update_drawing_area(GtkWidget *widget, char *action,
                    char *data, const char *whole_msg)
{
        if (eql(action, "remove")) {
                if (!rem_draw_op(widget, data))
                        ign_cmd(GTK_TYPE_DRAWING_AREA, whole_msg);
        } else if (eql(action, "refresh")) {
                gint width = gtk_widget_get_allocated_width (widget);
                gint height = gtk_widget_get_allocated_height (widget);

                gtk_widget_queue_draw_area(widget, 0, 0, width, height);
        } else if (ins_draw_op(widget, action, data));
        else
                ign_cmd(GTK_TYPE_DRAWING_AREA, whole_msg);
}

static void
update_entry(GtkEntry *entry, const char *action,
             const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_text"))
                gtk_entry_set_text(entry, data);
        else if (eql(action, "set_placeholder_text"))
                gtk_entry_set_placeholder_text(entry, data);
        else
                ign_cmd(type, whole_msg);
}

static void
update_label(GtkLabel *label, const char *action,
             const char *data, const char *whole_msg)
{
        if (eql(action, "set_text"))
                gtk_label_set_text(label, data);
        else
                ign_cmd(GTK_TYPE_LABEL, whole_msg);
}

static void
update_expander(GtkExpander *expander, const char *action,
                const char *data, const char *whole_msg)
{
        if (eql(action, "set_expanded"))
                gtk_expander_set_expanded(expander, strtol(data, NULL, 10));
        else if (eql(action, "set_label"))
                gtk_expander_set_label(expander, data);
        else
                ign_cmd(GTK_TYPE_EXPANDER, whole_msg);
}

static void
update_file_chooser_button(GtkFileChooser *chooser, const char *action,
                           const char *data, const char *whole_msg)
{
        if (eql(action, "set_filename"))
                gtk_file_chooser_set_filename(chooser, data);
        else
                ign_cmd(GTK_TYPE_FILE_CHOOSER_BUTTON, whole_msg);
}

static void
update_file_chooser_dialog(GtkFileChooser *chooser, const char *action,
                           const char *data, const char *whole_msg)
{
        if (eql(action, "set_filename"))
                gtk_file_chooser_set_filename(chooser, data);
        else if (eql(action, "set_current_name"))
                gtk_file_chooser_set_current_name(chooser, data);
        else
                ign_cmd(GTK_TYPE_FILE_CHOOSER_DIALOG, whole_msg);
}

static void
update_font_button(GtkFontButton *font_button, const char *action,
                   const char *data, const char *whole_msg)
{
        if (eql(action, "set_font_name"))
                gtk_font_button_set_font_name(font_button, data);
        else
                ign_cmd(GTK_TYPE_FONT_BUTTON, whole_msg);
}

static void
update_print_dialog(GtkPrintUnixDialog *dialog, const char *action,
                    const char *data, const char *whole_msg)
{
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
                                ign_cmd(GTK_TYPE_PRINT_UNIX_DIALOG, whole_msg);
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
                ign_cmd(GTK_TYPE_PRINT_UNIX_DIALOG, whole_msg);
}

static void
update_image(GtkImage *image, const char *action,
             const char *data, const char *whole_msg)
{
        GtkIconSize size;

        gtk_image_get_icon_name(image, NULL, &size);
        if (eql(action, "set_from_file"))
                gtk_image_set_from_file(image, data);
        else if (eql(action, "set_from_icon_name"))
                gtk_image_set_from_icon_name(image, data, size);
        else
                ign_cmd(GTK_TYPE_IMAGE, whole_msg);
}

static void
update_notebook(GtkNotebook *notebook, const char *action,
                const char *data, const char *whole_msg)
{
        if (eql(action, "set_current_page"))
                gtk_notebook_set_current_page(notebook, strtol(data, NULL, 10));
        else
                ign_cmd(GTK_TYPE_NOTEBOOK, whole_msg);
}

static void
update_progress_bar(GtkProgressBar *progressbar, const char *action,
                    const char *data, const char *whole_msg)
{
        if (eql(action, "set_text"))
                gtk_progress_bar_set_text(progressbar, *data == '\0' ? NULL : data);
        else if (eql(action, "set_fraction"))
                gtk_progress_bar_set_fraction(progressbar, strtod(data, NULL));
        else
                ign_cmd(GTK_TYPE_PROGRESS_BAR, whole_msg);
}

static void
update_scale(GtkRange *range, const char *action,
             const char *data, const char *whole_msg)
{
        if (eql(action, "set_value"))
                gtk_range_set_value(range, strtod(data, NULL));
        else
                ign_cmd(GTK_TYPE_SCALE, whole_msg);
}

static void
update_spinner(GtkSpinner *spinner, const char *action, const char *whole_msg)
{
        if (eql(action, "start"))
                gtk_spinner_start(spinner);
        else if (eql(action, "stop"))
                gtk_spinner_stop(spinner);
        else
                ign_cmd(GTK_TYPE_SPINNER, whole_msg);
}

static void
update_statusbar(GtkStatusbar *statusbar, const char *action,
                 const char *data, const char *whole_msg)
{
        if (eql(action, "push"))
                gtk_statusbar_push(statusbar, 0, data);
        else if (eql(action, "pop"))
                gtk_statusbar_pop(statusbar, 0);
        else if (eql(action, "remove_all"))
                gtk_statusbar_remove_all(statusbar, 0);
        else
                ign_cmd(GTK_TYPE_STATUSBAR, whole_msg);
}

static void
update_switch(GtkSwitch *switcher, const char *action,
              const char *data, const char *whole_msg)
{
        if (eql(action, "set_active"))
                gtk_switch_set_active(switcher, strtol(data, NULL, 10));
        else
                ign_cmd(GTK_TYPE_SWITCH, whole_msg);
}

static void
update_text_view(GtkTextView *view, const char *action,
                 const char *data, const char *whole_msg)
{
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
                        gtk_text_buffer_get_iter_at_offset(textbuf, &a, strtol(data, NULL, 10));
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "place_cursor_at_line")) {
                gtk_text_buffer_get_iter_at_line(textbuf, &a, strtol(data, NULL, 10));
                gtk_text_buffer_place_cursor(textbuf, &a);
        } else if (eql(action, "scroll_to_cursor"))
                gtk_text_view_scroll_to_mark(view, gtk_text_buffer_get_insert(textbuf), 0., 0, 0., 0.);
        else
                ign_cmd(GTK_TYPE_TEXT_VIEW, whole_msg);
}

static void
update_toggle_button(GtkToggleButton *toggle, const char *action,
                     const char *data, const char *whole_msg, GType type)
{
        if (eql(action, "set_label"))
                gtk_button_set_label(GTK_BUTTON(toggle), data);
        else if (eql(action, "set_active"))
                gtk_toggle_button_set_active(toggle, strtol(data, NULL, 10));
        else
                ign_cmd(type, whole_msg);
}

static void
update_tree_view(GtkTreeView *view, const char *action,
                 const char *data, const char *whole_msg)
{
        GtkTreeModel *model = gtk_tree_view_get_model(view);
        GtkListStore *store = GTK_LIST_STORE(model);
        GtkTreeIter iter0, iter1;
        char *tokens, *arg0_s, *arg1_s, *arg2_s, *endptr;
        int arg0_n = 0, arg1_n = 0;
        bool arg0_n_valid = false, arg1_n_valid = false;
        bool iter0_valid = false, iter1_valid = false;

        if ((tokens = malloc(strlen(data) + 1)) == NULL)
                OOM_ABORT;
        strcpy(tokens, data);
        arg0_s = strtok(tokens, WHITESPACE);
        arg1_s = strtok(NULL, WHITESPACE);
        arg2_s = strtok(NULL, "\n");
        errno = 0;
        endptr = NULL;
        iter0_valid =
                arg0_s != NULL &&
                (arg0_n_valid = (arg0_n = strtol(arg0_s, &endptr, 10)) >= 0 &&
                 errno == 0 && endptr != arg0_s) &&
                gtk_tree_model_get_iter_from_string(model, &iter0, arg0_s);
        errno = 0;
        endptr = NULL;
        iter1_valid =
                arg1_s != NULL &&
                (arg1_n_valid = (arg1_n = strtol(arg1_s, &endptr, 10)) >= 0 &&
                 errno == 0 && endptr != arg1_s) &&
                gtk_tree_model_get_iter_from_string(model, &iter1, arg1_s);
        if (eql(action, "set") && iter0_valid && arg1_n_valid &&
            arg1_n < gtk_tree_model_get_n_columns(model)) {
                GType col_type = gtk_tree_model_get_column_type(model, arg1_n);
                long long int n;
                double d;

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
                        n = strtoll(arg2_s, &endptr, 10);
                        if (!errno && endptr != arg2_s)
                                gtk_list_store_set(store, &iter0, arg1_n, n, -1);
                        else
                                ign_cmd(GTK_TYPE_TREE_VIEW, whole_msg);
                        break;
                case G_TYPE_FLOAT:
                case G_TYPE_DOUBLE:
                        errno = 0;
                        endptr = NULL;
                        d = strtod(arg2_s, &endptr);
                        if (!errno && endptr != arg2_s)
                                gtk_list_store_set(store, &iter0, arg1_n, d, -1);
                        else
                                ign_cmd(GTK_TYPE_TREE_VIEW, whole_msg);
                        gtk_list_store_set(store, &iter0, arg1_n, strtod(arg2_s, NULL), -1);
                        break;
                case G_TYPE_STRING:
                        gtk_list_store_set(store, &iter0, arg1_n, arg2_s, -1);
                        break;
                default:
                        fprintf(stderr, "column %d: %s not implemented\n", arg1_n, g_type_name(col_type));
                        break;
                }
        } else if (eql(action, "scroll") && arg0_n_valid && arg1_n_valid)
                gtk_tree_view_scroll_to_cell (view,
                                              gtk_tree_path_new_from_string(arg0_s),
                                              gtk_tree_view_get_column(view, arg1_n),
                                              0, 0., 0.);
        else if (eql(action, "insert_row"))
                if (eql(arg0_s, "end"))
                        gtk_list_store_insert_before(store, &iter1, NULL);
                else if (iter0_valid)
                        gtk_list_store_insert_before(store, &iter1, &iter0);
                else
                        ign_cmd(GTK_TYPE_TREE_VIEW, whole_msg);
        else if (eql(action, "move_row") && iter0_valid)
                if (eql(arg1_s, "end"))
                        gtk_list_store_move_before(store, &iter0, NULL);
                else if (iter1_valid)
                        gtk_list_store_move_before(store, &iter0, &iter1);
                else
                        ign_cmd(GTK_TYPE_TREE_VIEW, whole_msg);
        else if (eql(action, "remove_row") && iter0_valid)
                gtk_list_store_remove(store, &iter0);
        else
                ign_cmd(GTK_TYPE_TREE_VIEW, whole_msg);
        free(tokens);
}

static void
update_socket(GtkSocket *socket, const char *action,
              const char *data, const char *whole_msg)
{
        Window id;
        char str[BUFLEN];

        (void)data;
        if (eql(action, "id")) {
                id = gtk_socket_get_id(socket);
                snprintf(str, BUFLEN, "%lu", id);
                send_msg(GTK_BUILDABLE(socket), "id", str, NULL);
        } else
                ign_cmd(GTK_TYPE_SOCKET, whole_msg);
}

static void
update_window(GtkWindow *window, const char *action,
              const char *data, const char *whole_msg)
{
        if (eql(action, "set_title"))
                gtk_window_set_title(window, data);
        else
                ign_cmd(GTK_TYPE_WINDOW, whole_msg);
}

static void
fake_ui_activity(GObject *obj, const char *whole_msg, GType type)
{
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

/*
 * Parse command pointed to by ud, and act on ui accordingly.  Set
 * ud->digested = true if done.  Runs once per command inside
 * gtk_main_loop()
 */
static gboolean
update_ui(struct ui_data *ud)
{
        char name[ud->msg_size], action[ud->msg_size];
        char *data;
        int data_start = strlen(ud->msg);
        GObject *obj = NULL;
        GType type = G_TYPE_INVALID;

        name[0] = action[0] = '\0';
        sscanf(ud->msg,
               " %[0-9a-zA-Z_]:%[0-9a-zA-Z_]%*1[ \t]%n",
               name, action, &data_start);
        if (eql(action, "main_quit")) {
                gtk_main_quit();
                goto done;
        }
        if ((obj = (gtk_builder_get_object(ud->builder, name))) == NULL) {
                ign_cmd(type, ud->msg);
                goto done;
        }
        type = G_TYPE_FROM_INSTANCE(obj);
        data = ud->msg + data_start;
        if (eql(action, "force"))
                fake_ui_activity(obj, ud->msg, type);
        else if (eql(action, "set_sensitive"))
                gtk_widget_set_sensitive(GTK_WIDGET(obj), strtol(data, NULL, 10));
        else if (eql(action, "set_visible"))
                gtk_widget_set_visible(GTK_WIDGET(obj), strtol(data, NULL, 10));
        else if (eql(action, "style"))
                update_widget_style(GTK_WIDGET(obj), name, data);
        else if (type == GTK_TYPE_LABEL)
                update_label(GTK_LABEL(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_IMAGE)
                update_image(GTK_IMAGE(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_TEXT_VIEW)
                update_text_view(GTK_TEXT_VIEW(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_NOTEBOOK)
                update_notebook(GTK_NOTEBOOK(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_EXPANDER)
                update_expander(GTK_EXPANDER(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_FRAME)
                update_frame(GTK_FRAME(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_BUTTON)
                update_button(GTK_BUTTON(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_FILE_CHOOSER_DIALOG)
                update_file_chooser_dialog(GTK_FILE_CHOOSER(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_FILE_CHOOSER_BUTTON)
                update_file_chooser_button(GTK_FILE_CHOOSER(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_COLOR_BUTTON)
                update_color_button(GTK_COLOR_CHOOSER(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_FONT_BUTTON)
                update_font_button(GTK_FONT_BUTTON(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_PRINT_UNIX_DIALOG)
                update_print_dialog(GTK_PRINT_UNIX_DIALOG(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_SWITCH)
                update_switch(GTK_SWITCH(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_TOGGLE_BUTTON || type == GTK_TYPE_RADIO_BUTTON || type == GTK_TYPE_CHECK_BUTTON)
                update_toggle_button(GTK_TOGGLE_BUTTON(obj), action, data, ud->msg, type);
        else if (type == GTK_TYPE_SPIN_BUTTON || type == GTK_TYPE_ENTRY)
                update_entry(GTK_ENTRY(obj), action, data, ud->msg, type);
        else if (type == GTK_TYPE_SCALE)
                update_scale(GTK_RANGE(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_PROGRESS_BAR)
                update_progress_bar(GTK_PROGRESS_BAR(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_SPINNER)
                update_spinner(GTK_SPINNER(obj), action, ud->msg);
        else if (type == GTK_TYPE_COMBO_BOX_TEXT)
                update_combo_box_text(GTK_COMBO_BOX_TEXT(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_STATUSBAR)
                update_statusbar(GTK_STATUSBAR(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_CALENDAR)
                update_calendar(GTK_CALENDAR(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_TREE_VIEW)
                update_tree_view(GTK_TREE_VIEW(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_DRAWING_AREA)
                update_drawing_area(GTK_WIDGET(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_SOCKET)
                update_socket(GTK_SOCKET(obj), action, data, ud->msg);
        else if (type == GTK_TYPE_WINDOW)
                update_window(GTK_WINDOW(obj), action, data, ud->msg);
        else
                ign_cmd(type, ud->msg);
done:
        ud->msg_digested = true;
        return G_SOURCE_REMOVE;
}

static void
free_at(void **mem)
{
        free(*mem);
}

/*
 * Read lines from global stream "in" and perform the appropriate
 * actions on the GUI
 */
static void *
digest_msg(void *builder)
{
        for (;;) {
                char first_char = '\0';
                struct ui_data ud;

                if ((ud.msg = malloc(ud.msg_size = 32)) == NULL )
                        OOM_ABORT;
                pthread_cleanup_push((void(*)(void *))free_at, &ud.msg);
                pthread_testcancel();
                read_buf(in, &ud.msg, &ud.msg_size);
                sscanf(ud.msg, " %c", &first_char);
                ud.builder = builder;
                if (first_char != '#') {
                        ud.msg_digested = false;
                        pthread_testcancel();
                        gdk_threads_add_timeout(1, (GSourceFunc)update_ui, &ud);
                        while (!ud.msg_digested)
                                nanosleep(&(struct timespec){0, 1e6}, NULL);
                }
                pthread_cleanup_pop(1);
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
        FILE *stream = NULL;

        if (name != NULL && (stat(name, &sb), !S_ISFIFO(sb.st_mode)))
                if (mkfifo(name, 0666) != 0)
                        bye(EXIT_FAILURE, stderr,
                            "making fifo: %s\n", strerror(errno));
        switch (mode[0]) {
        case 'r':
                if (name == NULL)
                        stream = stdin;
                else {
                        fd = open(name, O_RDWR | O_NONBLOCK);
                        if (fd < 0)
                                bye(EXIT_FAILURE, stderr,
                                    "opening fifo: %s\n", strerror(errno));
                        stream = fdopen(fd, "r");
                }
                break;
        case 'w':
                if (name == NULL)
                        stream = stdout;
                else {
                        /* fopen blocks if there is no reader, so here is one */
                        fd = open(name, O_RDONLY | O_NONBLOCK);
                        if (fd < 0)
                                bye(EXIT_FAILURE, stderr,
                                    "opening fifo: %s\n", strerror(errno));
                        stream = fopen(name, "w");
                        /* unblocking done */
                        close(fd);
                }
                break;
        default:
                abort();
                break;
        }
        if (stream == NULL)
                bye(EXIT_FAILURE, stderr, "opening fifo: %s\n", strerror(errno));
        else
                setvbuf(stream, NULL, _IOLBF, 0);
        return stream;
}

/*
 * Remove suffix from name; find the object named like this
 */
static GObject *
obj_sans_suffix(const char *suffix, const char *name, gpointer *builder)
{
        int str_l;
        char str[BUFLEN + 1] = {'\0'};

        str_l = suffix - name;
        strncpy(str, name, str_l < BUFLEN ? str_l : BUFLEN);
        return gtk_builder_get_object(GTK_BUILDER(builder), str);
}

static void
connect_widget_signals(gpointer *obj, gpointer *builder)
{
        const char *name = NULL;
        char *suffix = NULL;
        GObject *obj2;
        GType type = G_TYPE_INVALID;

        type = G_TYPE_FROM_INSTANCE(obj);
        if (GTK_IS_BUILDABLE(obj))
                name = widget_name(obj);
        if (type == GTK_TYPE_TREE_VIEW_COLUMN)
                g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
        else if (type == GTK_TYPE_BUTTON) {
                /* Button associated with a GtkTextView. */
                if ((suffix = strstr(name, "_send_text")) != NULL &&
                    GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                else if ((suffix = strstr(name, "_send_selection")) != NULL &&
                         GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text_selection),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                /* Buttons associated with (and part of) a GtkDialog.
                 * (We shun response ids which could be returned from
                 * gtk_dialog_run() because that would require the
                 * user to define those response ids in Glade,
                 * numerically */
                else if ((suffix = strstr(name, "_cancel")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
                        if (eql(widget_name(obj2), MAIN_WIN))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                else if ((suffix = strstr(name, "_ok")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder))) {
                        if (GTK_IS_FILE_CHOOSER_DIALOG(obj2))
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                        else  /* generic button */
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
                        if (eql(widget_name(obj2), MAIN_WIN))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect_swapped(obj, "clicked", G_CALLBACK(gtk_widget_hide), obj2);
                } else if ((suffix = strstr(name, "_apply")) != NULL &&
                           GTK_IS_FILE_CHOOSER_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect_swapped(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                else  /* generic button */
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
        }
        else if (GTK_IS_MENU_ITEM(obj))
                if ((suffix = strstr(name, "_invoke")) != NULL &&
                    GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
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
 * We keep a list of one widget style provider for each widget
 */
static void
add_widget_style_provider(gpointer *obj, void *data)
{
        const char *name = NULL;
        struct style_provider *sp = NULL, *last_sp = NULL;
        GtkStyleContext *context;

        (void)data;
        if (!GTK_IS_WIDGET(obj))
                return;
        if ((sp = malloc(sizeof(struct style_provider))) == NULL)
                OOM_ABORT;
        name = widget_name(obj);
        sp->name = name;
        sp->provider = gtk_css_provider_new();
        if ((sp->style_decl = malloc(sizeof('\0'))) == NULL)
                OOM_ABORT;
        *(sp->style_decl) = '\0';
        context = gtk_widget_get_style_context(GTK_WIDGET(obj));
        gtk_style_context_add_provider(context,
                                       GTK_STYLE_PROVIDER(sp->provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_css_provider_load_from_data(sp->provider, sp->style_decl, -1, NULL);
        if (widget_style_providers == NULL) {
                widget_style_providers = sp;
                insque(widget_style_providers, NULL);
        } else {
                for (last_sp = widget_style_providers;
                     last_sp->next != NULL;
                     last_sp = last_sp->next);
                insque(sp, last_sp);
        }
}

static void
prepare_widgets(GtkBuilder *builder)
{
        GSList *objects = NULL;

        objects = gtk_builder_get_objects(builder);
        g_slist_foreach(objects, (GFunc)connect_widget_signals, builder);
        g_slist_foreach(objects, (GFunc)add_widget_style_provider, NULL);
        g_slist_free(objects);
}

int
main(int argc, char *argv[])
{
        char opt;
        char *in_fifo = NULL, *out_fifo = NULL, *ui_file = NULL;
        char *xid_s = NULL, xid_t[BUFLEN];
        Window xid;
        GtkWidget *plug, *body;
        GtkBuilder *builder;
        pthread_t receiver;
        GError *error = NULL;
        GObject *main_window = NULL;

        /* Disable runtime GLIB deprecation warnings: */
        setenv("G_ENABLE_DIAGNOSTIC", "0", 0);
        in = NULL;
        out = NULL;
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
        pthread_create(&receiver, NULL, digest_msg, (void*)builder);
        main_window = gtk_builder_get_object(builder, MAIN_WIN);
        if (!GTK_IS_WINDOW(main_window))
                bye(EXIT_FAILURE, stderr,
                    "no toplevel window named \'" MAIN_WIN "\'\n");
        prepare_widgets(builder);
        if (xid_s == NULL)      /* standalone */
                gtk_widget_show(GTK_WIDGET(main_window));
        else {                  /* We're being XEmbedded */
                xid = strtoul(xid_s, NULL, 10);
                snprintf(xid_t, BUFLEN, "%lu", xid);
                if (!eql(xid_s, xid_t))
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
        exit(EXIT_SUCCESS);
}
