#! /usr/bin/env bash

# Another possible shebang line:
#! /usr/bin/env mksh

# Pipeglade tests; they should be invoked in the build directory.
#
# Failure of a test can cause failure of one or more subsequent tests.

export LC_ALL=C
FIN=to-g.fifo
FOUT=from-g.fifo
FERR=err.fifo
rm -f $FIN $FOUT $FERR



# BATCH ONE
#
# Situations where pipeglade should exit immediately.  These tests
# should run automatically
######################################################################

check_call() {
    r=$2
    e=$3
    o=$4
    output=$($1 2>tmperr.txt)
    retval=$?
    error=$(<tmperr.txt)
    rm tmperr.txt
    echo "CALL $1"
    if test "$output" = "" -a "$o" = "" || (echo "$output" | grep -Fqe "$o"); then
        echo " OK   STDOUT $output"
    else
        echo " FAIL STDOUT $output"
        echo "    EXPECTED $o"
    fi
    if test "$error" = "" -a "$e" = "" || test "$retval" -eq "$r" && (echo "$error" | grep -Fqe "$e"); then
        echo " OK   EXIT/STDERR $retval $error"
    else
        echo " FAIL EXIT/STDERR $retval $error"
        echo "         EXPECTED $r $e"
    fi
}

check_call "./pipeglade -u nonexistent.ui" 1 "nonexistent.ui" ""
check_call "./pipeglade -u bad_window.ui" 1 "no toplevel window named 'window'" ""
check_call "./pipeglade -u html-template/404.html" 1 "'html'" ""
check_call "./pipeglade -u README" 1 "Document must begin with an element" ""
touch bad_fifo
check_call "./pipeglade -i bad_fifo" 1 "making fifo" ""
check_call "./pipeglade -o bad_fifo" 1 "making fifo" ""
rm bad_fifo
check_call "./pipeglade -h" 0 "usage: ./pipeglade [-h] [-i in-fifo] [-o out-fifo] [-u glade-builder-file.ui] [-G] [-V]" ""
check_call "./pipeglade -G" 0 "" "GTK+ v"
check_call "./pipeglade -V" 0 "" "."
check_call "./pipeglade -X" 0 "option" ""
check_call "./pipeglade -u" 0 "argument" ""
check_call "./pipeglade -i" 0 "argument" ""
check_call "./pipeglade -o" 0 "argument" ""
check_call "./pipeglade yyy" 0 "illegal parameter 'yyy'" ""
mkfifo $FIN
echo -e "statusbar1:pop\n _:main_quit" > $FIN &
check_call "./pipeglade -i $FIN" 0 "" ""

sleep .5
if test -e $FIN; then
    echo "FAILED to delete $FIN"
fi

if test -e $FOUT; then
    echo "FAILED to delete $FOUT"
fi




#exit
# BATCH TWO
#
# Error handling tests---bogus actions leading to appropriate error
# messages.  These tests should run automatically.
######################################################################

mkfifo $FERR

check_error() {
    echo "SEND $1"
    echo -e "$1" >$FIN
    read r <$FERR
    if test "$2" = "$r"; then
        echo " OK $r"
    else
        echo " FAIL     $r"
        echo " EXPECTED $2"
    fi
}

read r 2< $FERR &
./pipeglade -i $FIN 2> $FERR &

# wait for $FIN to appear
while test ! \( -e $FIN \); do :; done

