#! /usr/bin/env bash

# Another possible shebang line:
#! /usr/bin/env mksh

# Pipeglade tests; they should be invoked in the build directory.
#
# Failure of a test can cause failure of one or more subsequent tests.

export LC_ALL=C
export NO_AT_BRIDGE=1
FIN=to-g.fifo
FOUT=from-g.fifo
FERR=err.fifo
BAD_FIFO=bad_fifo
DIR=test_dir
FILE1=saved1.txt
FILE2=saved2.txt
FILE3=saved3.txt
FILE4=saved4.txt
FILE5=saved5.txt
FILE6=saved6.txt
rm -rf $FIN $FOUT $FERR $BAD_FIFO $FILE1 $FILE2 $FILE3 $FILE4 $FILE5 $FILE6 $DIR

# colored messages: bright green
OK=$'\E[32;1mOK\E[0m'
# bright red
FAIL=$'\E[31;1mFAIL\E[0m'
EXPECTED=$'\E[31;1mEXPECTED\E[0m'
# yellow
CALL=$'\E[33mCALL\E[0m'
SEND=$'\E[33mSEND\E[0m'

TESTS=0
FAILS=0
OKS=0

count_fail() {
    (( TESTS+=1 ))
    (( FAILS+=1 ))
}

count_ok() {
    (( TESTS+=1 ))
    (( OKS+=1 ))
}

check_rm() {
    i=0
    while test -e $1 && (( i<50 )); do
        sleep .1
        (( i+=1 ))
    done;
    if test -e $1; then
        count_fail
        echo " $FAIL $1 should be deleted"
    else
        count_ok
        echo " $OK   $1 deleted"
    fi
}

check_cmd() {
    if $1; then
        count_ok
        echo " $OK   $1"
    else
        count_fail
        echo " $FAIL $1"
    fi
}

echo "
# BATCH ONE
#
# Situations where pipeglade should exit immediately.  These tests
# should run automatically
######################################################################
"

check_call() {
    r=$2
    e=$3
    o=$4
    output=$($1 2>tmperr.txt)
    retval=$?
    error=$(<tmperr.txt)
    rm tmperr.txt
    echo "$CALL $1"
    if test "$output" = "" -a "$o" = "" || (echo "$output" | grep -Fqe "$o"); then
        count_ok
        echo " $OK   STDOUT $output"
    else
        count_fail
        echo " $FAIL STDOUT $output"
        echo "    $EXPECTED $o"
    fi
    if test "$error" = "" -a "$e" = "" || test "$retval" -eq "$r" && (echo "$error" | grep -Fqe "$e"); then
        count_ok
        echo " $OK   EXIT/STDERR $retval $error"
    else
        count_fail
        echo " $FAIL EXIT/STDERR $retval $error"
        echo "         $EXPECTED $r $e"
    fi
}

check_call "./pipeglade -u nonexistent.ui" 1 "nonexistent.ui" ""
check_call "./pipeglade -u bad_window.ui" 1 "no toplevel window named 'main'" ""
check_call "./pipeglade -u www-template/404.html" 1 "html" ""
check_call "./pipeglade -u README" 1 "Document must begin with an element" ""
check_call "./pipeglade -e x" 1 "x is not a valid XEmbed socket id" ""
check_call "./pipeglade -ex" 1 "x is not a valid XEmbed socket id" ""
check_call "./pipeglade -e -77" 1 "-77 is not a valid XEmbed socket id" ""
check_call "./pipeglade -e 77x" 1 "77x is not a valid XEmbed socket id" ""
check_call "./pipeglade -e +77" 1 "+77 is not a valid XEmbed socket id" ""
check_call "./pipeglade -e 999999999999999999999999999999" 1 "999999999999999999999999999999 is not a valid XEmbed socket id" ""
check_call "./pipeglade -e 99999999999999999" 1 "unable to embed into XEmbed socket 99999999999999999" ""
touch $BAD_FIFO
check_call "./pipeglade -i $BAD_FIFO" 1 "making fifo" ""
check_call "./pipeglade -o $BAD_FIFO" 1 "making fifo" ""
rm $BAD_FIFO
check_call "./pipeglade -h" 0 "" "usage: pipeglade [-h] [-e xid] [-i in-fifo] [-o out-fifo] [-u glade-file.ui]
                 [-G] [-V] [--display X-server]"
check_call "./pipeglade -G" 0 "" "GTK+ v"
check_call "./pipeglade -V" 0 "" "."
check_call "./pipeglade -X" 1 "option" ""
check_call "./pipeglade -e" 1 "argument" ""
check_call "./pipeglade -u" 1 "argument" ""
check_call "./pipeglade -i" 1 "argument" ""
check_call "./pipeglade -o" 1 "argument" ""
check_call "./pipeglade yyy" 1 "illegal parameter 'yyy'" ""
check_call "./pipeglade --display nnn" 1 "nnn"
mkfifo $FIN
echo -e "statusbar1:pop\n _:main_quit" > $FIN &
check_call "./pipeglade -i $FIN" 0 "" ""
mkfifo $FIN
echo -e "statusbar1:pop_id 111\n _:main_quit" > $FIN &
check_call "./pipeglade -i $FIN" 0 "" ""

check_rm $FIN
check_rm $FOUT



#exit
echo "
# BATCH TWO
#
# Error handling tests---bogus actions leading to appropriate error
# messages.  Most of these tests should run automatically.
######################################################################
"

mkfifo $FERR

check_error() {
    echo "$SEND $1"
    echo -e "$1" >$FIN
    while read r <$FERR; do
        # ignore irrelevant GTK warnings
        if test "$r" != "" && ! grep -q "WARNING"<<< "$r"; then
            break;
        fi
    done
    if test "$2" = "$r"; then
        count_ok
        echo " $OK $r"
    else
        count_fail
        echo " $FAIL     $r"
        echo " $EXPECTED $2"
    fi
}

read r 2< $FERR &
./pipeglade -i $FIN 2> $FERR &

# wait for $FIN to appear
while test ! \( -e $FIN \); do :; done

# Non-existent name
check_error "nnn" "ignoring command \"nnn\""
check_error "nnn:set_text FFFF" "ignoring command \"nnn:set_text FFFF\""
# Widget that shouldn't fire callbacks
check_error "label1:force" "ignoring GtkLabel command \"label1:force\""
# load file
check_error "_:load" "ignoring command \"_:load\""
check_error "_:load  " "ignoring command \"_:load  \""
check_error "_:load nonexistent.txt" "ignoring command \"_:load nonexistent.txt\""
mkdir -p $DIR
cat >$DIR/$FILE1 <<< "blah"
check_error "_:load $DIR/$FILE1" "ignoring command \"blah\""
cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE1"
check_error "_:load $DIR/$FILE1" "ignoring command \"_:load $DIR/$FILE1\""
cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE2"
cat >$DIR/$FILE2 <<< "_:load $DIR/$FILE1"
check_error "_:load $DIR/$FILE1" "ignoring command \"_:load $DIR/$FILE1\""
cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE2"
cat >$DIR/$FILE2 <<< "_:blah"
check_error "_:load $DIR/$FILE1" "ignoring command \"_:blah\""
rm -rf $DIR
# GtkWindow
check_error "main:nnn" "ignoring GtkWindow command \"main:nnn\""
check_error "main:move" "ignoring GtkWindow command \"main:move\""
check_error "main:move " "ignoring GtkWindow command \"main:move \""
check_error "main:move 700" "ignoring GtkWindow command \"main:move 700\""
check_error "main:move 700 nnn" "ignoring GtkWindow command \"main:move 700 nnn\""
# GtkLabel
check_error "label1:nnn" "ignoring GtkLabel command \"label1:nnn\""
# GtkImage
check_error "image1:nnn" "ignoring GtkImage command \"image1:nnn\""
# GtkNotebook
check_error "notebook1:nnn" "ignoring GtkNotebook command \"notebook1:nnn\""
# GtkExpander
check_error "expander1:nnn" "ignoring GtkExpander command \"expander1:nnn\""
# GtkTextView
check_error "textview1:nnn" "ignoring GtkTextView command \"textview1:nnn\""
check_error "textview1:save" "ignoring GtkTextView command \"textview1:save\""
mkdir $DIR; chmod a-w $DIR
check_error "textview1:save $DIR/$FILE1" "ignoring GtkTextView command \"textview1:save $DIR/$FILE1\""
check_error "textview1:save nonexistent/$FILE1" "ignoring GtkTextView command \"textview1:save nonexistent/$FILE1\""
rm -rf $DIR
# GtkButton
check_error "button1:nnn" "ignoring GtkButton command \"button1:nnn\""
# GtkSwitch
check_error "switch1:nnn" "ignoring GtkSwitch command \"switch1:nnn\""
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
# GtkPrintUnixDialog
check_error "printdialog:nnn" "ignoring GtkPrintUnixDialog command \"printdialog:nnn\""
check_error "statusbar1:push Click \"Print\"\n printdialog:print nonexistent.ps" "ignoring GtkPrintUnixDialog command \" printdialog:print nonexistent.ps\""
# GtkScale
check_error "scale1:nnn" "ignoring GtkScale command \"scale1:nnn\""
# GtkProgressBar
check_error "progressbar1:nnn" "ignoring GtkProgressBar command \"progressbar1:nnn\""
# GtkSpinner
check_error "spinner1:nnn" "ignoring GtkSpinner command \"spinner1:nnn\""
# GtkStatusbar
check_error "statusbar1:nnn" "ignoring GtkStatusbar command \"statusbar1:nnn\""
check_error "statusbar1:push_id" "ignoring GtkStatusbar command \"statusbar1:push_id\""
check_error "statusbar1:push_id " "ignoring GtkStatusbar command \"statusbar1:push_id \""
check_error "statusbar1:push_id abc" "ignoring GtkStatusbar command \"statusbar1:push_id abc\""
check_error "statusbar1:pop_id" "ignoring GtkStatusbar command \"statusbar1:pop_id\""
check_error "statusbar1:pop_id " "ignoring GtkStatusbar command \"statusbar1:pop_id \""
check_error "statusbar1:pop_id abc" "ignoring GtkStatusbar command \"statusbar1:pop_id abc\""
check_error "statusbar1:pop_id abc def" "ignoring GtkStatusbar command \"statusbar1:pop_id abc def\""
# GtkComboBoxText
check_error "comboboxtext1:nnn" "ignoring GtkComboBoxText command \"comboboxtext1:nnn\""
check_error "comboboxtext1:force" "ignoring GtkComboBoxText command \"comboboxtext1:force\""

