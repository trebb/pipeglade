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

#define VERSION "3.0.0"
#define BUFLEN 256
#define WHITESPACE " \t\n"

#define OOM_ABORT                                                       \
        {                                                               \
                fprintf(stderr,                                         \
                        "out of memory: %s (%s:%d)\n",                  \
                        __func__, __FILE__, __LINE__);                  \
                abort();                                                \
        }

static FILE *in;
static FILE *out;
struct ui_data {
        GtkBuilder *builder;
        size_t msg_size;
        char *msg;
        bool msg_digested;
};

static void
usage(char **argv)
{
        fprintf(stderr,
                "usage: %s "
                "[-h] "
                "[-i in-fifo] "
                "[-o out-fifo] "
                "[-u glade-builder-file.ui] "
                "[-G] "
                "[-V]\n",
                argv[0]);
        exit(EXIT_SUCCESS);
}

static void
version(void)
{
        printf(VERSION "\n");
        exit(EXIT_SUCCESS);
}

static void
gtk_versions(void)
{
        printf("GTK+ v%d.%d.%d (running v%d.%d.%d)\n",
               GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
               gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
        exit(EXIT_SUCCESS);
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
 * "<origin>:<section> <data ...>".  The variadic arguments are
 * strings; last argument must be NULL.  We're being patient with
 * receivers which may intermittently close their end of the fifo, and
 * make a couple of retries if an error occurs.
 */
static void
send_msg(GtkBuildable *obj, const char *section, ...)
{
        va_list ap;
        char *data;
        long nsec;

        for (nsec = 1e6; nsec < 1e9; nsec <<= 3) {
                va_start(ap, section);
                fprintf(out, "%s:%s ", widget_name(obj), section);
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

void
cb_show(GtkBuildable *obj, gpointer user_data)
{
        (void)obj;
        gtk_widget_show(user_data);
}

void
cb_hide(GtkBuildable *obj, gpointer user_data)
{

        (void)obj;
        gtk_widget_hide(user_data);
}

void
cb_send_file_chooser_dialog_selection(GtkBuildable *obj, gpointer user_data)
{
        (void)obj;
        send_msg(user_data,
                 "file",
                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(user_data)), NULL);
        send_msg(GTK_BUILDABLE(user_data),
                 "folder",
                 gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(user_data)), NULL);
}

/*
 * Callback that sends in a message the content of the text buffer
 * passed in user_data
 */
void
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
void
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
        /* else if (GTK_IS_COMBO_BOX_TEXT(obj)) */
        /*         send_msg(obj, user_data, gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(obj)), NULL); */
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
        else if (GTK_IS_BUTTON(obj) || GTK_IS_TREE_VIEW_COLUMN(obj))
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
        } else if (GTK_IS_DIALOG(obj))
                gtk_widget_hide_on_delete(GTK_WIDGET(obj));
        else
                fprintf(stderr, "ignoring callback from %s\n", widget_name(obj));
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
 * Error message
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
                        OOM_ABORT
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
gboolean
cb_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
        struct draw_op *op;
        struct drawing *p;

        (void)data;
        for (p = drawings; p != NULL; p = p->next)
                if (p->widget == widget)
                        for (op = p->draw_ops; op != NULL; op = op->next) {
                                draw(cr, op->op, op->op_args);
                        }
        return FALSE;
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
        GdkRGBA color;

        name[0] = action[0] = '\0';
        sscanf(ud->msg,
               " %[0-9a-zA-Z_]:%[0-9a-zA-Z_]%*[ \t] %n",
               name, action, &data_start);
        if (eql(action, "main_quit")) {
                gtk_main_quit();
                goto done;
        }
        obj = (gtk_builder_get_object(ud->builder, name));
        if (obj == NULL) {
                ign_cmd(type, ud->msg);
                goto done;
        }
        if (eql(action, "force")) {
                cb(GTK_BUILDABLE(obj), "forced");
                goto done;
        }
        data = ud->msg + data_start;
        if (eql(action, "set_sensitive")) {
                gtk_widget_set_sensitive(GTK_WIDGET(obj), strtol(data, NULL, 10));
                goto done;
        } else if (eql(action, "set_visible")) {
                gtk_widget_set_visible(GTK_WIDGET(obj), strtol(data, NULL, 10));
                goto done;
        } else if (eql(action, "override_font")) {
                if (data[0]) {
                        PangoFontDescription *font = pango_font_description_from_string(data);

                        gtk_widget_override_font(GTK_WIDGET(obj), font);
                        pango_font_description_free(font);
                } else
                        gtk_widget_override_font(GTK_WIDGET(obj), NULL);
                goto done;
        } else if (eql(action, "override_color")) {
                if (data[0]) {
                        gdk_rgba_parse(&color, data);
                        gtk_widget_override_color(GTK_WIDGET(obj), GTK_STATE_FLAG_NORMAL, &color);
                } else
                        gtk_widget_override_color(GTK_WIDGET(obj), GTK_STATE_FLAG_NORMAL, NULL);
                goto done;
        } else if (eql(action, "override_background_color")) {
                if (data[0]) {
                        gdk_rgba_parse(&color, data);
                        gtk_widget_override_background_color(GTK_WIDGET(obj), GTK_STATE_FLAG_NORMAL, &color);
                } else
                        gtk_widget_override_background_color(GTK_WIDGET(obj), GTK_STATE_FLAG_NORMAL, NULL);
                goto done;
        }
        type = G_TYPE_FROM_INSTANCE(obj);
        if (type == GTK_TYPE_LABEL) {
                if (eql(action, "set_text"))
                        gtk_label_set_text(GTK_LABEL(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_IMAGE) {
                GtkIconSize size;

                gtk_image_get_icon_name(GTK_IMAGE(obj), NULL, &size);
                if (eql(action, "set_from_file"))
                        gtk_image_set_from_file(GTK_IMAGE(obj), data);
                else if (eql(action, "set_from_icon_name"))
                        gtk_image_set_from_icon_name(GTK_IMAGE(obj), data, size);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_TEXT_VIEW) {
                GtkTextBuffer *textbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj));
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
                        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(obj), gtk_text_buffer_get_insert(textbuf), 0., 0, 0., 0.);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_NOTEBOOK) {
                if (eql(action, "set_current_page"))
                        gtk_notebook_set_current_page(GTK_NOTEBOOK(obj), strtol(data, NULL, 10));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_EXPANDER) {
                if (eql(action, "set_expanded"))
                        gtk_expander_set_expanded(GTK_EXPANDER(obj), strtol(data, NULL, 10));
                else if (eql(action, "set_label"))
                        gtk_expander_set_label(GTK_EXPANDER(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_FRAME) {
                if (eql(action, "set_label"))
                        gtk_frame_set_label(GTK_FRAME(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_BUTTON) {
                if (eql(action, "set_label"))
                        gtk_button_set_label(GTK_BUTTON(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_FILE_CHOOSER_DIALOG) {
                if (eql(action, "set_filename"))
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(obj), data);
                else if (eql(action, "set_current_name"))
                        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_FILE_CHOOSER_BUTTON) {
                if (eql(action, "set_filename"))
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_COLOR_BUTTON) {
                if (eql(action, "set_color")) {
                        gdk_rgba_parse(&color, data);
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(obj), &color);
                } else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_FONT_BUTTON) {
                if (eql(action, "set_font_name"))
                        gtk_font_button_set_font_name(GTK_FONT_BUTTON(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_SWITCH) {
                if (eql(action, "set_active"))
                        gtk_switch_set_active(GTK_SWITCH(obj), strtol(data, NULL, 10));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_TOGGLE_BUTTON || type == GTK_TYPE_RADIO_BUTTON || type == GTK_TYPE_CHECK_BUTTON) {
                if (eql(action, "set_label"))
                        gtk_button_set_label(GTK_BUTTON(obj), data);
                else if (eql(action, "set_active"))
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(obj), strtol(data, NULL, 10));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_SPIN_BUTTON || type == GTK_TYPE_ENTRY) {
                if (eql(action, "set_text"))
                        gtk_entry_set_text(GTK_ENTRY(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_SCALE) {
                if (eql(action, "set_value"))
                        gtk_range_set_value(GTK_RANGE(obj), strtod(data, NULL));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_PROGRESS_BAR) {
                if (eql(action, "set_text"))
                        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(obj), data);
                else if (eql(action, "set_fraction"))
                        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(obj), strtod(data, NULL));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_SPINNER) {
                if (eql(action, "start"))
                        gtk_spinner_start(GTK_SPINNER(obj));
                else if (eql(action, "stop"))
                        gtk_spinner_stop(GTK_SPINNER(obj));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_COMBO_BOX_TEXT) {
                if (eql(action, "prepend_text"))
                        gtk_combo_box_text_prepend_text(GTK_COMBO_BOX_TEXT(obj), data);
                else if (eql(action, "append_text"))
                        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(obj), data);
                else if (eql(action, "remove"))
                        gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(obj), strtol(data, NULL, 10));
                else if (eql(action, "insert_text")) {
                        char *position = strtok(data, WHITESPACE);
                        char *text = strtok(NULL, WHITESPACE);

                        gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(obj), strtol(position, NULL, 10), text);
                } else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_STATUSBAR) {
                if (eql(action, "push"))
                        gtk_statusbar_push(GTK_STATUSBAR(obj), 0, data);
                else if (eql(action, "pop"))
                        gtk_statusbar_pop(GTK_STATUSBAR(obj), 0);
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_CALENDAR) {
                int year = 0, month = 0, day = 0;

                if (eql(action, "select_date")) {
                        sscanf(data, "%d-%d-%d", &year, &month, &day);
                        if (month > -1 && month <= 11 && day > 0 && day <= 31) {
                                gtk_calendar_select_month(GTK_CALENDAR(obj), --month, year);
                                gtk_calendar_select_day(GTK_CALENDAR(obj), day);
                        } else
                                ign_cmd(type, ud->msg);
                } else if (eql(action, "mark_day")) {
                        day = strtol(data, NULL, 10);
                        if (day > 0 && day <= 31)
                                gtk_calendar_mark_day(GTK_CALENDAR(obj), day);
                        else
                                ign_cmd(type, ud->msg);
                } else if (eql(action, "clear_marks"))
                        gtk_calendar_clear_marks(GTK_CALENDAR(obj));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_TREE_VIEW) {
                GtkTreeView *view = GTK_TREE_VIEW(obj);
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
                                        ign_cmd(type, ud->msg);
                                break;
                        case G_TYPE_FLOAT:
                        case G_TYPE_DOUBLE:
                                errno = 0;
                                endptr = NULL;
                                d = strtod(arg2_s, &endptr);
                                if (!errno && endptr != arg2_s)
                                        gtk_list_store_set(store, &iter0, arg1_n, d, -1);
                                else
                                        ign_cmd(type, ud->msg);
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
                                ign_cmd(type, ud->msg);
                else if (eql(action, "move_row") && iter0_valid)
                        if (eql(arg1_s, "end"))
                                gtk_list_store_move_before(store, &iter0, NULL);
                        else if (iter1_valid)
                                gtk_list_store_move_before(store, &iter0, &iter1);
                        else
                                ign_cmd(type, ud->msg);
                else if (eql(action, "remove_row") && iter0_valid)
                        gtk_list_store_remove(store, &iter0);
                else
                        ign_cmd(type, ud->msg);
                free(tokens);
        } else if (type == GTK_TYPE_DRAWING_AREA) {
                if (eql(action, "remove")) {
                        if (!rem_draw_op(GTK_WIDGET(obj), data))
                                ign_cmd(type, ud->msg);
                } else if (eql(action, "refresh")) {
                        gint width = gtk_widget_get_allocated_width (GTK_WIDGET(obj));
                        gint height = gtk_widget_get_allocated_height (GTK_WIDGET(obj));

                        gtk_widget_queue_draw_area(GTK_WIDGET(obj), 0, 0, width, height);
                } else if (ins_draw_op(GTK_WIDGET(obj), action, data));
                else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_WINDOW) {
                if (eql(action, "set_title"))
                        gtk_window_set_title(GTK_WINDOW(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else
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
        FILE *stream;

        if (name != NULL && (stat(name, &sb), !S_ISFIFO(sb.st_mode)))
                if (mkfifo(name, 0666) != 0) {
                        perror("making fifo");
                        exit(EXIT_FAILURE);
                }
        switch (mode[0]) {
        case 'r':
                if (name == NULL)
                        stream = stdin;
                else {
                        fd = open(name, O_RDWR | O_NONBLOCK);
                        if (fd < 0) {
                                perror("opening fifo");
                                exit(EXIT_FAILURE);
                        }
                        stream = fdopen(fd, "r");
                }
                break;
        case 'w':
                if (name == NULL)
                        stream = stdout;
                else {
                        /* fopen blocks if there is no reader, so here is one */
                        fd = open(name, O_RDONLY | O_NONBLOCK);
                        if (fd < 0) {
                                perror("opening fifo");
                                exit(EXIT_FAILURE);
                        }
                        stream = fopen(name, "w");
                        /* unblocking done */
                        close(fd);
                }
                break;
        default:
                abort();
                break;
        }
        if (stream == NULL) {
                perror("opening fifo");
                exit(EXIT_FAILURE);
        } else {
                setvbuf(stream, NULL, _IOLBF, 0);
                return stream;
        }
}

static GObject *
obj_sans_suffix(char *suffix, const char *name, gpointer *builder)
{
        int str_l;
        char str[BUFLEN + 1] = {'\0'};

        str_l = suffix - name;
        strncpy(str, name, str_l < BUFLEN ? str_l : BUFLEN);
        return gtk_builder_get_object(GTK_BUILDER(builder), str);
}

static void
connect_signals(gpointer *obj, gpointer *builder)
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
        if (type == GTK_TYPE_BUTTON) {
                /* button associated with a GtkTextView */
                if ((suffix = strstr(name, "_send_text")) != NULL &&
                    GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                else if ((suffix = strstr(name, "_send_selection")) != NULL &&
                         GTK_IS_TEXT_VIEW(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_text_selection),
                                         gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj2)));
                /* button associated with (and part of) a GtkDialog */
                else if ((suffix = strstr(name, "_cancel")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
                        if (eql(widget_name(obj2), "window"))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb_hide), obj2);
                else if ((suffix = strstr(name, "_ok")) != NULL &&
                         GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder))) {
                        if (GTK_IS_FILE_CHOOSER_DIALOG(obj2))
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                        else  /* generic button */
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
                        if (eql(widget_name(obj2), "window"))
                                g_signal_connect(obj, "clicked", G_CALLBACK(gtk_main_quit), NULL);
                        else
                                g_signal_connect(obj, "clicked", G_CALLBACK(cb_hide), obj2);
                } else if ((suffix = strstr(name, "_apply")) != NULL &&
                           GTK_IS_FILE_CHOOSER_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb_send_file_chooser_dialog_selection), obj2);
                else  /* generic button */
                        g_signal_connect(obj, "clicked", G_CALLBACK(cb), "clicked");
        }
        /* else if (type == GTK_TYPE_MENU_ITEM ||type == GTK_TYPE_IMAGE_MENU_ITEM) */
        /*  instead, avoiding a deprecation warning about GtkImageMenuItem: */
        else if (GTK_IS_MENU_ITEM(obj))
                if ((suffix = strstr(name, "_invoke")) != NULL &&
                    GTK_IS_DIALOG(obj2 = obj_sans_suffix(suffix, name, builder)))
                        g_signal_connect(obj, "activate", G_CALLBACK(cb_show), obj2);
                else
                        g_signal_connect(obj, "activate", G_CALLBACK(cb), "active");
        else if (type == GTK_TYPE_DIALOG || type == GTK_TYPE_FILE_CHOOSER_DIALOG)
                if (eql(name, "window"))
                        g_signal_connect(obj, "delete-event", G_CALLBACK(gtk_main_quit), NULL);
                else
                        g_signal_connect(obj, "delete-event", G_CALLBACK(cb), NULL);
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
        else if (type == GTK_TYPE_DRAWING_AREA)
                g_signal_connect(obj, "draw", G_CALLBACK(cb_draw), NULL);
}