# Non-existent name
check_error "" "ignoring command \"\""
check_error "nnn" "ignoring command \"nnn\""
check_error "nnn:set_text FFFF" "ignoring command \"nnn:set_text FFFF\""
# Widget that shouldn't fire callbacks
check_error "label1:force_cb" "ignoring callback forced from label1"
# GtkLabel
check_error "label1:nnn" "ignoring GtkLabel command \"label1:nnn\""
# GtkImage
check_error "image1:nnn" "ignoring GtkImage command \"image1:nnn\""
# GtkTextView
check_error "textview1:nnn" "ignoring GtkTextView command \"textview1:nnn\""
# GtkButton
Check_error "button1:nnn" "ignoring GtkButton command \"button1:nnn\""
# GtkToggleButton
check_error "togglebutton1:nnn" "ignoring GtkToggleButton command \"togglebutton1:nnn\""
# GtkCheckButton
check_error "checkbutton1:nnn" "ignoring GtkCheckButton command \"checkbutton1:nnn\""
# GtkRadioButton
check_error "radiobutton1:nnn" "ignoring GtkRadioButton command \"radiobutton1:nnn\""
# GtkSpinButton
check_error "spinbutton1:nnn" "ignoring GtkSpinButton command \"spinbutton1:nnn\""
# GtkFileChooserButton
check_error "filechooserbutton1:nnn" "ignoring GtkFileChooserButton command \"filechooserbutton1:nnn\""
# GtkFilechooserDialog
check_error "open_dialog:nnn" "ignoring GtkFileChooserDialog command \"open_dialog:nnn\""
# GtkFontButton
check_error "fontbutton1:nnn" "ignoring GtkFontButton command \"fontbutton1:nnn\""
# GtkColorButton
check_error "colorbutton1:nnn" "ignoring GtkColorButton command \"colorbutton1:nnn\""
# GtkScale
check_error "scale1:nnn" "ignoring GtkScale command \"scale1:nnn\""
# GtkProgressBar
check_error "progressbar1:nnn" "ignoring GtkProgressBar command \"progressbar1:nnn\""
# GtkSpinner
check_error "spinner1:nnn" "ignoring GtkSpinner command \"spinner1:nnn\""
# GtkStatusbar
check_error "statusbar1:nnn" "ignoring GtkStatusbar command \"statusbar1:nnn\""
# GtkComboBoxText
check_error "comboboxtext1:nnn" "ignoring GtkComboBoxText command \"comboboxtext1:nnn\""
# GtkEntry
check_error "entry1:nnn" "ignoring GtkEntry command \"entry1:nnn\""
# GtkTreeView insert_row
check_error "treeview1:nnn" "ignoring GtkTreeView command \"treeview1:nnn\""
check_error "treeview1:insert_row 10000" "ignoring GtkTreeView command \"treeview1:insert_row 10000\""
check_error "treeview1:insert_row -1" "ignoring GtkTreeView command \"treeview1:insert_row -1\""
check_error "treeview1:insert_row nnn" "ignoring GtkTreeView command \"treeview1:insert_row nnn\""
check_error "treeview1:insert_row" "ignoring GtkTreeView command \"treeview1:insert_row\""
check_error "treeview1:insert_row " "ignoring GtkTreeView command \"treeview1:insert_row \""
# GtkTreeView remove_row
check_error "treeview1:remove_row 10000" "ignoring GtkTreeView command \"treeview1:remove_row 10000\""
check_error "treeview1:remove_row -1" "ignoring GtkTreeView command \"treeview1:remove_row -1\""
check_error "treeview1:remove_row nnn" "ignoring GtkTreeView command \"treeview1:remove_row nnn\""
check_error "treeview1:remove_row" "ignoring GtkTreeView command \"treeview1:remove_row\""
check_error "treeview1:remove_row " "ignoring GtkTreeView command \"treeview1:remove_row \""
# GtkTreeView move_row
check_error "treeview1:move_row" "ignoring GtkTreeView command \"treeview1:move_row\""
check_error "treeview1:move_row " "ignoring GtkTreeView command \"treeview1:move_row \""
check_error "treeview1:move_row nnn" "ignoring GtkTreeView command \"treeview1:move_row nnn\""
check_error "treeview1:move_row 10000 end" "ignoring GtkTreeView command \"treeview1:move_row 10000 end\""
check_error "treeview1:move_row -1 end" "ignoring GtkTreeView command \"treeview1:move_row -1 end\""
check_error "treeview1:move_row nnn end" "ignoring GtkTreeView command \"treeview1:move_row nnn end\""
check_error "treeview1:move_row 0 10000" "ignoring GtkTreeView command \"treeview1:move_row 0 10000\""
check_error "treeview1:move_row 0 -1" "ignoring GtkTreeView command \"treeview1:move_row 0 -1\""
check_error "treeview1:move_row 0 nnn" "ignoring GtkTreeView command \"treeview1:move_row 0 nnn\""
# GtkTreeView scroll
check_error "treeview1:scroll" "ignoring GtkTreeView command \"treeview1:scroll\""
check_error "treeview1:scroll " "ignoring GtkTreeView command \"treeview1:scroll \""
check_error "treeview1:scroll nnn" "ignoring GtkTreeView command \"treeview1:scroll nnn\""
check_error "treeview1:scroll -1 1" "ignoring GtkTreeView command \"treeview1:scroll -1 1\""
check_error "treeview1:scroll 1 -1" "ignoring GtkTreeView command \"treeview1:scroll 1 -1\""
check_error "treeview1:scroll nnn 1" "ignoring GtkTreeView command \"treeview1:scroll nnn 1\""
check_error "treeview1:scroll 1 nnn" "ignoring GtkTreeView command \"treeview1:scroll 1 nnn\""
# GtkTreeView set
check_error "treeview1:set" "ignoring GtkTreeView command \"treeview1:set\""
check_error "treeview1:set " "ignoring GtkTreeView command \"treeview1:set \""
check_error "treeview1:set nnn" "ignoring GtkTreeView command \"treeview1:set nnn\""
check_error "treeview1:set 0 nnn" "ignoring GtkTreeView command \"treeview1:set 0 nnn\""
check_error "treeview1:set nnn 0" "ignoring GtkTreeView command \"treeview1:set nnn 0\""
check_error "treeview1:set 10000 1 77" "ignoring GtkTreeView command \"treeview1:set 10000 1 77\""
check_error "treeview1:set 1 10000 77" "ignoring GtkTreeView command \"treeview1:set 1 10000 77\""
check_error "treeview1:set 1 11 77" "ignoring GtkTreeView command \"treeview1:set 1 11 77\""
check_error "treeview1:set nnn 1 77" "ignoring GtkTreeView command \"treeview1:set nnn 1 77\""
check_error "treeview1:set 1 nnn 77" "ignoring GtkTreeView command \"treeview1:set 1 nnn 77\""
check_error "treeview1:set -1 1 77" "ignoring GtkTreeView command \"treeview1:set -1 1 77\""
check_error "treeview1:set 1 -1 77" "ignoring GtkTreeView command \"treeview1:set 1 -1 77\""
# GtkTree set "abc" into numeric column
check_error "treeview1:set 1 1 abc" "ignoring GtkTreeView command \"treeview1:set 1 1 abc\""
# GtkCalendar
check_error "calendar1:nnn" "ignoring GtkCalendar command \"calendar1:nnn\""
check_error "calendar1:select_date" "ignoring GtkCalendar command \"calendar1:select_date\""
check_error "calendar1:select_date " "ignoring GtkCalendar command \"calendar1:select_date \""
check_error "calendar1:select_date nnn" "ignoring GtkCalendar command \"calendar1:select_date nnn\""
check_error "calendar1:select_date 2000-12-33" "ignoring GtkCalendar command \"calendar1:select_date 2000-12-33\""
check_error "calendar1:select_date 2000-13-20" "ignoring GtkCalendar command \"calendar1:select_date 2000-13-20\""