# GtkTreeView #
check_error "treeview1:nnn" "ignoring GtkTreeView command \"treeview1:nnn\""
check_error "treeview2:nnn" "ignoring GtkTreeView command \"treeview2:nnn\""
check_error "treeview1:force" "ignoring GtkTreeView command \"treeview1:force\""
# GtkTreeView save
check_error "treeview1:save" "ignoring GtkTreeView command \"treeview1:save\""
mkdir $DIR; chmod a-w $DIR
check_error "treeview1:save $DIR/$FILE1" "ignoring GtkTreeView command \"treeview1:save $DIR/$FILE1\""
check_error "treeview1:save nonexistent/$FILE1" "ignoring GtkTreeView command \"treeview1:save nonexistent/$FILE1\""
rm -rf $DIR
# GtkTreeView insert_row
check_error "treeview1:insert_row 10000" "ignoring GtkTreeView command \"treeview1:insert_row 10000\""
check_error "treeview1:insert_row -1" "ignoring GtkTreeView command \"treeview1:insert_row -1\""
check_error "treeview1:insert_row nnn" "ignoring GtkTreeView command \"treeview1:insert_row nnn\""
check_error "treeview1:insert_row" "ignoring GtkTreeView command \"treeview1:insert_row\""
check_error "treeview1:insert_row " "ignoring GtkTreeView command \"treeview1:insert_row \""
check_error "treeview1:insert_row -1" "ignoring GtkTreeView command \"treeview1:insert_row -1\""
check_error "treeview1:insert_row 1000" "ignoring GtkTreeView command \"treeview1:insert_row 1000\""
check_error "treeview2:insert_row 0" "ignoring GtkTreeView command \"treeview2:insert_row 0\""
check_error "treeview3:insert_row end" "missing model/ignoring GtkTreeView command \"treeview3:insert_row end\""
check_error "treeview2:insert_row end\n treeview2:insert_row 0 as_child\n treeview2:insert_row 0:0 as_child\n treeview2:expand abc" "ignoring GtkTreeView command \" treeview2:expand abc\""
check_error "treeview2:expand" "ignoring GtkTreeView command \"treeview2:expand\""
check_error "treeview2:expand 0:abc" "ignoring GtkTreeView command \"treeview2:expand 0:abc\""
check_error "treeview2:expand_all abc" "ignoring GtkTreeView command \"treeview2:expand_all abc\""
check_error "treeview2:expand_all 0:abc" "ignoring GtkTreeView command \"treeview2:expand_all 0:abc\""
check_error "treeview2:collapse abc" "ignoring GtkTreeView command \"treeview2:collapse abc\""
check_error "treeview2:collapse 0:abc" "ignoring GtkTreeView command \"treeview2:collapse 0:abc\""
check_error "treeview2:insert_row" "ignoring GtkTreeView command \"treeview2:insert_row\""
check_error "treeview2:insert_row abc" "ignoring GtkTreeView command \"treeview2:insert_row abc\""
check_error "treeview2:insert_row 0:abc" "ignoring GtkTreeView command \"treeview2:insert_row 0:abc\""
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
check_error "treeview2:move_row" "ignoring GtkTreeView command \"treeview2:move_row\""
check_error "treeview2:move_row 0:0 abc" "ignoring GtkTreeView command \"treeview2:move_row 0:0 abc\""
check_error "treeview2:move_row 0:0 0:abc" "ignoring GtkTreeView command \"treeview2:move_row 0:0 0:abc\""
check_error "treeview2:move_row abc end" "ignoring GtkTreeView command \"treeview2:move_row abc end\""
check_error "treeview2:move_row 0:abc end" "ignoring GtkTreeView command \"treeview2:move_row 0:abc end\""
# GtkTreeView remove_row
check_error "treeview1:remove_row 10000" "ignoring GtkTreeView command \"treeview1:remove_row 10000\""
check_error "treeview1:remove_row -1" "ignoring GtkTreeView command \"treeview1:remove_row -1\""
check_error "treeview1:remove_row nnn" "ignoring GtkTreeView command \"treeview1:remove_row nnn\""
check_error "treeview1:remove_row" "ignoring GtkTreeView command \"treeview1:remove_row\""
check_error "treeview1:remove_row " "ignoring GtkTreeView command \"treeview1:remove_row \""
check_error "treeview2:remove_row" "ignoring GtkTreeView command \"treeview2:remove_row\""
check_error "treeview2:remove_row abc" "ignoring GtkTreeView command \"treeview2:remove_row abc\""
check_error "treeview2:remove_row 0:abc" "ignoring GtkTreeView command \"treeview2:remove_row 0:abc\""
# GtkTreeView scroll
check_error "treeview1:scroll" "ignoring GtkTreeView command \"treeview1:scroll\""
check_error "treeview1:scroll " "ignoring GtkTreeView command \"treeview1:scroll \""
check_error "treeview1:scroll nnn" "ignoring GtkTreeView command \"treeview1:scroll nnn\""
check_error "treeview1:scroll -1 1" "ignoring GtkTreeView command \"treeview1:scroll -1 1\""
check_error "treeview1:scroll 1 -1" "ignoring GtkTreeView command \"treeview1:scroll 1 -1\""
check_error "treeview1:scroll nnn 1" "ignoring GtkTreeView command \"treeview1:scroll nnn 1\""
check_error "treeview1:scroll 1 nnn" "ignoring GtkTreeView command \"treeview1:scroll 1 nnn\""
check_error "treeview2:scroll" "ignoring GtkTreeView command \"treeview2:scroll\""
check_error "treeview2:scroll abc" "ignoring GtkTreeView command \"treeview2:scroll abc\""
check_error "treeview2:scroll 0:abc" "ignoring GtkTreeView command \"treeview2:scroll 0:abc\""
check_error "treeview2:scroll abc 0" "ignoring GtkTreeView command \"treeview2:scroll abc 0\""
check_error "treeview2:scroll 0:abc 0" "ignoring GtkTreeView command \"treeview2:scroll 0:abc 0\""
check_error "treeview2:scroll 0:0" "ignoring GtkTreeView command \"treeview2:scroll 0:0\""
check_error "treeview2:scroll 0:0 abc" "ignoring GtkTreeView command \"treeview2:scroll 0:0 abc\""
check_error "treeview2:set_cursor abc" "ignoring GtkTreeView command \"treeview2:set_cursor abc\""
check_error "treeview2:set_cursor 0:abc" "ignoring GtkTreeView command \"treeview2:set_cursor 0:abc\""
check_error "treeview2:clear 0" "ignoring GtkTreeView command \"treeview2:clear 0\""
check_error "treeview2:clear\n treeview2:insert_row 0" "ignoring GtkTreeView command \" treeview2:insert_row 0\""
# GtkTreeView set
check_error "treeview1:set" "ignoring GtkTreeView command \"treeview1:set\""
check_error "treeview1:set " "ignoring GtkTreeView command \"treeview1:set \""
check_error "treeview1:set nnn" "ignoring GtkTreeView command \"treeview1:set nnn\""
check_error "treeview1:set 0 nnn" "ignoring GtkTreeView command \"treeview1:set 0 nnn\""
check_error "treeview1:set nnn 0" "ignoring GtkTreeView command \"treeview1:set nnn 0\""
check_error "treeview1:set 1 10000 77" "ignoring GtkTreeView command \"treeview1:set 1 10000 77\""
check_error "treeview1:set 1 11 77" "ignoring GtkTreeView command \"treeview1:set 1 11 77\""
check_error "treeview1:set nnn 1 77" "ignoring GtkTreeView command \"treeview1:set nnn 1 77\""
check_error "treeview1:set 1 nnn 77" "ignoring GtkTreeView command \"treeview1:set 1 nnn 77\""
check_error "treeview1:set -1 1 77" "ignoring GtkTreeView command \"treeview1:set -1 1 77\""
check_error "treeview1:set 1 -1 77" "ignoring GtkTreeView command \"treeview1:set 1 -1 77\""
# GtkTree set "abc" into numeric column
check_error "treeview1:set 1 1 abc" "ignoring GtkTreeView command \"treeview1:set 1 1 abc\""