int
main(int argc, char *argv[])
{
        char opt;
        char *in_fifo = NULL, *out_fifo = NULL, *ui_file = NULL;
        GtkBuilder *builder;
        pthread_t receiver;
        GError *error = NULL;
        GObject *window = NULL;
        GSList *objects = NULL;

        in = NULL;
        out = NULL;
        while ((opt = getopt(argc, argv, "hi:o:u:GV")) != -1) {
                switch (opt) {
                case 'i': in_fifo = optarg; break;
                case 'o': out_fifo = optarg; break;
                case 'u': ui_file = optarg; break;
                case 'G': gtk_versions(); break;
                case 'V': version(); break;
                case '?':
                case 'h':
                default: usage(argv); break;
                }
        }
        if (argv[optind] != NULL) {
          fprintf(stderr, "illegal parameter '%s'\n", argv[optind]);
          usage(argv);
        }
        if (ui_file == NULL)
                ui_file = "pipeglade.ui";
        gtk_init(0, NULL);
        builder = gtk_builder_new();
        if (gtk_builder_add_from_file(builder, ui_file, &error) == 0) {
                fprintf(stderr, "%s\n", error->message);
                exit(EXIT_FAILURE);
        }
        in = fifo(in_fifo, "r");
        out = fifo(out_fifo, "w");
        pthread_create(&receiver, NULL, digest_msg, (void*)builder);
        window = gtk_builder_get_object(builder, "window");
        if (!GTK_IS_WINDOW(window)) {
                fprintf(stderr, "no toplevel window named \'window\'\n");
                exit(EXIT_FAILURE);
        }
        g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
        objects = gtk_builder_get_objects(builder);
        g_slist_foreach(objects, (GFunc)connect_signals, builder);
        g_slist_free(objects);
        gtk_widget_show(GTK_WIDGET(window));
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