echo "_:main_quit" >$FIN

sleep .5
if test -e $FIN; then
    echo "FAILED to delete $FIN"
fi

rm $FERR



#exit
# BATCH THREE
#
# Tests for the principal functionality---valid actions leading to
# correct results.  Manual intervention is required.  Instructions
# will be given on the statusbar of the test GUI.
######################################################################

mkfifo $FOUT

check() {
    N=$1
    echo "SEND $2"
    echo -e "$2" >$FIN
    i=0
    while (( i<$N )); do
        read r <$FOUT
        if test "$r" != ""; then
            if test "$r" = "$3"; then
                echo " OK  ($i)  $r"
            else
                echo " FAIL($i)  $r"
                echo " EXPECTED $3"
            fi
            shift
            (( i+=1 ))
        fi
    done
}

./pipeglade -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 1 "entry1:set_text FFFF" "entry1:0 FFFF"
check 1 "entry1:set_text GGGG" "entry1:0 GGGG"
check 1 "spinbutton1:set_text 33.0" "spinbutton1:0 33.0"
check 2 "radiobutton2:set_active 1" "radiobutton1:0 off" "radiobutton2:0 on"
check 2 "radiobutton1:set_active 1" "radiobutton2:0 off" "radiobutton1:0 on"
check 1 "togglebutton1:set_active 1" "togglebutton1:0 on"
check 1 "calendar1:select_date 1752-03-29" "calendar1:0 1752-03-29"