# GtkTreeViewColumn
check_error "treeviewcolumn3:nnn" "ignoring GtkTreeViewColumn command \"treeviewcolumn3:nnn\""
check_error "treeviewcolumn3:force" "ignoring GtkTreeViewColumn command \"treeviewcolumn3:force\""
# GtkEntry
check_error "entry1:nnn" "ignoring GtkEntry command \"entry1:nnn\""
# GtkCalendar
check_error "calendar1:nnn" "ignoring GtkCalendar command \"calendar1:nnn\""
check_error "calendar1:select_date" "ignoring GtkCalendar command \"calendar1:select_date\""
check_error "calendar1:select_date " "ignoring GtkCalendar command \"calendar1:select_date \""
check_error "calendar1:select_date nnn" "ignoring GtkCalendar command \"calendar1:select_date nnn\""
check_error "calendar1:select_date 2000-12-33" "ignoring GtkCalendar command \"calendar1:select_date 2000-12-33\""
check_error "calendar1:select_date 2000-13-20" "ignoring GtkCalendar command \"calendar1:select_date 2000-13-20\""
GtkSocket
check_error "socket1:nnn" "ignoring GtkSocket command \"socket1:nnn\""
GtkScrolledWindow
check_error "scrolledwindow3:nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:nnn\""
check_error "scrolledwindow3:hscroll" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll\""
check_error "scrolledwindow3:hscroll " "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll \""
check_error "scrolledwindow3:hscroll nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll nnn\""
check_error "scrolledwindow3:vscroll" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll\""
check_error "scrolledwindow3:vscroll " "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll \""
check_error "scrolledwindow3:vscroll nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll nnn\""
check_error "scrolledwindow3:hscroll_to_range" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range\""
check_error "scrolledwindow3:hscroll_to_range " "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range \""
check_error "scrolledwindow3:hscroll_to_range nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range nnn\""
check_error "scrolledwindow3:hscroll_to_range 10" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range 10\""
check_error "scrolledwindow3:hscroll_to_range 10 nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range 10 nnn\""
check_error "scrolledwindow3:hscroll_to_range nnn 10" "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range nnn 10\""
check_error "scrolledwindow3:vscroll_to_range" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range\""
check_error "scrolledwindow3:vscroll_to_range " "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range \""
check_error "scrolledwindow3:vscroll_to_range nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range nnn\""
check_error "scrolledwindow3:vscroll_to_range 10" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range 10\""
check_error "scrolledwindow3:vscroll_to_range 10 nnn" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range 10 nnn\""
check_error "scrolledwindow3:vscroll_to_range nnn 10" "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range nnn 10\""
# GtkDrawingArea
check_error "drawingarea1:nnn" "ignoring GtkDrawingArea command \"drawingarea1:nnn\""
check_error "drawingarea1:rectangle" "ignoring GtkDrawingArea command \"drawingarea1:rectangle\""
check_error "drawingarea1:rectangle " "ignoring GtkDrawingArea command \"drawingarea1:rectangle \""
check_error "drawingarea1:rectangle nnn" "ignoring GtkDrawingArea command \"drawingarea1:rectangle nnn\""
check_error "drawingarea1:rectangle 1" "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1\""
check_error "drawingarea1:rectangle 1 10" "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10\""
check_error "drawingarea1:rectangle 1 10 10" "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10\""
check_error "drawingarea1:rectangle 1 10 10 20" "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10 20\""
check_error "drawingarea1:rectangle 1 10 10 20 nnn" "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10 20 nnn\""
check_error "drawingarea1:arc" "ignoring GtkDrawingArea command \"drawingarea1:arc\""
check_error "drawingarea1:arc " "ignoring GtkDrawingArea command \"drawingarea1:arc \""
check_error "drawingarea1:arc nnn" "ignoring GtkDrawingArea command \"drawingarea1:arc nnn\""
check_error "drawingarea1:arc 1" "ignoring GtkDrawingArea command \"drawingarea1:arc 1\""
check_error "drawingarea1:arc 1 10" "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10\""
check_error "drawingarea1:arc 1 10 10" "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10\""
check_error "drawingarea1:arc 1 10 10 20" "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20\""
check_error "drawingarea1:arc 1 10 10 20 45" "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20 45\""
check_error "drawingarea1:arc 1 10 10 20 45 nnn" "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20 45 nnn\""
check_error "drawingarea1:arc_negative" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative\""
check_error "drawingarea1:arc_negative " "ignoring GtkDrawingArea command \"drawingarea1:arc_negative \""
check_error "drawingarea1:arc_negative nnn" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative nnn\""
check_error "drawingarea1:arc_negative 1" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1\""
check_error "drawingarea1:arc_negative 1 10" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10\""
check_error "drawingarea1:arc_negative 1 10 10" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10\""
check_error "drawingarea1:arc_negative 1 10 10 20" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20\""
check_error "drawingarea1:arc_negative 1 10 10 20 45" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20 45\""
check_error "drawingarea1:arc_negative 1 10 10 20 45 nnn" "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20 45 nnn\""
check_error "drawingarea1:curve_to" "ignoring GtkDrawingArea command \"drawingarea1:curve_to\""
check_error "drawingarea1:curve_to " "ignoring GtkDrawingArea command \"drawingarea1:curve_to \""
check_error "drawingarea1:curve_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:curve_to nnn\""
check_error "drawingarea1:curve_to 1" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1\""
check_error "drawingarea1:curve_to 1 10" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10\""
check_error "drawingarea1:curve_to 1 10 10" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10\""
check_error "drawingarea1:curve_to 1 10 10 20" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20\""
check_error "drawingarea1:curve_to 1 10 10 20 20" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20\""
check_error "drawingarea1:curve_to 1 10 10 20 20 25" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20 25\""
check_error "drawingarea1:curve_to 1 10 10 20 20 25 nnn" "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20 25 nnn\""
check_error "drawingarea1:rel_curve_to" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to\""
check_error "drawingarea1:rel_curve_to " "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to \""
check_error "drawingarea1:rel_curve_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to nnn\""
check_error "drawingarea1:rel_curve_to 1" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1\""
check_error "drawingarea1:rel_curve_to 1 10" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10\""
check_error "drawingarea1:rel_curve_to 1 10 10" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10\""
check_error "drawingarea1:rel_curve_to 1 10 10 20" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20\""
check_error "drawingarea1:rel_curve_to 1 10 10 20 20" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20\""
check_error "drawingarea1:rel_curve_to 1 10 10 20 20 25" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20 25\""
check_error "drawingarea1:rel_curve_to 1 10 10 20 20 25 nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20 25 nnn\""
check_error "drawingarea1:line_to" "ignoring GtkDrawingArea command \"drawingarea1:line_to\""
check_error "drawingarea1:line_to " "ignoring GtkDrawingArea command \"drawingarea1:line_to \""
check_error "drawingarea1:line_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:line_to nnn\""
check_error "drawingarea1:line_to 1" "ignoring GtkDrawingArea command \"drawingarea1:line_to 1\""
check_error "drawingarea1:line_to 1 20" "ignoring GtkDrawingArea command \"drawingarea1:line_to 1 20\""
check_error "drawingarea1:line_to 1 20 nnn" "ignoring GtkDrawingArea command \"drawingarea1:line_to 1 20 nnn\""
check_error "drawingarea1:rel_line_to" "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to\""
check_error "drawingarea1:rel_line_to " "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to \""
check_error "drawingarea1:rel_line_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to nnn\""
check_error "drawingarea1:rel_line_to 1" "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1\""
check_error "drawingarea1:rel_line_to 1 20" "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20\""
check_error "drawingarea1:rel_line_to 1 20 nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20 nnn\""
check_error "drawingarea1:move_to" "ignoring GtkDrawingArea command \"drawingarea1:move_to\""
check_error "drawingarea1:move_to " "ignoring GtkDrawingArea command \"drawingarea1:move_to \""
check_error "drawingarea1:move_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:move_to nnn\""
check_error "drawingarea1:move_to 1" "ignoring GtkDrawingArea command \"drawingarea1:move_to 1\""
check_error "drawingarea1:move_to 1 20" "ignoring GtkDrawingArea command \"drawingarea1:move_to 1 20\""
check_error "drawingarea1:move_to 1 20 nnn" "ignoring GtkDrawingArea command \"drawingarea1:move_to 1 20 nnn\""
check_error "drawingarea1:rel_move_to" "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to\""
check_error "drawingarea1:rel_move_to " "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to \""
check_error "drawingarea1:rel_move_to nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to nnn\""
check_error "drawingarea1:rel_move_to 1" "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1\""
check_error "drawingarea1:rel_move_to 1 20" "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1 20\""
check_error "drawingarea1:rel_move_to 1 20 nnn" "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1 20 nnn\""
check_error "drawingarea1:close_path" "ignoring GtkDrawingArea command \"drawingarea1:close_path\""
check_error "drawingarea1:close_path " "ignoring GtkDrawingArea command \"drawingarea1:close_path \""
check_error "drawingarea1:close_path nnn" "ignoring GtkDrawingArea command \"drawingarea1:close_path nnn\""
check_error "drawingarea1:set_source_rgba" "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba\""
check_error "drawingarea1:set_source_rgba " "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba \""
check_error "drawingarea1:set_source_rgba nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba nnn\""
check_error "drawingarea1:set_dash" "ignoring GtkDrawingArea command \"drawingarea1:set_dash\""
check_error "drawingarea1:set_dash " "ignoring GtkDrawingArea command \"drawingarea1:set_dash \""
check_error "drawingarea1:set_dash nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_dash nnn\""
check_error "drawingarea1:set_line_cap" "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap\""
check_error "drawingarea1:set_line_cap " "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap \""
check_error "drawingarea1:set_line_cap nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap nnn\""
check_error "drawingarea1:set_line_cap 1" "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap 1\""
check_error "drawingarea1:set_line_cap 1 nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap 1 nnn\""
check_error "drawingarea1:set_line_join" "ignoring GtkDrawingArea command \"drawingarea1:set_line_join\""
check_error "drawingarea1:set_line_join " "ignoring GtkDrawingArea command \"drawingarea1:set_line_join \""
check_error "drawingarea1:set_line_join nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_join nnn\""
check_error "drawingarea1:set_line_join 1" "ignoring GtkDrawingArea command \"drawingarea1:set_line_join 1\""
check_error "drawingarea1:set_line_join 1 nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_join 1 nnn\""
check_error "drawingarea1:set_line_width" "ignoring GtkDrawingArea command \"drawingarea1:set_line_width\""
check_error "drawingarea1:set_line_width " "ignoring GtkDrawingArea command \"drawingarea1:set_line_width \""
check_error "drawingarea1:set_line_width nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_width nnn\""
check_error "drawingarea1:set_line_width 1" "ignoring GtkDrawingArea command \"drawingarea1:set_line_width 1\""
check_error "drawingarea1:set_line_width 1 nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_line_width 1 nnn\""
check_error "drawingarea1:fill" "ignoring GtkDrawingArea command \"drawingarea1:fill\""
check_error "drawingarea1:fill " "ignoring GtkDrawingArea command \"drawingarea1:fill \""
check_error "drawingarea1:fill nnn" "ignoring GtkDrawingArea command \"drawingarea1:fill nnn\""
check_error "drawingarea1:fill_preserve" "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve\""
check_error "drawingarea1:fill_preserve " "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve \""
check_error "drawingarea1:fill_preserve nnn" "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve nnn\""
check_error "drawingarea1:stroke" "ignoring GtkDrawingArea command \"drawingarea1:stroke\""
check_error "drawingarea1:stroke " "ignoring GtkDrawingArea command \"drawingarea1:stroke \""
check_error "drawingarea1:stroke nnn" "ignoring GtkDrawingArea command \"drawingarea1:stroke nnn\""
check_error "drawingarea1:stroke_preserve" "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve\""
check_error "drawingarea1:stroke_preserve " "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve \""
check_error "drawingarea1:stroke_preserve nnn" "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve nnn\""
check_error "drawingarea1:remove" "ignoring GtkDrawingArea command \"drawingarea1:remove\""
check_error "drawingarea1:remove " "ignoring GtkDrawingArea command \"drawingarea1:remove \""
check_error "drawingarea1:remove nnn" "ignoring GtkDrawingArea command \"drawingarea1:remove nnn\""
check_error "drawingarea1:set_show_text" "ignoring GtkDrawingArea command \"drawingarea1:set_show_text\""
check_error "drawingarea1:set_show_text " "ignoring GtkDrawingArea command \"drawingarea1:set_show_text \""
check_error "drawingarea1:set_show_text nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_show_text nnn\""
check_error "drawingarea1:set_font_size" "ignoring GtkDrawingArea command \"drawingarea1:set_font_size\""
check_error "drawingarea1:set_font_size " "ignoring GtkDrawingArea command \"drawingarea1:set_font_size \""
check_error "drawingarea1:set_font_size nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_font_size nnn\""
check_error "drawingarea1:set_font_size 1" "ignoring GtkDrawingArea command \"drawingarea1:set_font_size 1\""
check_error "drawingarea1:set_font_size 1 nnn" "ignoring GtkDrawingArea command \"drawingarea1:set_font_size 1 nnn\""

