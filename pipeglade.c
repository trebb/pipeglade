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
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.0.0"
#define BUFLEN 256
#define WHITESPACE " \t\n"

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
                fprintf(out, "%s:%s ", gtk_buildable_get_name(obj), section);
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
                        fprintf(stderr, "Send error; retrying\n");
                        clearerr(out);
                        nanosleep(&(struct timespec){0, nsec}, NULL);
                        putc('\n', out);
                } else
                        break;
        }
}

/*
 * Callback that hides the window the originator is in
 */
void
cb_hide_toplevel(GtkBuildable *obj, gpointer user_data)
{
        GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(obj));

        if (gtk_widget_is_toplevel(toplevel))
                gtk_widget_hide(toplevel);
}

/*
 * Callback that sends a message describing the user selection of a
 * dialog
 */
void
cb_send_dialog_selection(GtkBuildable *obj, gpointer user_data)
{
        GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(obj));
 
        if (gtk_widget_is_toplevel(toplevel))
        {
                if (GTK_IS_FILE_CHOOSER(toplevel)) {
                        send_msg(GTK_BUILDABLE(toplevel),
                                 "file",
                                 gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(toplevel)), NULL);
                        send_msg(GTK_BUILDABLE(toplevel),
                                 "folder",
                                 gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(toplevel)), NULL);
                } /* responses of other dialogs go here */
        }
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

struct selection_data {
        GtkBuildable *buildable;
        char *section;
};

/*
 * send_tree_row_msg serves as an argument for
 * gtk_tree_selection_selected_foreach()
 */
