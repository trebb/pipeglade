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



#BATCH ONE
#
# Situations where pipeglade should exit immediately.  These tests
# should run automatically

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
check_call "./pipeglade -u bad_window.ui" 1 "No toplevel window named 'window'" ""
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
echo -e "statusbar1:statusbar:pop\n _:_:main_quit" > $FIN &
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
check_error "" "Ignoring command \"\""
check_error "nnn" "Ignoring command \"nnn\""
check_error "nnn:entry:set_text FFFF" "Ignoring command \"nnn:entry:set_text FFFF\""
# GtkLabel
check_error "label1:label:nnn" "Ignoring command \"label1:label:nnn\""
# GtkImage
check_error "image1:image:nnn" "Ignoring command \"image1:image:nnn\""
# GtkTextView
check_error "textview1:text_view:nnn" "Ignoring command \"textview1:text_view:nnn\""
# GtkButton
Check_error "button1:button:nnn" "Ignoring command \"button1:button:nnn\""
# GtkToggleButton
check_error "togglebutton1:toggle_button:nnn" "Ignoring command \"togglebutton1:toggle_button:nnn\""
# GtkCheckButton
check_error "checkbutton1:check_button:nnn" "Ignoring command \"checkbutton1:check_button:nnn\""
# GtkRadioButton
check_error "radiobutton1:radio_button:nnn" "Ignoring command \"radiobutton1:radio_button:nnn\""
# GtkSpinButton
check_error "spinbutton1:spin_button:nnn" "Ignoring command \"spinbutton1:spin_button:nnn\""
# GtkScale
check_error "scale1:scale:nnn" "Ignoring command \"scale1:scale:nnn\""
# GtkProgressBar
check_error "progressbar1:progress_bar:nnn" "Ignoring command \"progressbar1:progress_bar:nnn\""
# GtkSpinner
check_error "spinner1:spinner:nnn" "Ignoring command \"spinner1:spinner:nnn\""
# GtkStatusbar
check_error "scale1:statusbar:nnn" "Ignoring command \"scale1:statusbar:nnn\""
# GtkComboBoxText
check_error "comboboxtext1:combo_box_text:nnn" "Ignoring command \"comboboxtext1:combo_box_text:nnn\""
# GtkEntry
check_error "entry1:entry:nnn" "Ignoring command \"entry1:entry:nnn\""
check_error "entry1:nnn:set_text FFFF" "Ignoring command \"entry1:nnn:set_text FFFF\""
# GtkTreeView insert_row
check_error "treeview1:tree_view:nnn" "Ignoring command \"treeview1:tree_view:nnn\""
check_error "treeview1:tree_view:insert_row 10000" "Ignoring command \"treeview1:tree_view:insert_row 10000\""
check_error "treeview1:tree_view:insert_row -1" "Ignoring command \"treeview1:tree_view:insert_row -1\""
check_error "treeview1:tree_view:insert_row nnn" "Ignoring command \"treeview1:tree_view:insert_row nnn\""
check_error "treeview1:tree_view:insert_row" "Ignoring command \"treeview1:tree_view:insert_row\""
check_error "treeview1:tree_view:insert_row " "Ignoring command \"treeview1:tree_view:insert_row \""
# GtkTreeView remove_row
check_error "treeview1:tree_view:remove_row 10000" "Ignoring command \"treeview1:tree_view:remove_row 10000\""
check_error "treeview1:tree_view:remove_row -1" "Ignoring command \"treeview1:tree_view:remove_row -1\""
check_error "treeview1:tree_view:remove_row nnn" "Ignoring command \"treeview1:tree_view:remove_row nnn\""
check_error "treeview1:tree_view:remove_row" "Ignoring command \"treeview1:tree_view:remove_row\""
check_error "treeview1:tree_view:remove_row " "Ignoring command \"treeview1:tree_view:remove_row \""
# GtkTreeView move_row
check_error "treeview1:tree_view:move_row" "Ignoring command \"treeview1:tree_view:move_row\""
check_error "treeview1:tree_view:move_row " "Ignoring command \"treeview1:tree_view:move_row \""
check_error "treeview1:tree_view:move_row nnn" "Ignoring command \"treeview1:tree_view:move_row nnn\""
check_error "treeview1:tree_view:move_row 10000 end" "Ignoring command \"treeview1:tree_view:move_row 10000 end\""
check_error "treeview1:tree_view:move_row -1 end" "Ignoring command \"treeview1:tree_view:move_row -1 end\""
check_error "treeview1:tree_view:move_row nnn end" "Ignoring command \"treeview1:tree_view:move_row nnn end\""
check_error "treeview1:tree_view:move_row 0 10000" "Ignoring command \"treeview1:tree_view:move_row 0 10000\""
check_error "treeview1:tree_view:move_row 0 -1" "Ignoring command \"treeview1:tree_view:move_row 0 -1\""
check_error "treeview1:tree_view:move_row 0 nnn" "Ignoring command \"treeview1:tree_view:move_row 0 nnn\""
# GtkTreeView scroll
check_error "treeview1:tree_view:scroll" "Ignoring command \"treeview1:tree_view:scroll\""
check_error "treeview1:tree_view:scroll " "Ignoring command \"treeview1:tree_view:scroll \""
check_error "treeview1:tree_view:scroll nnn" "Ignoring command \"treeview1:tree_view:scroll nnn\""
check_error "treeview1:tree_view:scroll -1 1" "Ignoring command \"treeview1:tree_view:scroll -1 1\""
check_error "treeview1:tree_view:scroll 1 -1" "Ignoring command \"treeview1:tree_view:scroll 1 -1\""
check_error "treeview1:tree_view:scroll nnn 1" "Ignoring command \"treeview1:tree_view:scroll nnn 1\""
check_error "treeview1:tree_view:scroll 1 nnn" "Ignoring command \"treeview1:tree_view:scroll 1 nnn\""
# GtkTreeView set
check_error "treeview1:tree_view:set" "Ignoring command \"treeview1:tree_view:set\""
check_error "treeview1:tree_view:set " "Ignoring command \"treeview1:tree_view:set \""
check_error "treeview1:tree_view:set nnn" "Ignoring command \"treeview1:tree_view:set nnn\""
check_error "treeview1:tree_view:set 0 nnn" "Ignoring command \"treeview1:tree_view:set 0 nnn\""
check_error "treeview1:tree_view:set nnn 0" "Ignoring command \"treeview1:tree_view:set nnn 0\""
check_error "treeview1:tree_view:set 10000 1 77" "Ignoring command \"treeview1:tree_view:set 10000 1 77\""
check_error "treeview1:tree_view:set 1 10000 77" "Ignoring command \"treeview1:tree_view:set 1 10000 77\""
check_error "treeview1:tree_view:set 1 11 77" "Ignoring command \"treeview1:tree_view:set 1 11 77\""
check_error "treeview1:tree_view:set nnn 1 77" "Ignoring command \"treeview1:tree_view:set nnn 1 77\""
check_error "treeview1:tree_view:set 1 nnn 77" "Ignoring command \"treeview1:tree_view:set 1 nnn 77\""
check_error "treeview1:tree_view:set -1 1 77" "Ignoring command \"treeview1:tree_view:set -1 1 77\""
check_error "treeview1:tree_view:set 1 -1 77" "Ignoring command \"treeview1:tree_view:set 1 -1 77\""
#   "abc" into numeric column
check_error "treeview1:tree_view:set 1 1 abc" "Ignoring command \"treeview1:tree_view:set 1 1 abc\""
# GtkCalendar
check_error "calendar1:calendar:nnn" "Ignoring command \"calendar1:calendar:nnn\""
check_error "calendar1:calendar:select_date" "Ignoring command \"calendar1:calendar:select_date\""
check_error "calendar1:calendar:select_date " "Ignoring command \"calendar1:calendar:select_date \""
check_error "calendar1:calendar:select_date nnn" "Ignoring command \"calendar1:calendar:select_date nnn\""
check_error "calendar1:calendar:select_date 2000-12-33" "Ignoring command \"calendar1:calendar:select_date 2000-12-33\""
check_error "calendar1:calendar:select_date 2000-13-20" "Ignoring command \"calendar1:calendar:select_date 2000-13-20\""