echo "_:main_quit" >$FIN

check_rm $FIN
rm $FERR



#exit
echo "
# BATCH THREE
#
# Tests for the principal functionality---valid actions leading to
# correct results.  Manual intervention is required.  Instructions
# will be given on the statusbar of the test GUI.
######################################################################
"

mkfifo $FOUT

check() {
    # Flush stale pipeglade output
    while read -t .1 <$FOUT; do : ; done
    N=$1
    echo "$SEND $2"
    echo -e "$2" >$FIN
    i=0
    while (( i<$N )); do
        read r <$FOUT
        if test "$r" != ""; then
            if test "$r" = "$3"; then
                count_ok
                echo " $OK  ($i)  $r"
            else
                count_fail
                echo " $FAIL($i)  $r"
                echo " $EXPECTED $3"
            fi
            shift
            (( i+=1 ))
        fi
    done
}


./pipeglade --display ${DISPLAY-:0} -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 0 "# checking --display $DISPLAY\n _:main_quit"

check_rm $FIN
check_rm $FOUT


./pipeglade -u simple_dialog.ui -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 1 "main_apply:force" "main_apply:clicked"
check 0 "main_cancel:force"

check_rm $FIN
check_rm $FOUT


./pipeglade -u simple_open.ui -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 2 "main_apply:force" "main:file" "main:folder"
check 0 "main_cancel:force"

check_rm $FIN
check_rm $FOUT


./pipeglade -u simple_open.ui -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 2 "main_ok:force" "main:file" "main:folder"

check_rm $FIN
check_rm $FOUT


mkfifo -m 777 $FIN
mkfifo -m 777 $FOUT
./pipeglade -i $FIN -o $FOUT &
sleep .5
check_cmd "test $(stat -f %Op $FIN) -eq 10600"
check_cmd "test $(stat -f %Op $FOUT) -eq 10600"
echo -e "_:main_quit" > $FIN
check_rm $FIN
check_rm $FOUT


./pipeglade -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

check 0 "socket1:id"
read XID <$FOUT
XID=${XID/socket1:id }
(sleep .5; ./pipeglade -u simple_dialog.ui -e $XID <<< "main_cancel:force") &
check 2 "" "socket1:plug-added" "socket1:plug-removed"
(sleep .5; ./pipeglade -u simple_dialog.ui -e $XID <<< "main_cancel:force") &
check 2 "" "socket1:plug-added" "socket1:plug-removed"

check 1 "entry1:set_text FFFF" "entry1:text FFFF"
check 1 "entry1:set_text" "entry1:text"
check 1 "entry1:set_text FFFF" "entry1:text FFFF"
check 1 "entry1:set_text " "entry1:text"
check 0 "entry1:set_placeholder_text hint hint" # not much of a test
check 1 "entry1:set_text FFFF" "entry1:text FFFF"
check 1 "entry1:set_text GGGG" "entry1:text GGGG"
check 1 "entry1:force" "entry1:text GGGG"
check 1 "spinbutton1:set_text 33.0" "spinbutton1:text 33.0"
check 2 "radiobutton2:set_active 1" "radiobutton1:0" "radiobutton2:1"
check 2 "radiobutton1:set_active 1" "radiobutton2:0" "radiobutton1:1"
check 1 "switch1:set_active 1" "switch1:1"
check 1 "switch1:set_active 0" "switch1:0"
check 1 "togglebutton1:set_active 1" "togglebutton1:1"
check 1 "calendar1:select_date 1752-03-29" "calendar1:clicked 1752-03-29"
check 0 "progressbar1:set_text This Is A Progressbar."

L=$(i=0
    while (( i<100 )); do
        (( i+=1 ))
        echo -n "Repetitive input that is large enough to have the realloc() machinery kick in.---"
    done)