void
send_tree_row_msg(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, struct selection_data *data)
{
        char *path_s, *section;
        int col;
        GtkBuildable *obj;

        obj = (data->buildable);
        section = data->section;
        path_s = gtk_tree_path_to_string(path);
        for (col = 0; col < gtk_tree_model_get_n_columns(model); col++) {
                GValue value = G_VALUE_INIT;
                GType col_type;
                char str[BUFLEN];

                gtk_tree_model_get_value(model, iter, col, &value);
                col_type = gtk_tree_model_get_column_type(model, col);
                switch (col_type) {
                case G_TYPE_INT:
                        snprintf(str, BUFLEN, " %d %d", col, g_value_get_int(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_LONG:
                        snprintf(str, BUFLEN, " %d %ld", col, g_value_get_long(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_INT64:
                        snprintf(str, BUFLEN, " %d %" PRId64, col, g_value_get_int64(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_UINT:
                        snprintf(str, BUFLEN, " %d %u", col, g_value_get_uint(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_ULONG:
                        snprintf(str, BUFLEN, " %d %lu", col, g_value_get_ulong(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_UINT64:
                        snprintf(str, BUFLEN, " %d %" PRIu64, col, g_value_get_uint64(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_BOOLEAN:
                        snprintf(str, BUFLEN, " %d %d", col, g_value_get_boolean(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_FLOAT:
                        snprintf(str, BUFLEN, " %d %f", col, g_value_get_float(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_DOUBLE:
                        snprintf(str, BUFLEN, " %d %f", col, g_value_get_double(&value));
                        send_msg(obj, section, path_s, str, NULL);
                        break;
                case G_TYPE_STRING:
                        snprintf(str, BUFLEN, " %d ", col);
                        send_msg(obj, section, path_s, str, g_value_get_string(&value), NULL);
                        break;
                default:
                        fprintf(stderr, "Column %d not implemented: %s\n", col, G_VALUE_TYPE_NAME(&value));
                        break;
                }
                g_value_unset(&value);
        }
        g_free(path_s);
}

/*
 * cb_0(), cb_1(), ... call this function to do their work: Send
 * message(s) whose nature depends on the arguments passed
 */
static void
do_callback(GtkBuildable *obj, gpointer user_data, const char *section)
{
        char str[BUFLEN];
        const char *item_name;
        GdkRGBA color;
        GtkTreeView *tree_view;
        unsigned int year = 0, month = 0, day = 0;

        if (GTK_IS_ENTRY(obj)) {
                send_msg(obj, section, gtk_entry_get_text(GTK_ENTRY(obj)), NULL);
        } else if (GTK_IS_MENU_ITEM(obj)) {
                item_name = gtk_buildable_get_name(obj);
                strncpy(str, item_name, BUFLEN - 1);
                strncpy(str + strlen(item_name), "_dialog", BUFLEN - 1 - strlen(item_name));
                if (gtk_builder_get_object(user_data, str) != NULL)
                        gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(user_data, str)));
                else
                        send_msg(obj, section, gtk_menu_item_get_label(GTK_MENU_ITEM(obj)), NULL);
        } else if (GTK_IS_COMBO_BOX_TEXT(obj))
                send_msg(obj, section, gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(obj)), NULL);
        else if (GTK_IS_RANGE(obj)) {
                snprintf(str, BUFLEN, "%f", gtk_range_get_value(GTK_RANGE(obj)));
                send_msg(obj, section, str, NULL);
        } else if (GTK_IS_TOGGLE_BUTTON(obj))
                send_msg(obj, section, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(obj))?"on":"off", NULL);
        else if (GTK_IS_COLOR_BUTTON(obj)) {
                gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(obj), &color);
                send_msg(obj, section, gdk_rgba_to_string(&color), NULL);
        } else if (GTK_IS_FONT_BUTTON(obj))
                send_msg(obj, section, gtk_font_button_get_font_name(GTK_FONT_BUTTON(obj)), NULL);
        else if (GTK_IS_FILE_CHOOSER_BUTTON(obj))
                send_msg(obj, section, gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(obj)), NULL);
        else if (GTK_IS_BUTTON(obj))
                send_msg(obj, section, "clicked", NULL);
        else if (GTK_IS_CALENDAR(obj)) {
                gtk_calendar_get_date(GTK_CALENDAR(obj), &year, &month, &day);
                snprintf(str, BUFLEN, "%04u-%02u-%02u", year, ++month, day);
                send_msg(obj, section, str, NULL);
        } else if (GTK_IS_TREE_VIEW_COLUMN(obj))
                send_msg(obj, section, "clicked", NULL);
        else if (GTK_IS_TREE_SELECTION(obj)) {
                tree_view = gtk_tree_selection_get_tree_view(GTK_TREE_SELECTION(obj));
                send_msg(GTK_BUILDABLE(tree_view), section, "clicked", NULL);
                gtk_tree_selection_selected_foreach(
                        GTK_TREE_SELECTION(obj),
                        (GtkTreeSelectionForeachFunc)send_tree_row_msg,
                        &(struct selection_data){GTK_BUILDABLE(tree_view),
                                        section});
        } else if (GTK_IS_WINDOW(obj))
                gtk_widget_hide_on_delete(GTK_WIDGET(obj));
        else
                fprintf(stderr, "Ignoring callback %s from %s\n", section, gtk_buildable_get_name(obj));
}

/*
 * Callback functions cb_0(), cb_1(), cb_2(), and cb_3() for use in
 * simple widgets.  They're all identical except for sending different
 * section strings.
 */
#define MAKE_CB_N(n)                                            \
        void cb_##n(GtkBuildable *obj, gpointer user_data)      \
        {do_callback(obj, user_data, #n);}                      \

MAKE_CB_N(0)
MAKE_CB_N(1)
MAKE_CB_N(2)
MAKE_CB_N(3)

#undef MAKE_CB_N

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
                        if ((*buf = realloc(*buf, *bufsize = *bufsize * 2)) == NULL) {
                                fprintf(stderr, "Out of memory (%s in %s)\n", __func__, __FILE__);
                                abort();
                        }
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
        fprintf(stderr, "Ignoring %s%scommand \"%s\"\n", name, pad, msg);
}

/*
 * Parse command pointed to by ud, and act on ui accordingly.  Sets
 * ud->digested = true if done.  Runs once per command inside
 * gtk_main_loop()
 */
static gboolean
update_ui(struct ui_data *ud)
{
        char name[ud->msg_size], action[ud->msg_size];
        char *data;
        int data_start;
        GObject *obj = NULL;
        GType type = G_TYPE_INVALID;

        data_start = strlen(ud->msg);
        name[0] = action[0] = '\0';
        sscanf(ud->msg,
               " %[0-9a-zA-Z_]:%[0-9a-zA-Z_]%*[ \t] %n",
               name, action, &data_start);
        if (eql(action, "main_quit")) {
                gtk_main_quit();
                ud->msg_digested = true;
                return G_SOURCE_REMOVE;
        }
        obj = (gtk_builder_get_object(ud->builder, name));
        if (obj == NULL) {
                ign_cmd(type, ud->msg);
                ud->msg_digested = true;
                return G_SOURCE_REMOVE;
        }
        data = ud->msg + data_start;
        if (eql(action, "set_sensitive")) {
                gtk_widget_set_sensitive(GTK_WIDGET(obj), strtol(data, NULL, 10));
                ud->msg_digested = true;
                return G_SOURCE_REMOVE;
        } else if (eql(action, "set_visible")) {
                gtk_widget_set_visible(GTK_WIDGET(obj), strtol(data, NULL, 10));
                ud->msg_digested = true;
                return G_SOURCE_REMOVE;
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
                GtkTextBuffer *textbuf;
                GtkTextIter iter, a, b;

                textbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(obj));
                if (eql(action, "set_text"))
                        gtk_text_buffer_set_text(textbuf, data, -1);
                else if (eql(action, "delete")) {
                        gtk_text_buffer_get_bounds(textbuf, &a, &b);
                        gtk_text_buffer_delete(textbuf, &a, &b);
                } else if (eql(action, "insert_at_cursor"))
                        gtk_text_buffer_insert_at_cursor(textbuf, data, -1);
                else if (eql(action, "place_cursor")) {
                        if (eql(data, "end"))
                                gtk_text_buffer_get_end_iter(textbuf, &iter);
                        else    /* numeric offset */
                                gtk_text_buffer_get_iter_at_offset(textbuf, &iter, strtol(data, NULL, 10));
                        gtk_text_buffer_place_cursor(textbuf, &iter);
                } else if (eql(action, "place_cursor_at_line")) {
                        gtk_text_buffer_get_iter_at_line(textbuf, &iter, strtol(data, NULL, 10));
                        gtk_text_buffer_place_cursor(textbuf, &iter);
                } else if (eql(action, "scroll_to_cursor"))
                        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(obj), gtk_text_buffer_get_insert(textbuf), 0.0, 0, 0.0, 0.0);
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
                        GdkRGBA color;

                        gdk_rgba_parse(&color, data);
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(obj), &color);
                } else
                        ign_cmd(type, ud->msg);
        } else if (type == GTK_TYPE_FONT_BUTTON) {
                if (eql(action, "set_font_name"))
                        gtk_font_button_set_font_name(GTK_FONT_BUTTON(obj), data);
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
                        char *position, *text;

                        position = strtok(data, WHITESPACE);
                        text = strtok(NULL, WHITESPACE);
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
                GtkTreeIter iter0, iter1;
                GtkTreeView *view;
                GtkListStore *store;
                GtkTreeModel *model;
                char *tokens, *arg0_s, *arg1_s, *arg2_s, *endptr;
                int arg0_n = 0, arg1_n = 0;
                bool arg0_n_valid = false, arg1_n_valid = false;
                bool iter0_valid = false, iter1_valid = false;

                view = GTK_TREE_VIEW(obj);
                model = gtk_tree_view_get_model(view);
                store = GTK_LIST_STORE(model);
                if ((tokens = malloc(strlen(data) + 1)) == NULL) {
                        fprintf(stderr, "Out of memory (%s in %s)\n", __func__, __FILE__);
                        abort();
                }
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
                         errno == 0 && endptr != arg1_s)  &&
                        gtk_tree_model_get_iter_from_string(model, &iter1, arg1_s);
                if (eql(action, "set") && iter0_valid && arg1_n_valid &&
                    arg1_n < gtk_tree_model_get_n_columns(model)) {
                        GType col_type;
                        long long int n;
                        double d;

                        col_type = gtk_tree_model_get_column_type(model, arg1_n);
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
                                fprintf(stderr, "Column %d: %s not implemented\n", arg1_n, g_type_name(col_type));
                                break;
                        }
                } else if (eql(action, "scroll") && arg0_n_valid && arg1_n_valid)
                        gtk_tree_view_scroll_to_cell (view,
                                                      gtk_tree_path_new_from_string(arg0_s),
                                                      gtk_tree_view_get_column(view, arg1_n),
                                                      0, 0.0, 0.0);
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
        } else if (type == GTK_TYPE_WINDOW) {
                if (eql(action, "set_title"))
                        gtk_window_set_title(GTK_WINDOW(obj), data);
                else
                        ign_cmd(type, ud->msg);
        } else
                ign_cmd(type, ud->msg);
        ud->msg_digested = true;
        return G_SOURCE_REMOVE;
}

/*
 * Read lines from global stream "in" and perform the appropriate
 * actions on the GUI
 */
static void *
digest_msg(void *builder)
{
        for (;;) {
                char first_char;
                struct ui_data ud;

                if ((ud.msg = malloc(ud.msg_size = 32)) == NULL ) {
                        fprintf(stderr, "Out of memory (%s in %s)\n", __func__, __FILE__);
                        abort();
                }
                read_buf(in, &ud.msg, &ud.msg_size);
                sscanf(ud.msg, " %c", &first_char);
                ud.builder = builder;
                if (first_char != '#') {
                        ud.msg_digested = false;
                        gdk_threads_add_timeout(1, (GSourceFunc)update_ui, &ud);
                        while (!ud.msg_digested)
                                nanosleep(&(struct timespec){0, 1e6}, NULL);
                }
                free(ud.msg);
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

        stat(name, &sb);
        if (!S_ISFIFO(sb.st_mode))
                if (name != NULL && mkfifo(name, 0666) != 0) {
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
                        /* fopen blocks if there is no reader so here is one */
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

int
main(int argc, char *argv[])
{
        char opt, *ui_file = NULL;
        char *in_fifo = NULL, *out_fifo = NULL;
        GtkBuilder *builder;
        pthread_t receiver;
        GError *error = NULL;
        GObject *window = NULL;

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
        gtk_builder_connect_signals(builder, builder);          
        pthread_create(&receiver, NULL, digest_msg, (void*) builder);
        window = (gtk_builder_get_object(builder, "window"));
        if (!GTK_IS_WIDGET(window)) {
                fprintf(stderr, "No toplevel window named \'window\'\n");
                exit(EXIT_FAILURE);
        }
        gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "window")));
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