echo "_:_:main_quit" >$FIN

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

check 1 "entry1:entry:set_text FFFF" "entry1:0 FFFF"
check 1 "entry1:entry:set_text GGGG" "entry1:0 GGGG"
check 1 "spinbutton1:spin_button:set_text 33.0" "spinbutton1:0 33.0"
check 2 "radiobutton2:radio_button:set_active 1" "radiobutton1:0 off" "radiobutton2:0 on"
check 2 "radiobutton1:radio_button:set_active 1" "radiobutton2:0 off" "radiobutton1:0 on"
check 1 "togglebutton1:toggle_button:set_active 1" "togglebutton1:0 on"
check 1 "calendar1:calendar:select_date 1752-03-29" "calendar1:0 1752-03-29"

check 11 "statusbar1:statusbar:push Click the 66% line\n treeview1:tree_view:set 2 0 1\n treeview1:tree_view:set 2 1 -30000\n treeview1:tree_view:set 2 2 66\n treeview1:tree_view:set 2 3 -2000000000\n treeview1:tree_view:set 2 4 4000000000\n treeview1:tree_view:set 2 5 -2000000000\n treeview1:tree_view:set 2 6 4000000000\n treeview1:tree_view:set 2 7 3.141\n treeview1:tree_view:set 2 8 3.141\n treeview1:tree_view:set 2 9 TEXT" "treeview1:0 2 0 1" "treeview1:0 2 1 -30000" "treeview1:0 2 2 66" "treeview1:0 2 3 -2000000000" "treeview1:0 2 4 4000000000" "treeview1:0 2 5 -2000000000" "treeview1:0 2 6 4000000000" "treeview1:0 2 7 3.141000" "treeview1:0 2 8 3.141000" "treeview1:0 2 9 TEXT" "treeview1:0 2 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line again (insert_row)\n treeview1:tree_view:insert_row 0\n treeview1:tree_view:insert_row 2" "treeview1:0 4 0 1" "treeview1:0 4 1 -30000" "treeview1:0 4 2 66" "treeview1:0 4 3 -2000000000" "treeview1:0 4 4 4000000000" "treeview1:0 4 5 -2000000000" "treeview1:0 4 6 4000000000" "treeview1:0 4 7 3.141000" "treeview1:0 4 8 3.141000" "treeview1:0 4 9 TEXT" "treeview1:0 4 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line again (move_row)\n treeview1:tree_view:move_row 4 0" "treeview1:0 0 0 1" "treeview1:0 0 1 -30000" "treeview1:0 0 2 66" "treeview1:0 0 3 -2000000000" "treeview1:0 0 4 4000000000" "treeview1:0 0 5 -2000000000" "treeview1:0 0 6 4000000000" "treeview1:0 0 7 3.141000" "treeview1:0 0 8 3.141000" "treeview1:0 0 9 TEXT" "treeview1:0 0 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line again (move_row)\n treeview1:tree_view:move_row 0 2" "treeview1:0 1 0 1" "treeview1:0 1 1 -30000" "treeview1:0 1 2 66" "treeview1:0 1 3 -2000000000" "treeview1:0 1 4 4000000000" "treeview1:0 1 5 -2000000000" "treeview1:0 1 6 4000000000" "treeview1:0 1 7 3.141000" "treeview1:0 1 8 3.141000" "treeview1:0 1 9 TEXT" "treeview1:0 1 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line again (insert_row, move_row)\n treeview1:tree_view:insert_row end\n treeview1:tree_view:move_row 1 end" "treeview1:0 6 0 1" "treeview1:0 6 1 -30000" "treeview1:0 6 2 66" "treeview1:0 6 3 -2000000000" "treeview1:0 6 4 4000000000" "treeview1:0 6 5 -2000000000" "treeview1:0 6 6 4000000000" "treeview1:0 6 7 3.141000" "treeview1:0 6 8 3.141000" "treeview1:0 6 9 TEXT" "treeview1:0 6 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line again (remove_row)\n treeview1:tree_view:remove_row 0\n treeview1:tree_view:remove_row 2" "treeview1:0 4 0 1" "treeview1:0 4 1 -30000" "treeview1:0 4 2 66" "treeview1:0 4 3 -2000000000" "treeview1:0 4 4 4000000000" "treeview1:0 4 5 -2000000000" "treeview1:0 4 6 4000000000" "treeview1:0 4 7 3.141000" "treeview1:0 4 8 3.141000" "treeview1:0 4 9 TEXT" "treeview1:0 4 10 zzz"
check 11 "statusbar1:statusbar:push Click the 66% line once again (move_row)\n treeview1:tree_view:move_row 0 end" "treeview1:0 3 0 1" "treeview1:0 3 1 -30000" "treeview1:0 3 2 66" "treeview1:0 3 3 -2000000000" "treeview1:0 3 4 4000000000" "treeview1:0 3 5 -2000000000" "treeview1:0 3 6 4000000000" "treeview1:0 3 7 3.141000" "treeview1:0 3 8 3.141000" "treeview1:0 3 9 TEXT" "treeview1:0 3 10 zzz"
check 11 "treeview1:tree_view:remove_row 3" "treeview1:0 3 0 0" "treeview1:0 3 1 0" "treeview1:0 3 2 0" "treeview1:0 3 3 0" "treeview1:0 3 4 0" "treeview1:0 3 5 0" "treeview1:0 3 6 0" "treeview1:0 3 7 0.000000" "treeview1:0 3 8 0.000000" "treeview1:0 3 9 abc" "treeview1:0 3 10 xxx"
check 1 "statusbar1:statusbar:push Press \"button\" if the 66% line has vanished" "button1:0 clicked"
check 11 "statusbar1:statusbar:push Click the lowest line visible in the scrolled area (scroll)\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:insert_row 2\n treeview1:tree_view:scroll 24 0" "treeview1:0 24 0 0" "treeview1:0 24 1 0" "treeview1:0 24 2 0" "treeview1:0 24 3 0" "treeview1:0 24 4 0" "treeview1:0 24 5 0" "treeview1:0 24 6 0" "treeview1:0 24 7 0.000000" "treeview1:0 24 8 0.000000" "treeview1:0 24 9 abc" "treeview1:0 24 10 xxx"
check 11 "statusbar1:statusbar:push Click the highest line visible in the scrolled area (scroll)\n treeview1:tree_view:scroll 1 0" "treeview1:0 1 0 0" "treeview1:0 1 1 3" "treeview1:0 1 2 0" "treeview1:0 1 3 0" "treeview1:0 1 4 0" "treeview1:0 1 5 0" "treeview1:0 1 6 0" "treeview1:0 1 7 0.000000" "treeview1:0 1 8 0.000000" "treeview1:0 1 9 jkl" "treeview1:0 1 10 ZZZ"