check 1 "entry1:set_text $L" "entry1:text $L"

check 1 "statusbar1:push Open what should now be named \"EXPANDER\" and click the \"button inside expander\"\n expander1:set_expanded 0\n expander1:set_label EXPANDER" "button6:clicked"
check 0 "expander1:set_expanded 0"

check 12 "treeview2:set_visible 0\n treeview1:set 2 0 1\n treeview1:set 2 1 -30000\n treeview1:set 2 2 66\n treeview1:set 2 3 -2000000000\n treeview1:set 2 4 4000000000\n treeview1:set 2 5 -2000000000\n treeview1:set 2 6 4000000000\n treeview1:set 2 7 3.141\n treeview1:set 2 8 3.141\n treeview1:set 2 9 TEXT\n treeview1:set_cursor 2" "treeview1:clicked" "treeview1:gboolean 2 0 1" "treeview1:gint 2 1 -30000" "treeview1:guint 2 2 66" "treeview1:glong 2 3 -2000000000" "treeview1:glong 2 4 4000000000" "treeview1:glong 2 5 -2000000000" "treeview1:gulong 2 6 4000000000" "treeview1:gfloat 2 7 3.141000" "treeview1:gdouble 2 8 3.141000" "treeview1:gchararray 2 9 TEXT" "treeview1:gchararray 2 10 zzz"
mkdir -p $DIR
check 0 "treeview1:save $DIR/$FILE1"
check 0 "treeview1:save $DIR/$FILE1.bak"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "treeview1:insert_row 0\n treeview1:insert_row 2\n treeview1:set_cursor 4" "treeview1:clicked" "treeview1:gboolean 4 0 1" "treeview1:gint 4 1 -30000" "treeview1:guint 4 2 66" "treeview1:glong 4 3 -2000000000" "treeview1:glong 4 4 4000000000" "treeview1:glong 4 5 -2000000000" "treeview1:gulong 4 6 4000000000" "treeview1:gfloat 4 7 3.141000" "treeview1:gdouble 4 8 3.141000" "treeview1:gchararray 4 9 TEXT" "treeview1:gchararray 4 10 zzz"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "treeview1:move_row 4 0\n treeview1:set_cursor 0" "treeview1:clicked" "treeview1:gboolean 0 0 1" "treeview1:gint 0 1 -30000" "treeview1:guint 0 2 66" "treeview1:glong 0 3 -2000000000" "treeview1:glong 0 4 4000000000" "treeview1:glong 0 5 -2000000000" "treeview1:gulong 0 6 4000000000" "treeview1:gfloat 0 7 3.141000" "treeview1:gdouble 0 8 3.141000" "treeview1:gchararray 0 9 TEXT" "treeview1:gchararray 0 10 zzz"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "treeview1:move_row 0 2\n treeview1:set_cursor 1" "treeview1:clicked" "treeview1:gboolean 1 0 1" "treeview1:gint 1 1 -30000" "treeview1:guint 1 2 66" "treeview1:glong 1 3 -2000000000" "treeview1:glong 1 4 4000000000" "treeview1:glong 1 5 -2000000000" "treeview1:gulong 1 6 4000000000" "treeview1:gfloat 1 7 3.141000" "treeview1:gdouble 1 8 3.141000" "treeview1:gchararray 1 9 TEXT" "treeview1:gchararray 1 10 zzz"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "treeview1:insert_row end\n treeview1:move_row 1 end\n treeview1:set_cursor 6" "treeview1:clicked" "treeview1:gboolean 6 0 1" "treeview1:gint 6 1 -30000" "treeview1:guint 6 2 66" "treeview1:glong 6 3 -2000000000" "treeview1:glong 6 4 4000000000" "treeview1:glong 6 5 -2000000000" "treeview1:gulong 6 6 4000000000" "treeview1:gfloat 6 7 3.141000" "treeview1:gdouble 6 8 3.141000" "treeview1:gchararray 6 9 TEXT" "treeview1:gchararray 6 10 zzz"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "treeview1:remove_row 0\n treeview1:remove_row 2\n treeview1:set_cursor 4" "treeview1:clicked" "treeview1:gboolean 4 0 1" "treeview1:gint 4 1 -30000" "treeview1:guint 4 2 66" "treeview1:glong 4 3 -2000000000" "treeview1:glong 4 4 4000000000" "treeview1:glong 4 5 -2000000000" "treeview1:gulong 4 6 4000000000" "treeview1:gfloat 4 7 3.141000" "treeview1:gdouble 4 8 3.141000" "treeview1:gchararray 4 9 TEXT" "treeview1:gchararray 4 10 zzz"
check 1 "treeview1:set_cursor" "treeview1:clicked"
check 12 "statusbar1:push Click the 66% (move_row)\n treeview1:move_row 0 end\n treeview1:set_cursor 3" "treeview1:clicked" "treeview1:gboolean 3 0 1" "treeview1:gint 3 1 -30000" "treeview1:guint 3 2 66" "treeview1:glong 3 3 -2000000000" "treeview1:glong 3 4 4000000000" "treeview1:glong 3 5 -2000000000" "treeview1:gulong 3 6 4000000000" "treeview1:gfloat 3 7 3.141000" "treeview1:gdouble 3 8 3.141000" "treeview1:gchararray 3 9 TEXT" "treeview1:gchararray 3 10 zzz"
check 24 "treeview1:remove_row 3" "treeview1:clicked" "treeview1:gboolean 3 0 0" "treeview1:gint 3 1 0" "treeview1:guint 3 2 0" "treeview1:glong 3 3 0" "treeview1:glong 3 4 0" "treeview1:glong 3 5 0" "treeview1:gulong 3 6 0" "treeview1:gfloat 3 7 0.000000" "treeview1:gdouble 3 8 0.000000" "treeview1:gchararray 3 9 abc" "treeview1:gchararray 3 10 xxx" "treeview1:clicked" "treeview1:gboolean 3 0 0" "treeview1:gint 3 1 0" "treeview1:guint 3 2 0" "treeview1:glong 3 3 0" "treeview1:glong 3 4 0" "treeview1:glong 3 5 0" "treeview1:gulong 3 6 0" "treeview1:gfloat 3 7 0.000000" "treeview1:gdouble 3 8 0.000000" "treeview1:gchararray 3 9 abc" "treeview1:gchararray 3 10 xxx"
check 1 "statusbar1:push Click column col4 in the lowest line visible in the scrolled area and type 444 <Enter> (scroll)\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:scroll 24 0" "treeview1:gint 24 1 444"
check 12 "statusbar1:push Click column col3 in the highest line visible in the scrolled area (scroll)\n treeview1:scroll 1 0" "treeview1:clicked" "treeview1:gboolean 1 0 1" "treeview1:gint 1 1 3" "treeview1:guint 1 2 0" "treeview1:glong 1 3 0" "treeview1:glong 1 4 0" "treeview1:glong 1 5 0" "treeview1:gulong 1 6 0" "treeview1:gfloat 1 7 0.000000" "treeview1:gdouble 1 8 0.000000" "treeview1:gchararray 1 9 jkl" "treeview1:gchararray 1 10 ZZZ"

check 1 "statusbar1:push Click the header of column \"col3\"" "treeviewcolumn3:clicked"

check 2 "treeview1:clear\n button1:force" "treeview1:clicked" "button1:clicked"