check 12 "statusbar1:push Click the 66% line\n treeview1:set 2 0 1\n treeview1:set 2 1 -30000\n treeview1:set 2 2 66\n treeview1:set 2 3 -2000000000\n treeview1:set 2 4 4000000000\n treeview1:set 2 5 -2000000000\n treeview1:set 2 6 4000000000\n treeview1:set 2 7 3.141\n treeview1:set 2 8 3.141\n treeview1:set 2 9 TEXT" "treeview1:1 clicked" "treeview1:1 2 0 1" "treeview1:1 2 1 -30000" "treeview1:1 2 2 66" "treeview1:1 2 3 -2000000000" "treeview1:1 2 4 4000000000" "treeview1:1 2 5 -2000000000" "treeview1:1 2 6 4000000000" "treeview1:1 2 7 3.141000" "treeview1:1 2 8 3.141000" "treeview1:1 2 9 TEXT" "treeview1:1 2 10 zzz"
check 12 "statusbar1:push Click the 66% line again (insert_row)\n treeview1:insert_row 0\n treeview1:insert_row 2" "treeview1:1 clicked" "treeview1:1 4 0 1" "treeview1:1 4 1 -30000" "treeview1:1 4 2 66" "treeview1:1 4 3 -2000000000" "treeview1:1 4 4 4000000000" "treeview1:1 4 5 -2000000000" "treeview1:1 4 6 4000000000" "treeview1:1 4 7 3.141000" "treeview1:1 4 8 3.141000" "treeview1:1 4 9 TEXT" "treeview1:1 4 10 zzz"
check 12 "statusbar1:push Click the 66% line again (move_row)\n treeview1:move_row 4 0" "treeview1:1 clicked" "treeview1:1 0 0 1" "treeview1:1 0 1 -30000" "treeview1:1 0 2 66" "treeview1:1 0 3 -2000000000" "treeview1:1 0 4 4000000000" "treeview1:1 0 5 -2000000000" "treeview1:1 0 6 4000000000" "treeview1:1 0 7 3.141000" "treeview1:1 0 8 3.141000" "treeview1:1 0 9 TEXT" "treeview1:1 0 10 zzz"
check 12 "statusbar1:push Click the 66% line again (move_row)\n treeview1:move_row 0 2" "treeview1:1 clicked" "treeview1:1 1 0 1" "treeview1:1 1 1 -30000" "treeview1:1 1 2 66" "treeview1:1 1 3 -2000000000" "treeview1:1 1 4 4000000000" "treeview1:1 1 5 -2000000000" "treeview1:1 1 6 4000000000" "treeview1:1 1 7 3.141000" "treeview1:1 1 8 3.141000" "treeview1:1 1 9 TEXT" "treeview1:1 1 10 zzz"
check 12 "statusbar1:push Click the 66% line again (insert_row, move_row)\n treeview1:insert_row end\n treeview1:move_row 1 end" "treeview1:1 clicked" "treeview1:1 6 0 1" "treeview1:1 6 1 -30000" "treeview1:1 6 2 66" "treeview1:1 6 3 -2000000000" "treeview1:1 6 4 4000000000" "treeview1:1 6 5 -2000000000" "treeview1:1 6 6 4000000000" "treeview1:1 6 7 3.141000" "treeview1:1 6 8 3.141000" "treeview1:1 6 9 TEXT" "treeview1:1 6 10 zzz"
check 12 "statusbar1:push Click the 66% line again (remove_row)\n treeview1:remove_row 0\n treeview1:remove_row 2" "treeview1:1 clicked" "treeview1:1 4 0 1" "treeview1:1 4 1 -30000" "treeview1:1 4 2 66" "treeview1:1 4 3 -2000000000" "treeview1:1 4 4 4000000000" "treeview1:1 4 5 -2000000000" "treeview1:1 4 6 4000000000" "treeview1:1 4 7 3.141000" "treeview1:1 4 8 3.141000" "treeview1:1 4 9 TEXT" "treeview1:1 4 10 zzz"
check 12 "statusbar1:push Click the 66% line once again (move_row)\n treeview1:move_row 0 end" "treeview1:1 clicked" "treeview1:1 3 0 1" "treeview1:1 3 1 -30000" "treeview1:1 3 2 66" "treeview1:1 3 3 -2000000000" "treeview1:1 3 4 4000000000" "treeview1:1 3 5 -2000000000" "treeview1:1 3 6 4000000000" "treeview1:1 3 7 3.141000" "treeview1:1 3 8 3.141000" "treeview1:1 3 9 TEXT" "treeview1:1 3 10 zzz"
check 24 "treeview1:remove_row 3" "treeview1:1 clicked" "treeview1:1 3 0 0" "treeview1:1 3 1 0" "treeview1:1 3 2 0" "treeview1:1 3 3 0" "treeview1:1 3 4 0" "treeview1:1 3 5 0" "treeview1:1 3 6 0" "treeview1:1 3 7 0.000000" "treeview1:1 3 8 0.000000" "treeview1:1 3 9 abc" "treeview1:1 3 10 xxx" "treeview1:1 clicked" "treeview1:1 3 0 0" "treeview1:1 3 1 0" "treeview1:1 3 2 0" "treeview1:1 3 3 0" "treeview1:1 3 4 0" "treeview1:1 3 5 0" "treeview1:1 3 6 0" "treeview1:1 3 7 0.000000" "treeview1:1 3 8 0.000000" "treeview1:1 3 9 abc" "treeview1:1 3 10 xxx"
check 1 "statusbar1:push Press \"button\" if the 66% line has vanished" "button1:0 clicked"
check 12 "statusbar1:push Click the lowest line visible in the scrolled area (scroll)\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:scroll 24 0" "treeview1:1 clicked" "treeview1:1 24 0 0" "treeview1:1 24 1 0" "treeview1:1 24 2 0" "treeview1:1 24 3 0" "treeview1:1 24 4 0" "treeview1:1 24 5 0" "treeview1:1 24 6 0" "treeview1:1 24 7 0.000000" "treeview1:1 24 8 0.000000" "treeview1:1 24 9 abc" "treeview1:1 24 10 xxx"
check 12 "statusbar1:push Click the highest line visible in the scrolled area (scroll)\n treeview1:scroll 1 0" "treeview1:1 clicked" "treeview1:1 1 0 0" "treeview1:1 1 1 3" "treeview1:1 1 2 0" "treeview1:1 1 3 0" "treeview1:1 1 4 0" "treeview1:1 1 5 0" "treeview1:1 1 6 0" "treeview1:1 1 7 0.000000" "treeview1:1 1 8 0.000000" "treeview1:1 1 9 jkl" "treeview1:1 1 10 ZZZ"