check 1 "statusbar1:statusbar:push Click the header of column \"col3\"" "treeviewcolumn3:3 clicked"

check 1 "statusbar1:statusbar:push Press \"send_text\"" "send_text:text some textnetcn"
check 1 "statusbar1:statusbar:push Highlight \"some\" and press \"send selection\"" "send_selection:text some"
check 1 "statusbar1:statusbar:push Press \"send_text\" again\n textview1:text_view:place_cursor 5\n textview1:text_view:insert_at_cursor MORE " "send_text:text some MORE textnetcn"
check 1 "statusbar1:statusbar:push Press \"send_text\"  again\n textview1:text_view:place_cursor_at_line 1\n textview1:text_view:insert_at_cursor ETC " "send_text:text some MORE textnETC etcn"
check 1 "statusbar1:statusbar:push Press \"send_text\" once again\n textview1:text_view:delete" "send_text:text"
check 1 "statusbar1:statusbar:push Highlight the lowest visible text line and press \"send_selection\"\n textview1:text_view:place_cursor_at_line 1 \ntextview1:text_view:insert_at_cursor A\\\\nB\\\\nC\\\\nD\\\\nE\\\\nF\\\\nG\\\\nH\\\\nI\\\\nJ\\\\nK\\\\nL\\\\nM\\\\nN\\\\nO\\\\nP\\\\nQ\\\\nR\\\\nS\\\\nT\\\\nU\\\\nV\\\\nW\\\\nX\\\\nY\\\\nZ\\\\na\\\\nb\\\\nc\\\\nd\\\\ne\\\\nf\\\\ng\\\\nh\\\\ni\\\\nj\\\\nk\\\\nl\\\\nm\\\\nn\\\\no\\\\np\\\\nq\\\\nr\\\\ns\\\\nt\\\\nu\\\\nv\\\\nw\\\\nx\\\\ny\\\\nz \n textview1:text_view:place_cursor_at_line 46 \n textview1:text_view:scroll_to_cursor" "send_selection:text u"
check 1 "statusbar1:statusbar:push Again, highlight the lowest visible text line and press \"send_selection\"\n textview1:text_view:place_cursor end\n textview1:text_view:scroll_to_cursor" "send_selection:text z"
check 1 "statusbar1:statusbar:push Highlight the highest visible text line and press \"send_selection\"\n textview1:text_view:place_cursor 0 \n textview1:text_view:scroll_to_cursor" "send_selection:text A"
check 1 "statusbar1:statusbar:push Click once, just beyond the right end of the scale" "scale1:0 100.000000"
check 2 "statusbar1:statusbar:push Click \"Open\" in the \"File\" menu and type \"/\" into the entry line" "open_dialog:file /" "open_dialog:folder"
check 1 "statusbar1:statusbar:push Press the \"button\" which should now be renamed \"OK\"\n button1:button:set_label OK" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press the \"togglebutton\" which should now be renamed \"on/off\"\n togglebutton1:toggle_button:set_label on/off" "togglebutton1:0 off"
check 1 "statusbar1:statusbar:push Press the \"checkbutton\" which should now be renamed \"REGISTER\"\n checkbutton1:check_button:set_label REGISTER" "checkbutton1:0 on"
check 1 "statusbar1:statusbar:push Press the \"REGISTER\" checkbutton again\n checkbutton1:check_button:set_label REGISTER" "checkbutton1:0 off"
check 2 "statusbar1:statusbar:push Press the \"radiobutton\" which should now be renamed \"RADIO\"\n radiobutton2:radio_button:set_label RADIO" "radiobutton1:0 off" "radiobutton2:0 on"
check 1 "statusbar1:statusbar:push Press \"OK\" if the \"lorem ipsum dolor ...\" text now reads \"LABEL\"\n label1:label:set_text LABEL" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the green dot has turned red\n image1:image:set_from_icon_name gtk-no" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the red dot has turned into a green \"Q\"\n image1:image:set_from_file q.png" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Select \"def\" from the combobox" "comboboxtext1:0 def"
check 1 "statusbar1:statusbar:push Select \"FIRST\" from the combobox\n comboboxtext1:combo_box_text:prepend_text FIRST" "comboboxtext1:0 FIRST"
check 1 "statusbar1:statusbar:push Select \"LAST\" from the combobox\n comboboxtext1:combo_box_text:append_text LAST" "comboboxtext1:0 LAST"
check 1 "statusbar1:statusbar:push Select \"AVERAGE\" from the combobox\n comboboxtext1:combo_box_text:insert_text 3 AVERAGE" "comboboxtext1:0 AVERAGE"
check 1 "statusbar1:statusbar:push Select the second entry from the combobox\n comboboxtext1:combo_box_text:remove 0" "comboboxtext1:0 def"
check 2 "statusbar1:statusbar:push Click the \"+\" of the spinbutton \n button1:button:set_label OK" "spinbutton1:0 33.00" "spinbutton1:0 34.00"
check 1 "statusbar1:statusbar:push Click the \"+\" of the spinbutton again \n button1:button:set_label OK" "spinbutton1:0 35.00"
check 1 "statusbar1:statusbar:push Click the \"+\" of the spinbutton once again \n button1:button:set_label OK" "spinbutton1:0 36.00"
check 1 "statusbar1:statusbar:push Select folder \"/\" using the file chooser button" "filechooserbutton1:0 /"
check 1 "statusbar1:statusbar:push Press \"OK\" if both 1752-03-13 and 1752-03-14 are marked on the calendar\n calendar1:calendar:mark_day 13\n calendar1:calendar:mark_day 14" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if 1752-03-13 and 1752-03-14 are no longer marked on the calendar\n calendar1:calendar:clear_marks" "button1:0 clicked"
check 3 "statusbar1:statusbar:push Double-click on 1752-03-13 in the calendar" "calendar1:0 1752-03-13" "calendar1:0 1752-03-13" "calendar1:3 1752-03-13"
check 1 "statusbar1:statusbar:push Press \"OK\" if there is a spinning spinner\n spinner1:spinner:start" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the spinner has stopped\n spinner1:spinner:stop" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the window title is now \"ALMOST DONE\"\n window:window:set_title ALMOST DONE" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the progress bar shows 90%\n progressbar1:progress_bar:set_fraction .9" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"OK\" if the progress bar text reads \"The End\"\n progressbar1:progress_bar:set_text The End" "button1:0 clicked"
check 1 "statusbar1:statusbar:push Press \"No\"\n statusbar1:statusbar:push nonsense 1\n statusbar1:statusbar:push nonsense 2\n statusbar1:statusbar:push nonsense 3\n statusbar1:statusbar:pop\n statusbar1:statusbar:pop\n statusbar1:statusbar:pop" "no_button:0 clicked"

echo "_:_:main_quit" >$FIN

sleep .5
if test -e $FIN; then
    echo "FAILED to delete $FIN"
fi

if test -e $FOUT; then
    echo "FAILED to delete $FOUT"
fi