check 12 "treeview2:set_visible 1\n treeview2:insert_row end\n treeview2:insert_row 0 as_child\n treeview2:insert_row 0:0 as_child\n treeview2:insert_row 0:0\n treeview2:set 100:1:0 0 1\n treeview2:set 100:1:0 1 -30000\n treeview2:set 100:1:0 2 33\n treeview2:set 100:1:0 3 -2000000000\n treeview2:set 100:1:0 4 4000000000\n treeview2:set 100:1:0 5 -2000000000\n treeview2:set 100:1:0 6 4000000000\n treeview2:set 100:1:0 7 3.141\n treeview2:set 100:1:0 8 3.141\n treeview2:set 100:1:0 9 TEXT\n treeview2:expand_all\n treeview2:set_cursor 100:1:0" "treeview2:clicked" "treeview2:gboolean 100:1:0 0 1" "treeview2:gint 100:1:0 1 -30000" "treeview2:guint 100:1:0 2 33" "treeview2:glong 100:1:0 3 -2000000000" "treeview2:glong 100:1:0 4 4000000000" "treeview2:glong 100:1:0 5 -2000000000" "treeview2:gulong 100:1:0 6 4000000000" "treeview2:gfloat 100:1:0 7 3.141000" "treeview2:gdouble 100:1:0 8 3.141000" "treeview2:gchararray 100:1:0 9 TEXT" "treeview2:gchararray 100:1:0 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 12 "treeview2:insert_row 0\n treeview2:insert_row 0\n treeview2:set 102:1 3 876543210\n treeview2:set 102 3 448822\n treeview2:collapse\n treeview2:set_cursor 102" "treeview2:clicked" "treeview2:gboolean 102 0 0" "treeview2:gint 102 1 0" "treeview2:guint 102 2 0" "treeview2:glong 102 3 448822" "treeview2:glong 102 4 0" "treeview2:glong 102 5 0" "treeview2:gulong 102 6 0" "treeview2:gfloat 102 7 0.000000" "treeview2:gdouble 102 8 0.000000" "treeview2:gchararray 102 9" "treeview2:gchararray 102 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 0 "treeview2:save $DIR/$FILE2"
check 0 "treeview2:save $DIR/$FILE2.bak"
check 12 "treeview2:insert_row 0\n treeview2:collapse\n treeview2:set_cursor 103" "treeview2:clicked" "treeview2:gboolean 103 0 0" "treeview2:gint 103 1 0" "treeview2:guint 103 2 0" "treeview2:glong 103 3 448822" "treeview2:glong 103 4 0" "treeview2:glong 103 5 0" "treeview2:gulong 103 6 0" "treeview2:gfloat 103 7 0.000000" "treeview2:gdouble 103 8 0.000000" "treeview2:gchararray 103 9" "treeview2:gchararray 103 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 12 "statusbar1:push Click the lowest line visible in the scrolled area (1)\n treeview2:expand_all 103\n treeview2:scroll 103:1:0 0" "treeview2:clicked" "treeview2:gboolean 103:1:0 0 1" "treeview2:gint 103:1:0 1 -30000" "treeview2:guint 103:1:0 2 33" "treeview2:glong 103:1:0 3 -2000000000" "treeview2:glong 103:1:0 4 4000000000" "treeview2:glong 103:1:0 5 -2000000000" "treeview2:gulong 103:1:0 6 4000000000" "treeview2:gfloat 103:1:0 7 3.141000" "treeview2:gdouble 103:1:0 8 3.141000" "treeview2:gchararray 103:1:0 9 TEXT" "treeview2:gchararray 103:1:0 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 12 "statusbar1:push Click the lowest visible line (2)\n treeview2:collapse\n treeview2:expand 103\n treeview2:scroll 103:1 0" "treeview2:clicked" "treeview2:gboolean 103:1 0 0" "treeview2:gint 103:1 1 0" "treeview2:guint 103:1 2 0" "treeview2:glong 103:1 3 876543210" "treeview2:glong 103:1 4 0" "treeview2:glong 103:1 5 0" "treeview2:gulong 103:1 6 0" "treeview2:gfloat 103:1 7 0.000000" "treeview2:gdouble 103:1 8 0.000000" "treeview2:gchararray 103:1 9" "treeview2:gchararray 103:1 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 12 "statusbar1:push Click the lowest visible line (3)\n treeview2:collapse\n treeview2:expand_all\n treeview2:scroll 103:1:0 0" "treeview2:clicked" "treeview2:gboolean 103:1:0 0 1" "treeview2:gint 103:1:0 1 -30000" "treeview2:guint 103:1:0 2 33" "treeview2:glong 103:1:0 3 -2000000000" "treeview2:glong 103:1:0 4 4000000000" "treeview2:glong 103:1:0 5 -2000000000" "treeview2:gulong 103:1:0 6 4000000000" "treeview2:gfloat 103:1:0 7 3.141000" "treeview2:gdouble 103:1:0 8 3.141000" "treeview2:gchararray 103:1:0 9 TEXT" "treeview2:gchararray 103:1:0 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"
check 12 "statusbar1:push Click the lowest visible line (4)\n treeview2:expand_all\n treeview2:collapse 103:1\n treeview2:scroll 103:1 0" "treeview2:clicked" "treeview2:gboolean 103:1 0 0" "treeview2:gint 103:1 1 0" "treeview2:guint 103:1 2 0" "treeview2:glong 103:1 3 876543210" "treeview2:glong 103:1 4 0" "treeview2:glong 103:1 5 0" "treeview2:gulong 103:1 6 0" "treeview2:gfloat 103:1 7 0.000000" "treeview2:gdouble 103:1 8 0.000000" "treeview2:gchararray 103:1 9" "treeview2:gchararray 103:1 10"
check 1 "treeview2:set_cursor" "treeview2:clicked"

check 12 "treeview1:clear\n treeview1:set 1 9 ABC\\\\nDEF\\\\nGHI\n treeview1:set_cursor 1" "treeview1:clicked" "treeview1:gboolean 1 0 0" "treeview1:gint 1 1 0" "treeview1:guint 1 2 0" "treeview1:glong 1 3 0" "treeview1:glong 1 4 0" "treeview1:glong 1 5 0" "treeview1:gulong 1 6 0" "treeview1:gfloat 1 7 0.000000" "treeview1:gdouble 1 8 0.000000" "treeview1:gchararray 1 9 ABCnDEFnGHI" "treeview1:gchararray 1 10"

check 0 "treeview1:clear\n treeview2:clear"
check 0 "_:load $DIR/$FILE1"
rm -f $DIR/$FILE1
check 1 "treeview1:save $DIR/$FILE1\n button1:force" "button1:clicked"
check_cmd "cmp $DIR/$FILE1 $DIR/$FILE1.bak"
check 0 "treeview1:clear\n treeview2:clear"
check 0 "_:load $DIR/$FILE2"
rm -f $DIR/$FILE2
check 1 "treeview2:save $DIR/$FILE2\n button1:force" "button1:clicked"
check_cmd "cmp $DIR/$FILE2 $DIR/$FILE2.bak"
cat >$DIR/$FILE3 <<< "_:load $DIR/$FILE1.bak"
cat >>$DIR/$FILE3 <<< "_:load $DIR/$FILE2.bak"
cat >$DIR/$FILE4 <<< "_:load $DIR/$FILE3"
cat >$DIR/$FILE5 <<< "_:load $DIR/$FILE4"
cat >$DIR/$FILE6 <<< "_:load $DIR/$FILE5"
rm -f $DIR/$FILE1 $DIR/$FILE2
check 0 "treeview1:clear\n treeview2:clear"
check 0 "_:load $DIR/$FILE6"
rm -f $DIR/$FILE1 $DIR/$FILE2
check 1 "treeview1:save $DIR/$FILE1\n treeview2:save $DIR/$FILE2\n button1:force" "button1:clicked"
check_cmd "cmp $DIR/$FILE1 $DIR/$FILE1.bak"
check_cmd "cmp $DIR/$FILE2 $DIR/$FILE2.bak"
 rm -rf $DIR
check 0 "treeview2:clear\n treeview2:set 2 0 1"

check 1 "statusbar1:push Click the header of column \"col23\"" "treeviewcolumn23:clicked"

check 0 "notebook1:set_current_page 2"
check 1 "nonexistent_send_text:force" "nonexistent_send_text:clicked"
check 1 "nonexistent_send_selection:force" "nonexistent_send_selection:clicked"
check 1 "nonexistent_ok:force" "nonexistent_ok:clicked"
check 1 "nonexistent_apply:force" "nonexistent_apply:clicked"
check 1 "nonexistent_cancel:force" "nonexistent_cancel:clicked"
check 0 "notebook1:set_current_page 1"
check 1 "textview1_send_text:force" "textview1_send_text:text some textnetcn"
check 1 "textview1:place_cursor 5\n textview1:insert_at_cursor MORE \n textview1_send_text:force" "textview1_send_text:text some MORE textnetcn"
check 1 "textview1:place_cursor_at_line 1\n textview1:insert_at_cursor ETC \n textview1_send_text:force" "textview1_send_text:text some MORE textnETC etcn"
mkdir -p $DIR
check 1 "textview1:save $DIR/$FILE1\n button1:force" "button1:clicked"
i=0
while (( i<2000 )); do
    (( i+=1 ))
    cat $DIR/$FILE1 >> $DIR/$FILE2
done
i=0
while (( i<2000 )); do
    (( i+=1 ))
    echo "textview2:insert_at_cursor ##### THIS IS LINE $i.\\n" >> $DIR/$FILE3