check 1 "statusbar1:push Click the header of column \"col3\"" "treeviewcolumn3:3 clicked"
check 1 "statusbar1:push Press \"send_text\"" "send_text:text some textnetcn"
check 1 "statusbar1:push Highlight \"some\" and press \"send selection\"" "send_selection:text some"
check 1 "statusbar1:push Press \"send_text\" again\n textview1:place_cursor 5\n textview1:insert_at_cursor MORE " "send_text:text some MORE textnetcn"
check 1 "statusbar1:push Press \"send_text\"  again\n textview1:place_cursor_at_line 1\n textview1:insert_at_cursor ETC " "send_text:text some MORE textnETC etcn"
check 1 "statusbar1:push Press \"send_text\" once again\n textview1:delete" "send_text:text"
check 1 "statusbar1:push Highlight the lowest visible text line and press \"send_selection\"\n textview1:place_cursor_at_line 1 \ntextview1:insert_at_cursor A\\\\nB\\\\nC\\\\nD\\\\nE\\\\nF\\\\nG\\\\nH\\\\nI\\\\nJ\\\\nK\\\\nL\\\\nM\\\\nN\\\\nO\\\\nP\\\\nQ\\\\nR\\\\nS\\\\nT\\\\nU\\\\nV\\\\nW\\\\nX\\\\nY\\\\nZ\\\\na\\\\nb\\\\nc\\\\nd\\\\ne\\\\nf\\\\ng\\\\nh\\\\ni\\\\nj\\\\nk\\\\nl\\\\nm\\\\nn\\\\no\\\\np\\\\nq\\\\nr\\\\ns\\\\nt\\\\nu\\\\nv\\\\nw\\\\nx\\\\ny\\\\nz \n textview1:place_cursor_at_line 46 \n textview1:scroll_to_cursor" "send_selection:text u"
check 1 "statusbar1:push Again, highlight the lowest visible text line and press \"send_selection\"\n textview1:place_cursor end\n textview1:scroll_to_cursor" "send_selection:text z"
check 1 "statusbar1:push Highlight the highest visible text line and press \"send_selection\"\n textview1:place_cursor 0 \n textview1:scroll_to_cursor" "send_selection:text A"
check 2 "scale1:set_value 10\n scale1:force_cb" "scale1:0 10.000000" "scale1:forced 10.000000"
check 2 "statusbar1:push Click \"Open\" in the \"File\" menu and there, click \"OK\"\n open_dialog:set_filename q.png" "open_dialog:file $PWD/q.png" "open_dialog:folder $PWD"
check 2 "statusbar1:push Click \"Save As\" in the \"File\" menu and there, click \"OK\"\n save_as_dialog:set_current_name /somewhere/crazy_idea" "save_as_dialog:file /somewhere/crazy_idea" "save_as_dialog:folder"
check 1 "statusbar1:push Press the \"button\" which should now be renamed \"OK\"\n button1:set_label OK" "button1:0 clicked"
check 1 "statusbar1:push Press the \"togglebutton\" which should now be renamed \"on/off\"\n togglebutton1:set_label on/off" "togglebutton1:0 off"
check 1 "statusbar1:push Press the \"checkbutton\" which should now be renamed \"REGISTER\"\n checkbutton1:set_label REGISTER" "checkbutton1:0 on"
check 1 "statusbar1:push Press the \"REGISTER\" checkbutton again\n checkbutton1:set_label REGISTER" "checkbutton1:0 off"
check 2 "statusbar1:push Press the \"radiobutton\" which should now be renamed \"RADIO\"\n radiobutton2:set_label RADIO" "radiobutton1:0 off" "radiobutton2:0 on"
check 1 "statusbar1:push Press \"OK\" if the \"lorem ipsum dolor ...\" text now reads \"LABEL\"\n label1:set_text LABEL" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if the green dot has turned red\n image1:set_from_icon_name gtk-no" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if the red dot has turned into a green \"Q\"\n image1:set_from_file q.png" "button1:0 clicked"
check 1 "statusbar1:push Select \"def\" from the combobox" "comboboxtext1:0 def"
check 1 "statusbar1:push Select \"FIRST\" from the combobox\n comboboxtext1:prepend_text FIRST" "comboboxtext1:0 FIRST"
check 1 "statusbar1:push Select \"LAST\" from the combobox\n comboboxtext1:append_text LAST" "comboboxtext1:0 LAST"
check 1 "statusbar1:push Select \"AVERAGE\" from the combobox\n comboboxtext1:insert_text 3 AVERAGE" "comboboxtext1:0 AVERAGE"
check 1 "statusbar1:push Select the second entry from the combobox\n comboboxtext1:remove 0" "comboboxtext1:0 def"
check 2 "statusbar1:push Click the \"+\" of the spinbutton \n button1:set_label OK" "spinbutton1:0 33.00" "spinbutton1:0 34.00"
check 1 "statusbar1:push Click the \"+\" of the spinbutton again \n button1:set_label OK" "spinbutton1:0 35.00"
check 1 "statusbar1:push Click the \"+\" of the spinbutton once again \n button1:set_label OK" "spinbutton1:0 36.00"
check 1 "statusbar1:push Using the file chooser button (now labelled \"etc\"), select \"File System\" (= \"/\")\n filechooserbutton1:set_filename /etc/" "filechooserbutton1:0 /"
check 2 "statusbar1:push Click the font button (now labelled \"Sans Bold 40\"), and then \"Select\"\n fontbutton1:set_font_name Sans Bold 40" "fontbutton1:0 Sans Bold 40" "fontbutton1:0 Sans Bold 40"check 2 "statusbar1:push Click the color button (now turned yellow), and then \"Select\"\n colorbutton1:set_color yellow" "colorbutton1:0 rgb(255,255,0)" "colorbutton1:0 rgb(255,255,0)"
check 1 "colorbutton1:set_color rgb(0,255,0)\n colorbutton1:force_cb" "colorbutton1:forced rgb(0,255,0)"
check 1 "colorbutton1:set_color #00f\n colorbutton1:force_cb" "colorbutton1:forced rgb(0,0,255)"
check 1 "colorbutton1:set_color #ffff00000000\n colorbutton1:force_cb" "colorbutton1:forced rgb(255,0,0)"
check 1 "colorbutton1:set_color rgba(0,255,0,.5)\n colorbutton1:force_cb" "colorbutton1:forced rgba(0,255,0,0.5)"
check 1 "statusbar1:push Press \"OK\" if both 1752-03-13 and 1752-03-14 are marked on the calendar\n calendar1:mark_day 13\n calendar1:mark_day 14" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if 1752-03-13 and 1752-03-14 are no longer marked on the calendar\n calendar1:clear_marks" "button1:0 clicked"
check 3 "statusbar1:push Double-click on 1752-03-13 in the calendar" "calendar1:0 1752-03-13" "calendar1:0 1752-03-13" "calendar1:3 1752-03-13"
check 1 "statusbar1:push Press \"OK\" if there is a spinning spinner\n spinner1:start" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if the spinner has stopped\n spinner1:stop" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if there is now a \"Disconnect\" button\n button2:set_visible 1\n button2:set_sensitive 0" "button1:0 clicked"
check 1 "statusbar1:push Press \"Disconnect\"\n button2:set_sensitive 1" "button2:1 clicked"
check 1 "statusbar1:push Press \"OK\" if the window title is now \"ALMOST DONE\"\n window:set_title ALMOST DONE" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if the progress bar shows 90%\n progressbar1:set_fraction .9" "button1:0 clicked"
check 1 "statusbar1:push Press \"OK\" if the progress bar text reads \"The End\"\n progressbar1:set_text The End" "button1:0 clicked"
check 1 "statusbar1:push Press \"No\"\n statusbar1:push nonsense 1\n statusbar1:push nonsense 2\n statusbar1:push nonsense 3\n statusbar1:pop\n statusbar1:pop\n statusbar1:pop" "no_button:0 clicked"

echo "_:main_quit" >$FIN

sleep .5
if test -e $FIN; then
    echo "FAILED to delete $FIN"
fi

if test -e $FOUT; then
    echo "FAILED to delete $FOUT"
fi