done
check 0 "_:load $DIR/$FILE2"
check 0 "textview1:save $DIR/$FILE1"
check 0 "textview1:save $DIR/$FILE1"
check 0 "textview1:delete"
check 0 "textview2:delete"
check 0 "_:load $DIR/$FILE3"
check 0 "_:load $DIR/$FILE1"
check 0 "textview2:save $DIR/$FILE3"
check 1 "textview1:save $DIR/$FILE2\n button1:force" "button1:clicked"
check_cmd "cmp $DIR/$FILE1 $DIR/$FILE2"
echo "textview1:insert_at_cursor I'm a text containing backslashes:\\nONE\\\\\nTWO\\\\\\\\\\nTHREE\\\\\\\\\\\\\\nEnd" > $DIR/$FILE1
check 0 "textview1:delete\n _:load $DIR/$FILE1"
check 1 "textview1:save $DIR/$FILE1\n textview1:save $DIR/$FILE2\n textview1:delete\n _:load $DIR/$FILE1\n button1:force" "button1:clicked"
rm $DIR/$FILE1
check 1 "textview1:save $DIR/$FILE1\n button1:force" "button1:clicked"
check_cmd "test 96 = `wc -c $DIR/$FILE1 | awk '{print $1}'`"
check_cmd "cmp $DIR/$FILE1 $DIR/$FILE2"
check 1 "textview1:delete\n textview1_send_text:force" "textview1_send_text:text"
check 1 "statusbar1:push Highlight the lowest visible character and press \"send_selection\"\n textview1:place_cursor_at_line 1 \ntextview1:insert_at_cursor A\\\\nB\\\\nC\\\\nD\\\\nE\\\\nF\\\\nG\\\\nH\\\\nI\\\\nJ\\\\nK\\\\nL\\\\nM\\\\nN\\\\nO\\\\nP\\\\nQ\\\\nR\\\\nS\\\\nT\\\\nU\\\\nV\\\\nW\\\\nX\\\\nY\\\\nZ\\\\na\\\\nb\\\\nc\\\\nd\\\\ne\\\\nf\\\\ng\\\\nh\\\\ni\\\\nj\\\\nk\\\\nl\\\\nm\\\\nn\\\\no\\\\np\\\\nq\\\\nr\\\\ns\\\\nt\\\\nu\\\\nv\\\\nw\\\\nx\\\\ny\\\\nz \n textview1:place_cursor_at_line 46 \n textview1:scroll_to_cursor" "textview1_send_selection:text u"
check 1 "statusbar1:push Again, highlight the lowest visible character and press \"send_selection\"\n textview1:place_cursor end\n textview1:scroll_to_cursor" "textview1_send_selection:text z"
check 1 "statusbar1:push Highlight the highest visible character and press \"send_selection\"\n textview1:place_cursor 0 \n textview1:scroll_to_cursor" "textview1_send_selection:text A"
check 1 "treeview2:set 100:10:5 2 8888888\n treeview2:save $DIR/$FILE1\n textview2:save $DIR/$FILE2\n button1:force" "button1:clicked"
cp $DIR/$FILE1 $DIR/$FILE4
check_cmd "cmp $DIR/$FILE2 $DIR/$FILE3"
check 1 "treeview2:clear\n textview2:delete\n _:load $DIR/$FILE1\n _:load $DIR/$FILE2\n button1:force" "button1:clicked"
rm $DIR/$FILE1 $DIR/$FILE2
check 1 "treeview2:save $DIR/$FILE1\n textview2:save $DIR/$FILE2\n button1:force" "button1:clicked"
check_cmd "cmp $DIR/$FILE1 $DIR/$FILE4"
check_cmd "cmp $DIR/$FILE2 $DIR/$FILE3"
rm -rf $DIR
check 1 "scale1:set_value 10\n scale1:force" "scale1:value 10.000000"
check 5 "open_dialog:set_filename q.png\n file:force\n open_dialog_invoke:force\n open_dialog_apply:force\n open_dialog_ok:force" "file:active _File" "open_dialog:file $PWD/q.png" "open_dialog:folder $PWD" "open_dialog:file $PWD/q.png" "open_dialog:folder $PWD"
check 1 "file:force\n open_dialog_invoke:force\n open_dialog_cancel:force" "file:active _File"
check 3 "save_as_dialog:set_current_name /somewhere/crazy_idea\n file:force\n save_as_dialog_invoke:force\n save_as_dialog_ok:force" "file:active _File" "save_as_dialog:file /somewhere/crazy_idea" "save_as_dialog:folder"
check 1 "nonexistent_invoke:force" "nonexistent_invoke:active nonexistent"
check 1 "statusbar1:push Press the \"button\" which should now be renamed \"OK\"\n button1:set_label OK" "button1:clicked"
check 1 "statusbar1:push Press the \"togglebutton\" which should now be renamed \"on/off\"\n togglebutton1:set_label on/off" "togglebutton1:0"
check 1 "togglebutton1:force" "togglebutton1:1"
check 1 "statusbar1:push Press the \"checkbutton\" which should now be renamed \"REGISTER\"\n checkbutton1:set_label REGISTER" "checkbutton1:1"
check 1 "checkbutton1:force" "checkbutton1:0"
check 2 "statusbar1:push Press the \"radiobutton\" which should now be renamed \"RADIO\"\n radiobutton2:set_label RADIO" "radiobutton1:0" "radiobutton2:1"
check 2 "radiobutton1:force" "radiobutton2:0" "radiobutton1:1"
check 1 "statusbar1:push Click the widget whose label font is now Bold Italic 20\n switch1:style font:Bold Italic 20" "switch1:1"
check 1 "statusbar1:push Click the widget whose label has turned red\n switch1:style color:red" "switch1:0"
check 1 "statusbar1:push Click the widget whose background has turned yellow\n checkbutton1:style background-color:yellow" "checkbutton1:1"
check 1 "statusbar1:push Press \"OK\" if font and colors changed in previous steps are back to normal\n switch1:style\n checkbutton1:style" "button1:clicked"
check 1 "switch1:force" "switch1:1"
check 1 "statusbar1:push Press \"OK\" if the \"lorem ipsum dolor ...\" text inside \"frame1\" now reads \"LABEL\"\n label1:set_text LABEL" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the label of the frame around \"LABEL\" now reads \"LOREM IPSUM\"\n frame1:set_label LOREM IPSUM" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the green dot has turned red\n image1:set_from_icon_name gtk-no" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the red dot has turned into a green \"Q\"\n image1:set_from_file q.png" "button1:clicked"
check 1 "statusbar1:push Select \"FIRST\" from the combobox\n comboboxtext1:prepend_text FIRST" "comboboxtext1_entry:text FIRST"
check 1 "statusbar1:push Select \"LAST\" from the combobox\n comboboxtext1:append_text LAST" "comboboxtext1_entry:text LAST"
check 1 "statusbar1:push Select \"AVERAGE\" from the combobox\n comboboxtext1:insert_text 3 AVERAGE" "comboboxtext1_entry:text AVERAGE"
check 1 "statusbar1:push Select the second entry from the combobox\n comboboxtext1:remove 0" "comboboxtext1_entry:text def"
check 2 "statusbar1:push Click the \"+\" of the spinbutton" "spinbutton1:text 33.00" "spinbutton1:text 34.00"
check 1 "statusbar1:push Click the \"+\" of the spinbutton again" "spinbutton1:text 35.00"
check 1 "statusbar1:push Click the \"+\" of the spinbutton once again" "spinbutton1:text 36.00"
check 1 "spinbutton1:force" "spinbutton1:text 36.00"
check 1 "statusbar1:push Using the file chooser button (now labelled \"etc\"), select \"File System\" (= \"/\")\n filechooserbutton1:set_filename /etc/" "filechooserbutton1:file /"
check 1 "filechooserbutton1:force" "filechooserbutton1:file /"
check 1 "statusbar1:push Click \"Select\"\n fontbutton1:set_font_name Sans Bold 40\n fontbutton1:force" "fontbutton1:font Sans Bold 40"
check 1 "statusbar1:push Click \"Select\" (1)\n colorbutton1:set_color yellow\n colorbutton1:force" "colorbutton1:color rgb(255,255,0)"
check 1 "statusbar1:push Click \"Select\" (2)\n colorbutton1:set_color rgb(0,255,0)\n colorbutton1:force" "colorbutton1:color rgb(0,255,0)"
check 1 "statusbar1:push Click \"Select\" (3)\n colorbutton1:set_color #00f\n colorbutton1:force" "colorbutton1:color rgb(0,0,255)"
check 1 "statusbar1:push Click \"Select\" (4)\n colorbutton1:set_color #ffff00000000\n colorbutton1:force" "colorbutton1:color rgb(255,0,0)"
check 1 "statusbar1:push Click \"Select\" (5)\n colorbutton1:set_color rgba(0,255,0,.5)\n colorbutton1:force" "colorbutton1:color rgba(0,255,0,0.5)"
check 0 "statusbar1:push Click \"Cancel\"\n printdialog:print nonexistent.ps"
check 1 "statusbar1:push Press \"OK\" if both 1752-03-13 and 1752-03-14 are marked on the calendar\n calendar1:mark_day 13\n calendar1:mark_day 14" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if 1752-03-13 and 1752-03-14 are no longer marked on the calendar\n calendar1:clear_marks" "button1:clicked"
check 3 "statusbar1:push Double-click on 1752-03-13 in the calendar" "calendar1:clicked 1752-03-13" "calendar1:clicked 1752-03-13" "calendar1:doubleclicked 1752-03-13"
check 1 "calendar1:force" "calendar1:clicked 1752-03-13"

check 0 "drawingarea1:rectangle 1 0 0 150 150\n drawingarea1:fill 1\n drawingarea1:refresh"
check 0 "drawingarea1:remove 1\n drawingarea1:remove 2\n drawingarea1:remove 3\n drawingarea1:remove 4\n drawingarea1:refresh"
check 0 "drawingarea1:rectangle 1 0 0 150 150\n drawingarea1:fill 1\n drawingarea1:refresh"
check 0 "drawingarea1:arc 1 80 80 60 30 60\n drawingarea1:set_source_rgba 1 red\n drawingarea1:stroke_preserve 1\n drawingarea1:line_to 1 80 80\n drawingarea1:fill 1\n drawingarea1:refresh"
check 0 "drawingarea1:arc_negative 1 80 80 70 30 60\n drawingarea1:set_source_rgba 1 green\n drawingarea1:stroke_preserve 1\n drawingarea1:rel_line_to 1 -50 -50\n drawingarea1:stroke 1\n drawingarea1:refresh"
check 0 "drawingarea1:curve_to 1 30 30 90 120 120 30\n drawingarea1:set_source_rgba 1 blue\n drawingarea1:stroke 1\n drawingarea1:refresh"
check 0 "drawingarea1:move_to 1 160 160\n drawingarea1:rel_curve_to 1 30 30 90 120 120 30\n drawingarea1:set_source_rgba 1 orange\n drawingarea1:stroke_preserve 1\n drawingarea1:refresh"
check 0 "drawingarea1:move_to 1 0 0\n drawingarea1:rel_move_to 1 0 155\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 10\n drawingarea1:stroke 1"
check 0 "drawingarea1:move_to 1 0 160\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 20 5\n drawingarea1:stroke 1"
check 0 "drawingarea1:move_to 1 0 165\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 5 20\n drawingarea1:stroke 1"
check 0 "drawingarea1:move_to 1 0 170\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 3 3 3 3 3 15\n drawingarea1:stroke 1"
check 0 "drawingarea1:refresh\n drawingarea1:set_dash 1"
check 0 "drawingarea1:set_source_rgba 1 brown\n drawingarea1:set_line_width 1 15"
check 1 "statusbar1:push Press \"OK\" if the brown shape is rounded\n drawingarea1:set_line_join 2 round\n drawingarea1:set_line_cap 2 round\n drawingarea1:move_to 1 160 20\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1\n drawingarea1:refresh" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the second brown shape is shorter and bevelled\n drawingarea1:set_line_join 3 bevel\n drawingarea1:set_line_cap 3 butt\n drawingarea1:move_to 1 160 70\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1\n drawingarea1:refresh" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the third brown shape is square\n drawingarea1:set_line_join 3 miter\n drawingarea1:set_line_cap 3 square\n drawingarea1:move_to 1 160 120\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1\n drawingarea1:refresh" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the first brown shape is no longer rounded\n drawingarea1:remove 2\n drawingarea1:refresh" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if all three brown shapes look the same\n drawingarea1:remove 3\n drawingarea1:refresh" "button1:clicked"
check 0 "drawingarea1:move_to 5 50 50\n drawingarea1:line_to 5 200 10\n drawingarea1:line_to 5 150 200\n drawingarea1:close_path 1\n drawingarea1:set_source_rgba 5 rgba(0,255,0,.2)\n drawingarea1:fill_preserve 1\n drawingarea1:refresh"
check 0 "drawingarea1:move_to 5 10 50\n drawingarea1:set_source_rgba 5 cyan\n drawingarea1:set_font_size 5 30\n drawingarea1:show_text 5 Xyz\n drawingarea1:set_font_size 5 10\n drawingarea1:show_text 5 Abc\n drawingarea1:refresh"
check 0 "drawingarea1:remove 1\n drawingarea1:remove 2\n drawingarea1:remove 3\n drawingarea1:remove 4\n drawingarea1:refresh"

check 0 "drawingarea2:rectangle 1 0 0 150 150\n drawingarea2:fill 1\n drawingarea2:refresh"
check 0 "drawingarea2:arc 1 80 80 60 30 60\n drawingarea2:set_source_rgba 1 red\n drawingarea2:stroke_preserve 1\n drawingarea2:line_to 1 80 80\n drawingarea2:fill 1\n drawingarea2:refresh"
check 0 "drawingarea2:arc_negative 1 80 80 70 30 60\n drawingarea2:set_source_rgba 1 green\n drawingarea2:stroke_preserve 1\n drawingarea2:rel_line_to 1 -50 -50\n drawingarea2:stroke 1\n drawingarea2:refresh"
check 0 "drawingarea2:curve_to 1 30 30 90 120 120 30\n drawingarea2:set_source_rgba 1 blue\n drawingarea2:stroke 1\n drawingarea2:refresh"
check 0 "drawingarea2:move_to 1 160 160\n drawingarea2:rel_curve_to 1 30 30 90 120 120 30\n drawingarea2:set_source_rgba 1 orange\n drawingarea2:stroke_preserve 1\n drawingarea2:refresh"
check 0 "drawingarea2:move_to 1 0 0\n drawingarea2:rel_move_to 1 0 155\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 10\n drawingarea2:stroke 1"
check 0 "drawingarea2:move_to 1 0 160\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 20 5\n drawingarea2:stroke 1"
check 0 "drawingarea2:move_to 1 0 165\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 5 20\n drawingarea2:stroke 1"
check 0 "drawingarea2:move_to 1 0 170\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 3 3 3 3 3 15\n drawingarea2:stroke 1"
check 0 "drawingarea2:refresh\n drawingarea2:set_dash 1"
check 0 "drawingarea2:set_source_rgba 1 brown\n drawingarea2:set_line_width 1 15"
check 0 "drawingarea2:set_line_cap 2 round\n drawingarea2:move_to 1 160 20\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1\n drawingarea2:refresh"
check 0 "drawingarea2:set_line_join 3 bevel\n drawingarea2:set_line_cap 3 butt\n drawingarea2:move_to 1 160 70\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1\n drawingarea2:refresh"
check 0 "drawingarea2:set_line_join 3 miter\n drawingarea2:set_line_cap 3 square\n drawingarea2:move_to 1 160 120\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1\n drawingarea2:refresh"
check 0 "drawingarea2:remove 2\n drawingarea2:refresh"
check 0 "drawingarea2:remove 3\n drawingarea2:refresh"

check 1 "statusbar1:push Press the biggest button if there is a spinning spinner\n spinner1:start\n no_button:set_size_request 400 400" "no_button:clicked"
check 1 "statusbar1:push Press \"OK\" if the spinner has stopped\n spinner1:stop" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the \"No\" button is back to normal size\n no_button:set_size_request" "button1:clicked"

check 0 "notebook1:set_current_page 3"
check 1 "statusbar1:push Click into page 4 (vscroll)\n scrolledwindow8:vscroll 4500" "button_sw:clicked"
check 1 "statusbar1:push Click into page 4 (hscroll)\n scrolledwindow8:hscroll 4500" "button_se:clicked"
check 1 "statusbar1:push Click into page 4 (hscroll_to_range, vscroll_to_range)\n scrolledwindow8:hscroll_to_range 1600 2900\n scrolledwindow8:vscroll_to_range 1600 2900" "button_c:clicked"

check 1 "statusbar1:push Press \"OK\" if we are fullscreen now\n main:fullscreen" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if we are back to default size\n main:unfullscreen" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if window is 1000x1000 now\n main:resize 1000 1000" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if we are back to default size again\n main:resize" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if our NE corner is at 400, 200 now\n main:move 400 200" "button1:clicked"

check 1 "statusbar1:push Press \"OK\" if there is now a \"Disconnect\" button\n button2:set_visible 1\n button2:set_sensitive 0" "button1:clicked"
check 1 "statusbar1:push Press \"Disconnect\"\n button2:set_sensitive 1" "button2:clicked"
check 1 "statusbar1:push Press \"OK\" if the window title is now \"ALMOST DONE\"\n main:set_title ALMOST DONE" "button1:clicked"

check 1 "statusbar1:push Press \"BIG BUTTON\" inside the window titled \"PRESS ME\"\n window1:set_title PRESS ME\n window1:set_visible 1" "button3:clicked"
check 0 "window1:set_visible 0"

check 1 "statusbar1:push Press \"OK\" if the progress bar shows 90%\n progressbar1:set_fraction .9\n progressbar1:set_text" "button1:clicked"
check 1 "statusbar1:push Press \"OK\" if the progress bar text reads \"The End\"\n progressbar1:set_text The End" "button1:clicked"
check 1 "statusbar1:push_id 100 Press \"No\"\n statusbar1:push_id 1 nonsense #1\n statusbar1:push_id 2 nonsense #2.1\n statusbar1:push_id 2 nonsense 2.2\n statusbar1:pop\n statusbar1:pop\n statusbar1:pop_id 1\n statusbar1:pop_id 1\n statusbar1:pop_id 2\n statusbar1:pop_id 2" "no_button:clicked"

echo "_:main_quit" >$FIN

check_rm $FIN
check_rm $FOUT



echo "PASSED: $OKS/$TESTS; FAILED: $FAILS/$TESTS"
