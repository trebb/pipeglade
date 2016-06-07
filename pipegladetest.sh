#! /usr/bin/env bash

# Pipeglade tests; they should be invoked in the build directory.
#
# Usage: ./pipegladetset.sh
# Usage: ./pipegladetset.sh interactive
# Usage: ./pipegladetset.sh automatic
#
# Failure of a test can cause failure of one or more subsequent tests.

INTERACTIVE=true
AUTOMATIC=true
if test ${1-X} == interactive; then unset -v AUTOMATIC ; fi
if test ${1-X} == automatic; then unset -v INTERACTIVE ; fi

export LC_ALL=C
export NO_AT_BRIDGE=1
FIN=to-g.fifo
FOUT=from-g.fifo
FERR=err.fifo
LOG=test.log
ERR_FILE=err.txt
BAD_FIFO=bad_fifo
PID_FILE=pid
OUT_FILE=out.txt
DIR=test_dir
EPS_FILE=test.eps
EPSF_FILE=test.epsf
PDF_FILE=test.pdf
PS_FILE=test.ps
SVG_FILE=test.svg
FILE1=saved1.txt
FILE2=saved2.txt
FILE3=saved3.txt
FILE4=saved4.txt
FILE5=saved5.txt
FILE6=saved6.txt
BIG_INPUT=big.txt
BIG_INPUT2=big2.txt
BIG_INPUT_ERR=err.txt
WEIRD_PATHS=$(awk 'BEGIN{ for (i=0x01; i<= 0xff; i++) if (i != 0x2a && i != 0x2f && i != 0x3f && i != 0x5c)  printf "'$DIR'/(%c) ", i }')
BIG_STRING=$(for i in {1..100}; do echo -n "abcdefghijklmnopqrstuvwxyz($i)ABCDEFGHIJKLMNOPQRSTUVWXYZ0{${RANDOM}}123456789"; done)
BIG_NUM=$(for i in {1..100}; do echo -n "$RANDOM"; done)
rm -rf $FIN $FOUT $FERR $LOG $ERR_FILE $BAD_FIFO $PID_FILE $OUT_FILE \
   $EPS_FILE $EPSF_FILE $PDF_FILE $PS_FILE $SVG_FILE \
   $FILE1 $FILE2 $FILE3 $FILE4 $FILE5 $FILE6 $BIG_INPUT $BIG_INPUT2 $BIG_INPUT_ERR $DIR

if stat -f "%0p" 2>/dev/null; then
    STAT_CMD='stat -f "%0p"'
else
    # probably GNU stat
    STAT_CMD='stat -c "%a"'
fi

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
    done
    if test -e $1; then
        count_fail
        echo " $FAIL $1 should be deleted"
        rm -f $1
    else
        count_ok
        echo " $OK   $1 deleted"
    fi
}

check_cmd() {
    i=0
    while ! eval "$1" && (( i<50 )); do
        sleep .1
        (( i+=1 ))
    done
    if eval "$1"; then
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

# check_call command expected_status expected_stderr expected_stdout
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


if test $AUTOMATIC; then

    check_call "./pipeglade -u nonexistent.ui" 1 \
               "nonexistent.ui" ""
    check_call "./pipeglade -u bad_window.ui" 1 \
               "no toplevel window with id 'main'" ""
    check_call "./pipeglade -u www-template/404.html" 1 \
               "html" ""
    check_call "./pipeglade -u README" 1 \
               "Document must begin with an element" ""
    check_call "./pipeglade -e x" 1 \
               "x is not a valid XEmbed socket id" ""
    check_call "./pipeglade -ex" 1 \
               "x is not a valid XEmbed socket id" ""
    check_call "./pipeglade -e -77" 1 \
               "-77 is not a valid XEmbed socket id" ""
    check_call "./pipeglade -e 77x" 1 \
               "77x is not a valid XEmbed socket id" ""
    check_call "./pipeglade -e +77" 1 \
               "+77 is not a valid XEmbed socket id" ""
    check_call "./pipeglade -e 999999999999999999999999999999" 1 \
               "999999999999999999999999999999 is not a valid XEmbed socket id" ""
    check_call "./pipeglade -e 99999999999999999" 1 \
               "unable to embed into XEmbed socket 99999999999999999" ""
    touch $BAD_FIFO
    check_call "./pipeglade -i $BAD_FIFO" 1 \
               "using pre-existing fifo" ""
    check_call "./pipeglade -o $BAD_FIFO" 1 \
               "using pre-existing fifo" ""
    rm $BAD_FIFO
    check_call "./pipeglade -b" 1 \
               "parameter -b requires both -i and -o"
    check_call "./pipeglade -b -i $FIN" 1 \
               "parameter -b requires both -i and -o"
    check_call "./pipeglade -b -i $FOUT" 1 \
               "parameter -b requires both -i and -o"
    rm $FIN $FOUT
    check_call "./pipeglade -h" 0 \
               "" "usage: pipeglade [[-i in-fifo] [-o out-fifo] [-b] [-u glade-file.ui] [-e xid]
                 [-l log-file] [-O err-file] [--display X-server]] | [-h|-G|-V]"
    check_call "./pipeglade -G" 0 \
               "" "GTK+  v"
    check_call "./pipeglade -G" 0 \
               "" "cairo v"
    check_call "./pipeglade -V" 0 \
               "" "."
    check_call "./pipeglade -X" 1 \
               "option" ""
    check_call "./pipeglade -e" 1 \
               "argument" ""
    check_call "./pipeglade -u" 1 \
               "argument" ""
    check_call "./pipeglade -i" 1 \
               "argument" ""
    check_call "./pipeglade -o" 1 \
               "argument" ""
    mkdir -p $DIR
    check_call "./pipeglade -O" 1 \
               "argument" ""
    check_call "./pipeglade -O $DIR" 1 \
               "" "redirecting stderr to"
    check_call "./pipeglade -l $DIR" 1 \
               "opening log file" ""
    check_call "./pipeglade -l" 1 \
               "argument" ""
    # assuming we can't adjust permissions of /dev/null:
    check_call "./pipeglade -O /dev/null" 1 \
               "" "setting permissions of /dev/null:"
    check_call "./pipeglade -l /dev/null" 1 \
               "setting permissions of /dev/null:" ""
    check_call "./pipeglade yyy" 1 \
               "illegal parameter 'yyy'" ""
    check_call "./pipeglade --display nnn" 1 \
               "nnn"
    check_rm $FIN
    check_rm $FOUT

fi


#exit

echo "
# BATCH TWO
#
# Error handling tests---bogus actions leading to appropriate error
# messages.  Most of these tests should run automatically.
######################################################################
"

mkfifo $FERR

# check_error command expected_stderr
check_error() {
    echo "$SEND ${1:0:300}"
    echo -e "$1" >$FIN
    while read r <$FERR; do
        # ignore irrelevant GTK warnings
        if test "$r" != "" && ! grep -qe "WARNING"<<< "$r"; then
            break;
        fi
    done
    if grep -qFe "${2:0:300}" <<< "${r:0:300}"; then
        count_ok
        echo " $OK ${r:0:300}"
    else
        count_fail
        echo " $FAIL     ${r:0:300}"
        echo " $EXPECTED ${2:0:300}"
    fi
}

read r 2< $FERR &
./pipeglade -i $FIN 2> $FERR &
# wait for $FIN to appear
while test ! \( -e $FIN \); do :; done

if test $AUTOMATIC; then
    # Non-existent id
    check_error "nnn" \
                "ignoring command \"nnn\""
    check_error "BIG_STRING" \
                "ignoring command \"BIG_STRING\""
    check_error "nnn:set_text FFFF" \
                "ignoring command \"nnn:set_text FFFF\""
    check_error "nnn:set_text $BIG_STRING" \
                "ignoring command \"nnn:set_text $BIG_STRING\""
    check_error "nnn:set_tooltip_text FFFF" \
                "ignoring command \"nnn:set_tooltip_text FFFF\""
    check_error "nnn:set_tooltip_text $BIG_STRING" \
                "ignoring command \"nnn:set_tooltip_text $BIG_STRING\""
    check_error "nnn:set_sensitive 0" \
                "ignoring command \"nnn:set_sensitive 0\""
    check_error "nnn:set_sensitive 1" \
                "ignoring command \"nnn:set_sensitive 1\""
    check_error "nnn:set_visible 0" \
                "ignoring command \"nnn:set_visible 0\""
    check_error "nnn:set_visible 1" \
                "ignoring command \"nnn:set_visible 1\""
    check_error "nnn:grab_focus" \
                "ignoring command \"nnn:grab_focus\""
    check_error "nnn:set_size_request 100 100" \
                "ignoring command \"nnn:set_size_request 100 100\""
    check_error "nnn:set_size_request $BIG_NUM $BIG_NUM" \
                "ignoring command \"nnn:set_size_request $BIG_NUM $BIG_NUM\""
    check_error "nnn:style font:Bold 11" \
                "ignoring command \"nnn:style font:Bold 11\""
    check_error "nnn:force" \
                "ignoring command \"nnn:force\""
    check_error "nnn:block 1" \
                "ignoring command \"nnn:block 1\""
    # Illegal id
    check_error "+:main_quit" \
                "ignoring command \"+:main_quit"
    check_error "=:main_quit" \
                "ignoring command \"=:main_quit"
    check_error "|:main_quit" \
                "ignoring command \"|:main_quit"
    # Wrong number or kind of arguments for generic actions
    check_error "button1:set_sensitive" \
                "ignoring GtkButton command \"button1:set_sensitive\""
    check_error "button1:set_sensitive 2" \
                "ignoring GtkButton command \"button1:set_sensitive 2\""
    check_error "button1:set_sensitive $BIG_NUM" \
                "ignoring GtkButton command \"button1:set_sensitive $BIG_NUM\""
    check_error "button1:set_sensitive nnn" \
                "ignoring GtkButton command \"button1:set_sensitive nnn\""
    check_error "button1:set_sensitive 0 1" \
                "ignoring GtkButton command \"button1:set_sensitive 0 1\""
    check_error "button1:set_visible" \
                "ignoring GtkButton command \"button1:set_visible\""
    check_error "button1:set_visible 2" \
                "ignoring GtkButton command \"button1:set_visible 2\""
    check_error "button1:set_visible $BIG_NUM" \
                "ignoring GtkButton command \"button1:set_visible $BIG_NUM\""
    check_error "button1:set_visible nnn" \
                "ignoring GtkButton command \"button1:set_visible nnn\""
    check_error "button1:set_visible 0 1" \
                "ignoring GtkButton command \"button1:set_visible 0 1\""
    check_error "button1:grab_focus 2" \
                "ignoring GtkButton command \"button1:grab_focus 2\""
    check_error "button1:set_size_request 100" \
                "ignoring GtkButton command \"button1:set_size_request 100\""
    check_error "button1:set_size_request 100 100 100" \
                "ignoring GtkButton command \"button1:set_size_request 100 100 100\""
    check_error "button1:force 2" \
                "ignoring GtkButton command \"button1:force 2\""
    check_error "_:main_quit 2" \
                "ignoring command \"_:main_quit 2\""
    check_error "button1:block 2" \
                "ignoring GtkButton command \"button1:block 2\""
    check_error "button1:block 0 0" \
                "ignoring GtkButton command \"button1:block 0 0\""
    check_error "button1:snapshot" \
                "ignoring GtkButton command \"button1:snapshot\""
    check_error "button1:snapshot " \
                "ignoring GtkButton command \"button1:snapshot \""
    # Widget that shouldn't fire callbacks
    check_error "label1:force" \
                "ignoring GtkLabel command \"label1:force\""
    # Widget that can't grab focus
    check_error "label1:grab_focus" \
                "ignoring GtkLabel command \"label1:grab_focus\""
    # load file
    check_error "_:load" \
                "ignoring command \"_:load\""
    check_error "_:load " \
                "ignoring command \"_:load \""
    check_error "_:load nonexistent.txt" \
                "ignoring command \"_:load nonexistent.txt\""
    for i in $WEIRD_PATHS; do
        check_error "_:load nonexistent/${i}QqQ" \
                    "ignoring command \"_:load nonexistent/${i}QqQ\""
    done
    # _:load junk
    mkdir -p $DIR
    cat >$DIR/$FILE1 <<< "blah"
    check_error "_:load $DIR/$FILE1" \
                "ignoring command \"blah\""
    for i in $WEIRD_PATHS; do
        cat >$i <<< "blah"
        check_error "_:load $i" \
                    "ignoring command \"blah\""
    done
    # recursive :load
    cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE1"
    check_error "_:load $DIR/$FILE1" \
                "ignoring command \"_:load $DIR/$FILE1\""
    for i in $WEIRD_PATHS; do
        cat >$i <<< "_:load $i"
        check_error "_:load $i" \
                    "ignoring command \"_:load $i\""
    done
    cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE2"
    cat >$DIR/$FILE2 <<< "_:load $DIR/$FILE1"
    check_error "_:load $DIR/$FILE1" \
                "ignoring command \"_:load $DIR/$FILE1\""
    cat >$DIR/$FILE1 <<< "_:load $DIR/$FILE2"
    cat >$DIR/$FILE2 <<< "_:blah"
    check_error "_:load $DIR/$FILE1" \
                "ignoring command \"_:blah\""
    rm -rf $DIR
    # GtkWindow
    check_error "main:nnn" \
                "ignoring GtkWindow command \"main:nnn\""
    check_error "main:move" \
                "ignoring GtkWindow command \"main:move\""
    check_error "main:move " \
                "ignoring GtkWindow command \"main:move \""
    check_error "main:move 700" \
                "ignoring GtkWindow command \"main:move 700\""
    check_error "main:move 700 nnn" \
                "ignoring GtkWindow command \"main:move 700 nnn\""
    check_error "main:move $BIG_NUM $BIG_STRING" \
                "ignoring GtkWindow command \"main:move $BIG_NUM $BIG_STRING\""
    check_error "main:move 700 700 700" \
                "ignoring GtkWindow command \"main:move 700 700 700\""
    check_error "main:resize 700" \
                "ignoring GtkWindow command \"main:resize 700\""
    check_error "main:resize 700 nnn" \
                "ignoring GtkWindow command \"main:resize 700 nnn\""
    check_error "main:resize 700 700 700" \
                "ignoring GtkWindow command \"main:resize 700 700 700\""
    check_error "main:fullscreen 1" \
                "ignoring GtkWindow command \"main:fullscreen 1\""
    check_error "main:unfullscreen 1" \
                "ignoring GtkWindow command \"main:unfullscreen 1\""
    # GtkLabel
    check_error "label1:nnn" \
                "ignoring GtkLabel command \"label1:nnn\""
    # GtkImage
    check_error "image1:nnn" \
                "ignoring GtkImage command \"image1:nnn\""
    # GtkNotebook
    check_error "notebook1:nnn" \
                "ignoring GtkNotebook command \"notebook1:nnn\""
    check_error "notebook1:set_current_page" \
                "ignoring GtkNotebook command \"notebook1:set_current_page\""
    check_error "notebook1:set_current_page " \
                "ignoring GtkNotebook command \"notebook1:set_current_page \""
    check_error "notebook1:set_current_page nnn" \
                "ignoring GtkNotebook command \"notebook1:set_current_page nnn\""
    check_error "notebook1:set_current_page -1" \
                "ignoring GtkNotebook command \"notebook1:set_current_page -1\""
    check_error "notebook1:set_current_page $BIG_NUM" \
                "ignoring GtkNotebook command \"notebook1:set_current_page $BIG_NUM\""
    check_error "notebook1:set_current_page 1 1" \
                "ignoring GtkNotebook command \"notebook1:set_current_page 1 1\""
    # GtkExpander
    check_error "expander1:nnn" \
                "ignoring GtkExpander command \"expander1:nnn\""
    check_error "expander1:set_expanded" \
                "ignoring GtkExpander command \"expander1:set_expanded\""
    check_error "expander1:set_expanded 3" \
                "ignoring GtkExpander command \"expander1:set_expanded 3\""
    check_error "expander1:set_expanded $BIG_NUM" \
                "ignoring GtkExpander command \"expander1:set_expanded $BIG_NUM\""
    check_error "expander1:set_expanded 0 1" \
                "ignoring GtkExpander command \"expander1:set_expanded 0 1\""
    # GtkTextView
    check_error "textview1:nnn" \
                "ignoring GtkTextView command \"textview1:nnn\""
    check_error "textview1:save" \
                "ignoring GtkTextView command \"textview1:save\""
    check_error "textview1:delete nnn" \
                "ignoring GtkTextView command \"textview1:delete nnn\""
    check_error "textview1:place_cursor" \
                "ignoring GtkTextView command \"textview1:place_cursor\""
    check_error "textview1:place_cursor " \
                "ignoring GtkTextView command \"textview1:place_cursor \""
    check_error "textview1:place_cursor nnn" \
                "ignoring GtkTextView command \"textview1:place_cursor nnn\""
    check_error "textview1:place_cursor 1 1" \
                "ignoring GtkTextView command \"textview1:place_cursor 1 1\""
    check_error "textview1:place_cursor end 1" \
                "ignoring GtkTextView command \"textview1:place_cursor end 1\""
    check_error "textview1:place_cursor_at_line" \
                "ignoring GtkTextView command \"textview1:place_cursor_at_line\""
    check_error "textview1:place_cursor_at_line " \
                "ignoring GtkTextView command \"textview1:place_cursor_at_line \""
    check_error "textview1:place_cursor_at_line nnn" \
                "ignoring GtkTextView command \"textview1:place_cursor_at_line nnn\""
    check_error "textview1:place_cursor_at_line 1 1" \
                "ignoring GtkTextView command \"textview1:place_cursor_at_line 1 1\""
    check_error "textview1:scroll_to_cursor nnn" \
                "ignoring GtkTextView command \"textview1:scroll_to_cursor nnn\""
    mkdir $DIR; chmod a-w $DIR
    check_error "textview1:save $DIR/$FILE1" \
                "ignoring GtkTextView command \"textview1:save $DIR/$FILE1\""
    check_error "textview1:save nonexistent/$FILE1" \
                "ignoring GtkTextView command \"textview1:save nonexistent/$FILE1\""
    for i in $WEIRD_PATHS; do
        check_error "textview1:save nonexistent/$i" \
                    "ignoring GtkTextView command \"textview1:save nonexistent/$i\""
    done

    rm -rf $DIR
    # GtkButton
    check_error "button1:nnn" \
                "ignoring GtkButton command \"button1:nnn\""
    # GtkSwitch
    check_error "switch1:nnn" \
                "ignoring GtkSwitch command \"switch1:nnn\""
    check_error "switch1:set_active" \
                "ignoring GtkSwitch command \"switch1:set_active\""
    check_error "switch1:set_active " \
                "ignoring GtkSwitch command \"switch1:set_active \""
    check_error "switch1:set_active 2" \
                "ignoring GtkSwitch command \"switch1:set_active 2\""
    check_error "switch1:set_active $BIG_NUM" \
                "ignoring GtkSwitch command \"switch1:set_active $BIG_NUM\""
    check_error "switch1:set_active 0 1" \
                "ignoring GtkSwitch command \"switch1:set_active 0 1\""
    # GtkToggleButton
    check_error "togglebutton1:nnn" \
                "ignoring GtkToggleButton command \"togglebutton1:nnn\""
    check_error "togglebutton1:set_active" \
                "ignoring GtkToggleButton command \"togglebutton1:set_active\""
    check_error "togglebutton1:set_active " \
                "ignoring GtkToggleButton command \"togglebutton1:set_active \""
    check_error "togglebutton1:set_active 2" \
                "ignoring GtkToggleButton command \"togglebutton1:set_active 2\""
    check_error "togglebutton1:set_active $BIG_NUM" \
                "ignoring GtkToggleButton command \"togglebutton1:set_active $BIG_NUM\""
    check_error "togglebutton1:set_active 1 0" \
                "ignoring GtkToggleButton command \"togglebutton1:set_active 1 0\""
    # GtkCheckButton
    check_error "checkbutton1:nnn" \
                "ignoring GtkCheckButton command \"checkbutton1:nnn\""
    check_error "checkbutton1:set_active" \
                "ignoring GtkCheckButton command \"checkbutton1:set_active\""
    check_error "checkbutton1:set_active " \
                "ignoring GtkCheckButton command \"checkbutton1:set_active \""
    check_error "checkbutton1:set_active 2" \
                "ignoring GtkCheckButton command \"checkbutton1:set_active 2\""
    check_error "checkbutton1:set_active $BIG_NUM" \
                "ignoring GtkCheckButton command \"checkbutton1:set_active $BIG_NUM\""
    check_error "checkbutton1:set_active 1 1" \
                "ignoring GtkCheckButton command \"checkbutton1:set_active 1 1\""
    # GtkRadioButton
    check_error "radiobutton1:nnn" \
                "ignoring GtkRadioButton command \"radiobutton1:nnn\""
    check_error "radiobutton1:set_active" \
                "ignoring GtkRadioButton command \"radiobutton1:set_active\""
    check_error "radiobutton1:set_active " \
                "ignoring GtkRadioButton command \"radiobutton1:set_active \""
    check_error "radiobutton1:set_active 2" \
                "ignoring GtkRadioButton command \"radiobutton1:set_active 2\""
    check_error "radiobutton1:set_active $BIG_NUM" \
                "ignoring GtkRadioButton command \"radiobutton1:set_active $BIG_NUM\""
    check_error "radiobutton1:set_active nnn" \
                "ignoring GtkRadioButton command \"radiobutton1:set_active nnn\""
    check_error "radiobutton1:set_active 0 1" \
                "ignoring GtkRadioButton command \"radiobutton1:set_active 0 1\""
    # GtkSpinButton
    check_error "spinbutton1:nnn" \
                "ignoring GtkSpinButton command \"spinbutton1:nnn\""
    check_error "spinbutton1:set_text" \
                "ignoring GtkSpinButton command \"spinbutton1:set_text\""
    check_error "spinbutton1:set_text " \
                "ignoring GtkSpinButton command \"spinbutton1:set_text \""
    check_error "spinbutton1:set_text nnn" \
                "ignoring GtkSpinButton command \"spinbutton1:set_text nnn\""
    check_error "spinbutton1:set_text $BIG_STRING" \
                "ignoring GtkSpinButton command \"spinbutton1:set_text $BIG_STRING\""
    check_error "spinbutton1:set_text 10 10" \
                "ignoring GtkSpinButton command \"spinbutton1:set_text 10 10\""
    check_error "spinbutton1:set_range" \
                "ignoring GtkSpinButton command \"spinbutton1:set_range\""
    check_error "spinbutton1:set_range " \
                "ignoring GtkSpinButton command \"spinbutton1:set_range \""
    check_error "spinbutton1:set_range 10 nnn" \
                "ignoring GtkSpinButton command \"spinbutton1:set_range 10 nnn\""
    check_error "spinbutton1:set_range 10 20 10" \
                "ignoring GtkSpinButton command \"spinbutton1:set_range 10 20 10\""
    check_error "spinbutton1:set_increments" \
                "ignoring GtkSpinButton command \"spinbutton1:set_increments\""
    check_error "spinbutton1:set_increments " \
                "ignoring GtkSpinButton command \"spinbutton1:set_increments \""
    check_error "spinbutton1:set_increments 10 nnn" \
                "ignoring GtkSpinButton command \"spinbutton1:set_increments 10 nnn\""
    check_error "spinbutton1:set_increments 10 20 10" \
                "ignoring GtkSpinButton command \"spinbutton1:set_increments 10 20 10\""
    # GtkDialog
    check_error "dialog1:resize 100" \
                "ignoring GtkDialog command \"dialog1:resize 100\""
    check_error "dialog1:resize 100 100 100" \
                "ignoring GtkDialog command \"dialog1:resize 100 100 100\""
    check_error "dialog1:move" \
                "ignoring GtkDialog command \"dialog1:move\""
    check_error "dialog1:move " \
                "ignoring GtkDialog command \"dialog1:move \""
    check_error "dialog1:move 100" \
                "ignoring GtkDialog command \"dialog1:move 100\""
    check_error "dialog1:move 100 100 100" \
                "ignoring GtkDialog command \"dialog1:move 100 100 100\""
    check_error "dialog1:fullscreen 1" \
                "ignoring GtkDialog command \"dialog1:fullscreen 1\""
    check_error "dialog1:unfullscreen 1" \
                "ignoring GtkDialog command \"dialog1:unfullscreen 1\""
    # GtkFileChooserButton
    check_error "filechooserbutton1:nnn" \
                "ignoring GtkFileChooserButton command \"filechooserbutton1:nnn\""
    # GtkFilechooserDialog
    check_error "open_dialog:nnn" \
                "ignoring GtkFileChooserDialog command \"open_dialog:nnn\""
    check_error "open_dialog:resize 100" \
                "ignoring GtkFileChooserDialog command \"open_dialog:resize 100\""
    check_error "open_dialog:resize 100 100 100" \
                "ignoring GtkFileChooserDialog command \"open_dialog:resize 100 100 100\""
    check_error "open_dialog:move" \
                "ignoring GtkFileChooserDialog command \"open_dialog:move\""
    check_error "open_dialog:move " \
                "ignoring GtkFileChooserDialog command \"open_dialog:move \""
    check_error "open_dialog:move 100" \
                "ignoring GtkFileChooserDialog command \"open_dialog:move 100\""
    check_error "open_dialog:move 100 100 100" \
                "ignoring GtkFileChooserDialog command \"open_dialog:move 100 100 100\""
    check_error "open_dialog:fullscreen 1" \
                "ignoring GtkFileChooserDialog command \"open_dialog:fullscreen 1\""
    check_error "open_dialog:unfullscreen 1" \
                "ignoring GtkFileChooserDialog command \"open_dialog:unfullscreen 1\""
    # GtkFontButton
    check_error "fontbutton1:nnn" \
                "ignoring GtkFontButton command \"fontbutton1:nnn\""
    # GtkColorButton
    check_error "colorbutton1:nnn" \
                "ignoring GtkColorButton command \"colorbutton1:nnn\""
    # GtkPrintUnixDialog
    check_error "printdialog:nnn" \
                "ignoring GtkPrintUnixDialog command \"printdialog:nnn\""
fi
if test $INTERACTIVE; then
    check_error "statusbar1:push Click \"Print\"\n printdialog:print nonexistent.ps" \
                "ignoring GtkPrintUnixDialog command \" printdialog:print nonexistent.ps\""
fi;
if test $AUTOMATIC; then
    # GtkScale
    check_error "scale1:nnn" \
                "ignoring GtkScale command \"scale1:nnn\""
    check_error "scale1:set_value" \
                "ignoring GtkScale command \"scale1:set_value\""
    check_error "scale1:set_value " \
                "ignoring GtkScale command \"scale1:set_value \""
    check_error "scale1:set_value nnn" \
                "ignoring GtkScale command \"scale1:set_value nnn\""
    check_error "scale1:set_value $BIG_STRING" \
                "ignoring GtkScale command \"scale1:set_value $BIG_STRING\""
    check_error "scale1:set_value 10 10" \
                "ignoring GtkScale command \"scale1:set_value 10 10\""
    check_error "scale1:set_fill_level nnn" \
                "ignoring GtkScale command \"scale1:set_fill_level nnn\""
    check_error "scale1:set_fill_level $BIG_STRING" \
                "ignoring GtkScale command \"scale1:set_fill_level $BIG_STRING\""
    check_error "scale1:set_fill_level 10 10" \
                "ignoring GtkScale command \"scale1:set_fill_level 10 10\""
    check_error "scale1:set_range" \
                "ignoring GtkScale command \"scale1:set_range\""
    check_error "scale1:set_range " \
                "ignoring GtkScale command \"scale1:set_range \""
    check_error "scale1:set_range 10" \
                "ignoring GtkScale command \"scale1:set_range 10\""
    check_error "scale1:set_range $BIG_NUM" \
                "ignoring GtkScale command \"scale1:set_range $BIG_NUM\""
    check_error "scale1:set_range x 10" \
                "ignoring GtkScale command \"scale1:set_range x 10\""
    check_error "scale1:set_range 10 10 10" \
                "ignoring GtkScale command \"scale1:set_range 10 10 10\""
    check_error "scale1:set_increments" \
                "ignoring GtkScale command \"scale1:set_increments\""
    check_error "scale1:set_increments " \
                "ignoring GtkScale command \"scale1:set_increments \""
    check_error "scale1:set_increments 10" \
                "ignoring GtkScale command \"scale1:set_increments 10\""
    check_error "scale1:set_increments $BIG_NUM" \
                "ignoring GtkScale command \"scale1:set_increments $BIG_NUM\""
    check_error "scale1:set_increments x 10" \
                "ignoring GtkScale command \"scale1:set_increments x 10\""
    check_error "scale1:set_increments 10 10 10" \
                "ignoring GtkScale command \"scale1:set_increments 10 10 10\""
    # GtkProgressBar
    check_error "progressbar1:nnn" \
                "ignoring GtkProgressBar command \"progressbar1:nnn\""
    check_error "progressbar1:set_fraction" \
                "ignoring GtkProgressBar command \"progressbar1:set_fraction\""
    check_error "progressbar1:set_fraction " \
                "ignoring GtkProgressBar command \"progressbar1:set_fraction \""
    check_error "progressbar1:set_fraction nnn" \
                "ignoring GtkProgressBar command \"progressbar1:set_fraction nnn\""
    check_error "progressbar1:set_fraction $BIG_STRING" \
                "ignoring GtkProgressBar command \"progressbar1:set_fraction $BIG_STRING\""
    check_error "progressbar1:set_fraction .5 1" \
                "ignoring GtkProgressBar command \"progressbar1:set_fraction .5 1\""
    # GtkSpinner
    check_error "spinner1:nnn" \
                "ignoring GtkSpinner command \"spinner1:nnn\""
    check_error "spinner1:start 1" \
                "ignoring GtkSpinner command \"spinner1:start 1\""
    check_error "spinner1:start $BIG_STRING" \
                "ignoring GtkSpinner command \"spinner1:start $BIG_STRING\""
    check_error "spinner1:stop 1" \
                "ignoring GtkSpinner command \"spinner1:stop 1\""
    check_error "spinner1:stop $BIG_STRING" \
                "ignoring GtkSpinner command \"spinner1:stop $BIG_STRING\""
    # GtkStatusbar
    check_error "statusbar1:nnn" \
                "ignoring GtkStatusbar command \"statusbar1:nnn\""
    check_error "statusbar1:push_id" \
                "ignoring GtkStatusbar command \"statusbar1:push_id\""
    check_error "statusbar1:pop_id" \
                "ignoring GtkStatusbar command \"statusbar1:pop_id\""
    check_error "statusbar1:remove_all_id" \
                "ignoring GtkStatusbar command \"statusbar1:remove_all_id\""
    check_error "statusbar1:remove_all_id " \
                "ignoring GtkStatusbar command \"statusbar1:remove_all_id \""
    check_error "statusbar1:remove_all_id a b" \
                "ignoring GtkStatusbar command \"statusbar1:remove_all_id a b\""
    check_error "statusbar1:remove_all a" \
                "ignoring GtkStatusbar command \"statusbar1:remove_all a\""
    check_error "statusbar1:remove_all $BIG_STRING" \
                "ignoring GtkStatusbar command \"statusbar1:remove_all $BIG_STRING\""
    # GtkComboBoxText
    check_error "comboboxtext1:nnn" \
                "ignoring GtkComboBoxText command \"comboboxtext1:nnn\""
    check_error "comboboxtext1:force" \
                "ignoring GtkComboBoxText command \"comboboxtext1:force\""
    check_error "comboboxtext1:insert_text" \
                "ignoring GtkComboBoxText command \"comboboxtext1:insert_text\""
    check_error "comboboxtext1:insert_text x y" \
                "ignoring GtkComboBoxText command \"comboboxtext1:insert_text x y\""
    check_error "comboboxtext1:remove" \
                "ignoring GtkComboBoxText command \"comboboxtext1:remove\""
    check_error "comboboxtext1:remove x y" \
                "ignoring GtkComboBoxText command \"comboboxtext1:remove x y\""

    # GtkTreeView #
    check_error "treeview1:nnn" \
                "ignoring GtkTreeView command \"treeview1:nnn\""
    check_error "treeview2:nnn" \
                "ignoring GtkTreeView command \"treeview2:nnn\""
    check_error "treeview1:force" \
                "ignoring GtkTreeView command \"treeview1:force\""
    # GtkTreeView save
    check_error "treeview1:save" \
                "ignoring GtkTreeView command \"treeview1:save\""
    mkdir $DIR; chmod a-w $DIR
    check_error "treeview1:save $DIR/$FILE1" \
                "ignoring GtkTreeView command \"treeview1:save $DIR/$FILE1\""
    check_error "treeview1:save nonexistent/$FILE1" \
                "ignoring GtkTreeView command \"treeview1:save nonexistent/$FILE1\""
    rm -rf $DIR
    for i in $WEIRD_PATHS; do
        check_error "treeview1:save nonexistent/$i" \
                    "ignoring GtkTreeView command \"treeview1:save nonexistent/$i\""
    done
    # GtkTreeView insert_row
    check_error "treeview1:insert_row 10000" \
                "ignoring GtkTreeView command \"treeview1:insert_row 10000\""
    check_error "treeview1:insert_row $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:insert_row $BIG_STRING\""
    check_error "treeview1:insert_row -1" \
                "ignoring GtkTreeView command \"treeview1:insert_row -1\""
    check_error "treeview1:insert_row nnn" \
                "ignoring GtkTreeView command \"treeview1:insert_row nnn\""
    check_error "treeview1:insert_row" \
                "ignoring GtkTreeView command \"treeview1:insert_row\""
    check_error "treeview1:insert_row " \
                "ignoring GtkTreeView command \"treeview1:insert_row \""
    check_error "treeview1:insert_row -1" \
                "ignoring GtkTreeView command \"treeview1:insert_row -1\""
    check_error "treeview1:insert_row 1000" \
                "ignoring GtkTreeView command \"treeview1:insert_row 1000\""
    check_error "treeview2:insert_row 0" \
                "ignoring GtkTreeView command \"treeview2:insert_row 0\""
    check_error "treeview3:insert_row end" \
                "missing model/ignoring GtkTreeView command \"treeview3:insert_row end\""
    check_error "treeview2:insert_row end\n treeview2:insert_row 0 as_child\n treeview2:insert_row 0:0 as_child\n treeview2:expand abc" \
                "ignoring GtkTreeView command \" treeview2:expand abc\""
    check_error "treeview2:expand" \
                "ignoring GtkTreeView command \"treeview2:expand\""
    check_error "treeview2:expand 0:abc" \
                "ignoring GtkTreeView command \"treeview2:expand 0:abc\""
    check_error "treeview2:expand 0 0" \
                "ignoring GtkTreeView command \"treeview2:expand 0 0\""
    check_error "treeview2:expand_all abc" \
                "ignoring GtkTreeView command \"treeview2:expand_all abc\""
    check_error "treeview2:expand_all $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview2:expand_all $BIG_STRING\""
    check_error "treeview2:expand_all 0:abc" \
                "ignoring GtkTreeView command \"treeview2:expand_all 0:abc\""
    check_error "treeview2:expand_all 0 0" \
                "ignoring GtkTreeView command \"treeview2:expand_all 0 0\""
    check_error "treeview2:collapse abc" \
                "ignoring GtkTreeView command \"treeview2:collapse abc\""
    check_error "treeview2:collapse $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview2:collapse $BIG_STRING\""
    check_error "treeview2:collapse 0:abc" \
                "ignoring GtkTreeView command \"treeview2:collapse 0:abc\""
    check_error "treeview2:collapse 0 0" \
                "ignoring GtkTreeView command \"treeview2:collapse 0 0\""
    check_error "treeview2:insert_row" \
                "ignoring GtkTreeView command \"treeview2:insert_row\""
    check_error "treeview2:insert_row abc" \
                "ignoring GtkTreeView command \"treeview2:insert_row abc\""
    check_error "treeview2:insert_row 0:abc" \
                "ignoring GtkTreeView command \"treeview2:insert_row 0:abc\""
    check_error "treeview2:insert_row end 0" \
                "ignoring GtkTreeView command \"treeview2:insert_row end 0\""
    # GtkTreeView move_row
    check_error "treeview1:move_row" \
                "ignoring GtkTreeView command \"treeview1:move_row\""
    check_error "treeview1:move_row " \
                "ignoring GtkTreeView command \"treeview1:move_row \""
    check_error "treeview1:move_row nnn" \
                "ignoring GtkTreeView command \"treeview1:move_row nnn\""
    check_error "treeview1:move_row $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:move_row $BIG_STRING\""
    check_error "treeview1:move_row 10000 end" \
                "ignoring GtkTreeView command \"treeview1:move_row 10000 end\""
    check_error "treeview1:move_row $BIG_STRING end" \
                "ignoring GtkTreeView command \"treeview1:move_row $BIG_STRING end\""
    check_error "treeview1:move_row -1 end" \
                "ignoring GtkTreeView command \"treeview1:move_row -1 end\""
    check_error "treeview1:move_row nnn end" \
                "ignoring GtkTreeView command \"treeview1:move_row nnn end\""
    check_error "treeview1:move_row 0 10000" \
                "ignoring GtkTreeView command \"treeview1:move_row 0 10000\""
    check_error "treeview1:move_row 0 -1" \
                "ignoring GtkTreeView command \"treeview1:move_row 0 -1\""
    check_error "treeview1:move_row 0 nnn" \
                "ignoring GtkTreeView command \"treeview1:move_row 0 nnn\""
    check_error "treeview2:move_row" \
                "ignoring GtkTreeView command \"treeview2:move_row\""
    check_error "treeview2:move_row 0:0 abc" \
                "ignoring GtkTreeView command \"treeview2:move_row 0:0 abc\""
    check_error "treeview2:move_row 0:0 0:abc" \
                "ignoring GtkTreeView command \"treeview2:move_row 0:0 0:abc\""
    check_error "treeview2:move_row abc end" \
                "ignoring GtkTreeView command \"treeview2:move_row abc end\""
    check_error "treeview2:move_row 0:abc end" \
                "ignoring GtkTreeView command \"treeview2:move_row 0:abc end\""
    check_error "treeview2:move_row 0 end 0" \
                "ignoring GtkTreeView command \"treeview2:move_row 0 end 0\""
    # GtkTreeView remove_row
    check_error "treeview1:remove_row 10000" \
                "ignoring GtkTreeView command \"treeview1:remove_row 10000\""
    check_error "treeview1:remove_row -1" \
                "ignoring GtkTreeView command \"treeview1:remove_row -1\""
    check_error "treeview1:remove_row nnn" \
                "ignoring GtkTreeView command \"treeview1:remove_row nnn\""
    check_error "treeview1:remove_row $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:remove_row $BIG_STRING\""
    check_error "treeview1:remove_row" \
                "ignoring GtkTreeView command \"treeview1:remove_row\""
    check_error "treeview1:remove_row " \
                "ignoring GtkTreeView command \"treeview1:remove_row \""
    check_error "treeview2:remove_row" \
                "ignoring GtkTreeView command \"treeview2:remove_row\""
    check_error "treeview2:remove_row abc" \
                "ignoring GtkTreeView command \"treeview2:remove_row abc\""
    check_error "treeview2:remove_row 0:abc" \
                "ignoring GtkTreeView command \"treeview2:remove_row 0:abc\""
    check_error "treeview2:remove_row 0 0" \
                "ignoring GtkTreeView command \"treeview2:remove_row 0 0\""
    # GtkTreeView scroll
    check_error "treeview1:scroll" \
                "ignoring GtkTreeView command \"treeview1:scroll\""
    check_error "treeview1:scroll " \
                "ignoring GtkTreeView command \"treeview1:scroll \""
    check_error "treeview1:scroll nnn" \
                "ignoring GtkTreeView command \"treeview1:scroll nnn\""
    check_error "treeview1:scroll $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:scroll $BIG_STRING\""
    check_error "treeview1:scroll -1 1" \
                "ignoring GtkTreeView command \"treeview1:scroll -1 1\""
    check_error "treeview1:scroll 1 -1" \
                "ignoring GtkTreeView command \"treeview1:scroll 1 -1\""
    check_error "treeview1:scroll nnn 1" \
                "ignoring GtkTreeView command \"treeview1:scroll nnn 1\""
    check_error "treeview1:scroll 1 nnn" \
                "ignoring GtkTreeView command \"treeview1:scroll 1 nnn\""
    check_error "treeview1:scroll 0 0 0" \
                "ignoring GtkTreeView command \"treeview1:scroll 0 0 0\""
    check_error "treeview2:scroll" \
                "ignoring GtkTreeView command \"treeview2:scroll\""
    check_error "treeview2:scroll abc" \
                "ignoring GtkTreeView command \"treeview2:scroll abc\""
    check_error "treeview2:scroll 0:abc" \
                "ignoring GtkTreeView command \"treeview2:scroll 0:abc\""
    check_error "treeview2:scroll abc 0" \
                "ignoring GtkTreeView command \"treeview2:scroll abc 0\""
    check_error "treeview2:scroll 0:abc 0" \
                "ignoring GtkTreeView command \"treeview2:scroll 0:abc 0\""
    check_error "treeview2:scroll 0:0" \
                "ignoring GtkTreeView command \"treeview2:scroll 0:0\""
    check_error "treeview2:scroll 0:0 abc" \
                "ignoring GtkTreeView command \"treeview2:scroll 0:0 abc\""
    check_error "treeview2:set_cursor abc" \
                "ignoring GtkTreeView command \"treeview2:set_cursor abc\""
    check_error "treeview2:set_cursor 0:abc" \
                "ignoring GtkTreeView command \"treeview2:set_cursor 0:abc\""
    check_error "treeview2:set_cursor 0 0" \
                "ignoring GtkTreeView command \"treeview2:set_cursor 0 0\""
    check_error "treeview2:set_cursor 0 $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview2:set_cursor 0 $BIG_STRING\""
    check_error "treeview2:clear 0" \
                "ignoring GtkTreeView command \"treeview2:clear 0\""
    check_error "treeview2:clear\n treeview2:insert_row 0" \
                "ignoring GtkTreeView command \" treeview2:insert_row 0\""
    # GtkTreeView set
    check_error "treeview1:set" \
                "ignoring GtkTreeView command \"treeview1:set\""
    check_error "treeview1:set " \
                "ignoring GtkTreeView command \"treeview1:set \""
    check_error "treeview1:set nnn" \
                "ignoring GtkTreeView command \"treeview1:set nnn\""
    check_error "treeview1:set $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:set $BIG_STRING\""
    check_error "treeview1:set 0 nnn" \
                "ignoring GtkTreeView command \"treeview1:set 0 nnn\""
    check_error "treeview1:set nnn 0" \
                "ignoring GtkTreeView command \"treeview1:set nnn 0\""
    check_error "treeview1:set 1 10000 77" \
                "ignoring GtkTreeView command \"treeview1:set 1 10000 77\""
    check_error "treeview1:set 1 11 77" \
                "ignoring GtkTreeView command \"treeview1:set 1 11 77\""
    check_error "treeview1:set nnn 1 77" \
                "ignoring GtkTreeView command \"treeview1:set nnn 1 77\""
    check_error "treeview1:set 1 nnn 77" \
                "ignoring GtkTreeView command \"treeview1:set 1 nnn 77\""
    check_error "treeview1:set -1 1 77" \
                "ignoring GtkTreeView command \"treeview1:set -1 1 77\""
    check_error "treeview1:set 1 -1 77" \
                "ignoring GtkTreeView command \"treeview1:set 1 -1 77\""
    # GtkTreeView set junk into numeric columns
    check_error "treeview1:set 1 1 abc" \
                "ignoring GtkTreeView command \"treeview1:set 1 1 abc\""
    check_error "treeview1:set 1 1 $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:set 1 1 $BIG_STRING\""
    check_error "treeview1:set 1 1" \
                "ignoring GtkTreeView command \"treeview1:set 1 1\""
    check_error "treeview1:set 1 1 555.5" \
                "ignoring GtkTreeView command \"treeview1:set 1 1 555.5\""
    check_error "treeview1:set 1 1 555 5" \
                "ignoring GtkTreeView command \"treeview1:set 1 1 555 5\""
    check_error "treeview1:set 1 7 abc" \
                "ignoring GtkTreeView command \"treeview1:set 1 7 abc\""
    check_error "treeview1:set 1 7 $BIG_STRING" \
                "ignoring GtkTreeView command \"treeview1:set 1 7 $BIG_STRING\""
    check_error "treeview1:set 1 7" \
                "ignoring GtkTreeView command \"treeview1:set 1 7\""
    check_error "treeview1:set 1 7 555 5" \
                "ignoring GtkTreeView command \"treeview1:set 1 7 555 5\""

    # GtkTreeViewColumn (which is not a GtkWidget)
    check_error "treeviewcolumn3:nnn" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:nnn\""
    check_error "treeviewcolumn3:force" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:force\""
    check_error "treeviewcolumn3:grab_focus" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:grab_focus\""
    check_error "treeviewcolumn3:snapshot x.svg" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:snapshot x.svg\""
    check_error "treeviewcolumn3:set_sensitive 0" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:set_sensitive 0\""
    check_error "treeviewcolumn3:set_size_request 50 60" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:set_size_request 50 60\""
    check_error "treeviewcolumn3:set_visible 0" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:set_visible 0\""
    check_error "treeviewcolumn3:style" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:style\""
    check_error "treeviewcolumn3:set_tooltip_text" \
                "ignoring GtkTreeViewColumn command \"treeviewcolumn3:set_tooltip_text\""

    # GtkEntry
    check_error "entry1:nnn" \
                "ignoring GtkEntry command \"entry1:nnn\""
    # GtkCalendar
    check_error "calendar1:nnn" \
                "ignoring GtkCalendar command \"calendar1:nnn\""
    check_error "calendar1:select_date" \
                "ignoring GtkCalendar command \"calendar1:select_date\""
    check_error "calendar1:select_date " \
                "ignoring GtkCalendar command \"calendar1:select_date \""
    check_error "calendar1:select_date nnn" \
                "ignoring GtkCalendar command \"calendar1:select_date nnn\""
    check_error "calendar1:select_date $BIG_STRING" \
                "ignoring GtkCalendar command \"calendar1:select_date $BIG_STRING\""
    check_error "calendar1:select_date 2000-12-33" \
                "ignoring GtkCalendar command \"calendar1:select_date 2000-12-33\""
    check_error "calendar1:select_date 2000-13-20" \
                "ignoring GtkCalendar command \"calendar1:select_date 2000-13-20\""
    check_error "calendar1:select_date 2000-10-10 1" \
                "ignoring GtkCalendar command \"calendar1:select_date 2000-10-10 1\""
    check_error "calendar1:mark_day" \
                "ignoring GtkCalendar command \"calendar1:mark_day\""
    check_error "calendar1:mark_day " \
                "ignoring GtkCalendar command \"calendar1:mark_day \""
    check_error "calendar1:mark_day nnn" \
                "ignoring GtkCalendar command \"calendar1:mark_day nnn\""
    check_error "calendar1:mark_day $BIG_STRING" \
                "ignoring GtkCalendar command \"calendar1:mark_day $BIG_STRING\""
    check_error "calendar1:mark_day 10 1" \
                "ignoring GtkCalendar command \"calendar1:mark_day 10 1\""
    check_error "calendar1:clear_marks 1" \
                "ignoring GtkCalendar command \"calendar1:clear_marks 1\""
    # GtkSocket
    check_error "socket1:nnn" \
                "ignoring GtkSocket command \"socket1:nnn\""
    check_error "socket1:id 1" \
                "ignoring GtkSocket command \"socket1:id 1\""
    # GtkScrolledWindow
    check_error "scrolledwindow3:nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:nnn\""
    check_error "scrolledwindow3:hscroll" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll\""
    check_error "scrolledwindow3:hscroll " \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll \""
    check_error "scrolledwindow3:hscroll nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll nnn\""
    check_error "scrolledwindow3:hscroll $BIG_STRING" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll $BIG_STRING\""
    check_error "scrolledwindow3:hscroll 100 100" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll 100 100\""
    check_error "scrolledwindow3:vscroll" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll\""
    check_error "scrolledwindow3:vscroll " \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll \""
    check_error "scrolledwindow3:vscroll nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll nnn\""
    check_error "scrolledwindow3:vscroll $BIG_STRING" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll $BIG_STRING\""
    check_error "scrolledwindow3:vscroll 100 100" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll 100 100\""
    check_error "scrolledwindow3:hscroll_to_range" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range\""
    check_error "scrolledwindow3:hscroll_to_range " \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range \""
    check_error "scrolledwindow3:hscroll_to_range nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range nnn\""
    check_error "scrolledwindow3:hscroll_to_range $BIG_STRING" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range $BIG_STRING\""
    check_error "scrolledwindow3:hscroll_to_range 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range 10\""
    check_error "scrolledwindow3:hscroll_to_range 10 nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range 10 nnn\""
    check_error "scrolledwindow3:hscroll_to_range nnn 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range nnn 10\""
    check_error "scrolledwindow3:hscroll_to_range 5 10 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:hscroll_to_range 5 10 10\""
    check_error "scrolledwindow3:vscroll_to_range" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range\""
    check_error "scrolledwindow3:vscroll_to_range " \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range \""
    check_error "scrolledwindow3:vscroll_to_range nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range nnn\""
    check_error "scrolledwindow3:vscroll_to_range $BIG_STRING" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range $BIG_STRING\""
    check_error "scrolledwindow3:vscroll_to_range 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range 10\""
    check_error "scrolledwindow3:vscroll_to_range 10 nnn" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range 10 nnn\""
    check_error "scrolledwindow3:vscroll_to_range nnn 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range nnn 10\""
    check_error "scrolledwindow3:vscroll_to_range 5 10 10" \
                "ignoring GtkScrolledWindow command \"scrolledwindow3:vscroll_to_range 5 10 10\""
    # GtkEventBox
    check_error "eventbox1:nnn" \
                "ignoring GtkEventBox command \"eventbox1:nnn\""
    # GtkDrawingArea
    check_error "drawingarea1:nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:nnn\""
    check_error "drawingarea1:rectangle" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle\""
    check_error "drawingarea1:rectangle " \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle \""
    check_error "drawingarea1:rectangle nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle nnn\""
    check_error "drawingarea1:rectangle $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle $BIG_STRING\""
    check_error "drawingarea1:rectangle 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1\""
    check_error "drawingarea1:rectangle 1 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10\""
    check_error "drawingarea1:rectangle 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10\""
    check_error "drawingarea1:rectangle 1 10 10 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10 20\""
    check_error "drawingarea1:rectangle 1 10 10 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10 20 nnn\""
    check_error "drawingarea1:rectangle 1 10 10 20 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rectangle 1 10 10 20 20 20\""
    check_error "drawingarea1:arc" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc\""
    check_error "drawingarea1:arc " \
                "ignoring GtkDrawingArea command \"drawingarea1:arc \""
    check_error "drawingarea1:arc nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc nnn\""
    check_error "drawingarea1:arc $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc $BIG_STRING\""
    check_error "drawingarea1:arc 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1\""
    check_error "drawingarea1:arc 1 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10\""
    check_error "drawingarea1:arc 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10\""
    check_error "drawingarea1:arc 1 10 10 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20\""
    check_error "drawingarea1:arc 1 10 10 20 45" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20 45\""
    check_error "drawingarea1:arc 1 10 10 20 45 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20 45 nnn\""
    check_error "drawingarea1:arc 1 10 10 20 45 90 7" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc 1 10 10 20 45 90 7\""
    check_error "drawingarea1:arc_negative" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative\""
    check_error "drawingarea1:arc_negative " \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative \""
    check_error "drawingarea1:arc_negative nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative nnn\""
    check_error "drawingarea1:arc_negative $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative $BIG_STRING\""
    check_error "drawingarea1:arc_negative 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1\""
    check_error "drawingarea1:arc_negative 1 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10\""
    check_error "drawingarea1:arc_negative 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10\""
    check_error "drawingarea1:arc_negative 1 10 10 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20\""
    check_error "drawingarea1:arc_negative 1 10 10 20 45" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20 45\""
    check_error "drawingarea1:arc_negative 1 10 10 20 45 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20 45 nnn\""
    check_error "drawingarea1:arc_negative 1 10 10 20 45 90 7" \
                "ignoring GtkDrawingArea command \"drawingarea1:arc_negative 1 10 10 20 45 90 7\""
    check_error "drawingarea1:curve_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to\""
    check_error "drawingarea1:curve_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to \""
    check_error "drawingarea1:curve_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to nnn\""
    check_error "drawingarea1:curve_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to $BIG_STRING\""
    check_error "drawingarea1:curve_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1\""
    check_error "drawingarea1:curve_to 1 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10\""
    check_error "drawingarea1:curve_to 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10\""
    check_error "drawingarea1:curve_to 1 10 10 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20\""
    check_error "drawingarea1:curve_to 1 10 10 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20\""
    check_error "drawingarea1:curve_to 1 10 10 20 20 25" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20 25\""
    check_error "drawingarea1:curve_to 1 10 10 20 20 25 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20 25 nnn\""
    check_error "drawingarea1:curve_to 1 10 10 20 20 25 25 77" \
                "ignoring GtkDrawingArea command \"drawingarea1:curve_to 1 10 10 20 20 25 25 77\""
    check_error "drawingarea1:rel_curve_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to\""
    check_error "drawingarea1:rel_curve_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to \""
    check_error "drawingarea1:rel_curve_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to nnn\""
    check_error "drawingarea1:rel_curve_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to $BIG_STRING\""
    check_error "drawingarea1:rel_curve_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1\""
    check_error "drawingarea1:rel_curve_to 1 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10\""
    check_error "drawingarea1:rel_curve_to 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10\""
    check_error "drawingarea1:rel_curve_to 1 10 10 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20\""
    check_error "drawingarea1:rel_curve_to 1 10 10 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20\""
    check_error "drawingarea1:rel_curve_to 1 10 10 20 20 25" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20 25\""
    check_error "drawingarea1:rel_curve_to 1 10 10 20 20 25 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20 25 nnn\""
    check_error "drawingarea1:rel_curve_to 1 10 10 20 20 25 25 77" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_curve_to 1 10 10 20 20 25 25 77\""
    check_error "drawingarea1:line_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to\""
    check_error "drawingarea1:line_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to \""
    check_error "drawingarea1:line_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to nnn\""
    check_error "drawingarea1:line_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to $BIG_STRING\""
    check_error "drawingarea1:line_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to 1\""
    check_error "drawingarea1:line_to 1 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to 1 20\""
    check_error "drawingarea1:line_to 1 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to 1 20 nnn\""
    check_error "drawingarea1:line_to 1 20 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:line_to 1 20 20 20\""
    check_error "drawingarea1:rel_line_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to\""
    check_error "drawingarea1:rel_line_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to \""
    check_error "drawingarea1:rel_line_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to nnn\""
    check_error "drawingarea1:rel_line_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to $BIG_STRING\""
    check_error "drawingarea1:rel_line_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1\""
    check_error "drawingarea1:rel_line_to 1 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20\""
    check_error "drawingarea1:rel_line_to 1 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20 nnn\""
    check_error "drawingarea1:rel_line_to 1 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20 nnn\""
    check_error "drawingarea1:rel_line_to 1 20 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_line_to 1 20 20 20\""
    check_error "drawingarea1:move_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to\""
    check_error "drawingarea1:move_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to \""
    check_error "drawingarea1:move_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to nnn\""
    check_error "drawingarea1:move_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to $BIG_STRING\""
    check_error "drawingarea1:move_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to 1\""
    check_error "drawingarea1:move_to 1 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to 1 20\""
    check_error "drawingarea1:move_to 1 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to 1 20 nnn\""
    check_error "drawingarea1:move_to 1 20 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:move_to 1 20 20 20\""
    check_error "drawingarea1:rel_move_to" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to\""
    check_error "drawingarea1:rel_move_to " \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to \""
    check_error "drawingarea1:rel_move_to nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to nnn\""
    check_error "drawingarea1:rel_move_to $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to $BIG_STRING\""
    check_error "drawingarea1:rel_move_to 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1\""
    check_error "drawingarea1:rel_move_to 1 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1 20\""
    check_error "drawingarea1:rel_move_to 1 20 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1 20 nnn\""
    check_error "drawingarea1:rel_move_to 1 20 20 20" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_to 1 20 20 20\""
    check_error "drawingarea1:close_path" \
                "ignoring GtkDrawingArea command \"drawingarea1:close_path\""
    check_error "drawingarea1:close_path " \
                "ignoring GtkDrawingArea command \"drawingarea1:close_path \""
    check_error "drawingarea1:close_path nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:close_path nnn\""
    check_error "drawingarea1:close_path $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:close_path $BIG_STRING\""
    check_error "drawingarea1:close_path 1 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:close_path 1 1\""
    check_error "drawingarea1:set_source_rgba" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba\""
    check_error "drawingarea1:set_source_rgba " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba \""
    check_error "drawingarea1:set_source_rgba nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba nnn\""
    check_error "drawingarea1:set_source_rgba $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_source_rgba $BIG_STRING\""
    check_error "drawingarea1:set_dash" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_dash\""
    check_error "drawingarea1:set_dash " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_dash \""
    check_error "drawingarea1:set_dash nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_dash nnn\""
    check_error "drawingarea1:set_dash $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_dash $BIG_STRING\""
    check_error "drawingarea1:set_line_cap" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap\""
    check_error "drawingarea1:set_line_cap " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap \""
    check_error "drawingarea1:set_line_cap nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap nnn\""
    check_error "drawingarea1:set_line_cap $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap $BIG_STRING\""
    check_error "drawingarea1:set_line_cap 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap 1\""
    check_error "drawingarea1:set_line_cap 1 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap 1 nnn\""
    check_error "drawingarea1:set_line_cap 1 butt butt" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_cap 1 butt butt\""
    check_error "drawingarea1:set_line_join" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join\""
    check_error "drawingarea1:set_line_join " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join \""
    check_error "drawingarea1:set_line_join nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join nnn\""
    check_error "drawingarea1:set_line_join $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join $BIG_STRING\""
    check_error "drawingarea1:set_line_join 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join 1\""
    check_error "drawingarea1:set_line_join 1 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join 1 nnn\""
    check_error "drawingarea1:set_line_join 1 miter miter" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_join 1 miter miter\""
    check_error "drawingarea1:set_line_width" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width\""
    check_error "drawingarea1:set_line_width " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width \""
    check_error "drawingarea1:set_line_width nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width nnn\""
    check_error "drawingarea1:set_line_width $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width $BIG_STRING\""
    check_error "drawingarea1:set_line_width 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width 1\""
    check_error "drawingarea1:set_line_width 1 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width 1 nnn\""
    check_error "drawingarea1:set_line_width 1 3 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_line_width 1 3 3\""
    check_error "drawingarea1:fill" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill\""
    check_error "drawingarea1:fill " \
                "ignoring GtkDrawingArea command \"drawingarea1:fill \""
    check_error "drawingarea1:fill nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill nnn\""
    check_error "drawingarea1:fill $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill $BIG_STRING\""
    check_error "drawingarea1:fill 1 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill 1 1\""
    check_error "drawingarea1:fill_preserve" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve\""
    check_error "drawingarea1:fill_preserve " \
                "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve \""
    check_error "drawingarea1:fill_preserve nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve nnn\""
    check_error "drawingarea1:fill_preserve $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve $BIG_STRING\""
    check_error "drawingarea1:fill_preserve 1 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:fill_preserve 1 1\""
    check_error "drawingarea1:stroke" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke\""
    check_error "drawingarea1:stroke " \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke \""
    check_error "drawingarea1:stroke nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke nnn\""
    check_error "drawingarea1:stroke $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke $BIG_STRING\""
    check_error "drawingarea1:stroke 3 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke 3 3\""
    check_error "drawingarea1:stroke_preserve" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve\""
    check_error "drawingarea1:stroke_preserve " \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve \""
    check_error "drawingarea1:stroke_preserve nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve nnn\""
    check_error "drawingarea1:stroke_preserve $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve $BIG_STRING\""
    check_error "drawingarea1:stroke_preserve 3 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:stroke_preserve 3 3\""
    check_error "drawingarea1:remove" \
                "ignoring GtkDrawingArea command \"drawingarea1:remove\""
    check_error "drawingarea1:remove " \
                "ignoring GtkDrawingArea command \"drawingarea1:remove \""
    check_error "drawingarea1:remove nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:remove nnn\""
    check_error "drawingarea1:remove $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:remove $BIG_STRING\""
    check_error "drawingarea1:remove 1 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:remove 1 1\""
    check_error "drawingarea1:set_show_text" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_show_text\""
    check_error "drawingarea1:set_show_text " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_show_text \""
    check_error "drawingarea1:set_show_text nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_show_text nnn\""
    check_error "drawingarea1:set_show_text $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_show_text $BIG_STRING\""
    check_error "drawingarea1:set_font_face" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face\""
    check_error "drawingarea1:set_font_face " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face \""
    check_error "drawingarea1:set_font_face nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face nnn\""
    check_error "drawingarea1:set_font_face $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face $BIG_STRING\""
    check_error "drawingarea1:set_font_face 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1\""
    check_error "drawingarea1:set_font_face 1 normal" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 normal\""
    check_error "drawingarea1:set_font_size" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size\""
    check_error "drawingarea1:set_font_size " \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size \""
    check_error "drawingarea1:set_font_size nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size nnn\""
    check_error "drawingarea1:set_font_size $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size $BIG_STRING\""
    check_error "drawingarea1:set_font_size 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size 1\""
    check_error "drawingarea1:set_font_size 1 nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size 1 nnn\""
    check_error "drawingarea1:set_font_size 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_size 1 10 10\""
    check_error "drawingarea1:set_font_face" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face\""
    check_error "drawingarea1:set_font_face 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1\""
    check_error "drawingarea1:set_font_face 1 normal" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 normal\""
    check_error "drawingarea1:set_font_face 1 normal nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 normal nnn\""
    check_error "drawingarea1:set_font_face 1 nnn normal" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 nnn normal\""
    check_error "drawingarea1:set_font_face 1 normal $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 normal $BIG_STRING\""
    check_error "drawingarea1:set_font_face 1 $BIG_STRING normal" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face 1 $BIG_STRING normal\""
    check_error "drawingarea1:set_font_face x normal normal" \
                "ignoring GtkDrawingArea command \"drawingarea1:set_font_face x normal normal\""
    check_error "drawingarea1:rel_move_for" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for\""
    check_error "drawingarea1:rel_move_for " \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for \""
    check_error "drawingarea1:rel_move_for 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for 1\""
    check_error "drawingarea1:rel_move_for 1 nnn Text" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for 1 nnn Text\""
    check_error "drawingarea1:rel_move_for 1 $BIG_STRING Text" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for 1 $BIG_STRING Text\""
    check_error "drawingarea1:rel_move_for nnn c Text" \
                "ignoring GtkDrawingArea command \"drawingarea1:rel_move_for nnn c Text\""
    check_error "drawingarea1:transform" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform\""
    check_error "drawingarea1:transform " \
                "ignoring GtkDrawingArea command \"drawingarea1:transform \""
    check_error "drawingarea1:transform nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform nnn\""
    check_error "drawingarea1:transform $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform $BIG_STRING\""
    check_error "drawingarea1:transform 1 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2\""
    check_error "drawingarea1:transform 1 2 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3\""
    check_error "drawingarea1:transform 1 2 3 4" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3 4\""
    check_error "drawingarea1:transform 1 2 3 4 5" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3 4 5\""
    check_error "drawingarea1:transform 1 2 3 4 5 6" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3 4 5 6\""
    check_error "drawingarea1:transform 1 2 3 x 5 6 7" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3 x 5 6 7\""
    check_error "drawingarea1:transform 1 2 3 4 5 6 7 8" \
                "ignoring GtkDrawingArea command \"drawingarea1:transform 1 2 3 4 5 6 7 8\""
    check_error "drawingarea1:translate" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate\""
    check_error "drawingarea1:translate " \
                "ignoring GtkDrawingArea command \"drawingarea1:translate \""
    check_error "drawingarea1:translate nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate nnn\""
    check_error "drawingarea1:translate $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate $BIG_STRING\""
    check_error "drawingarea1:translate 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate 1\""
    check_error "drawingarea1:translate 1 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate 1 2\""
    check_error "drawingarea1:translate nnn 2 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate nnn 2 3\""
    check_error "drawingarea1:translate 1 x 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate 1 x 3\""
    check_error "drawingarea1:translate 1 2 3 4" \
                "ignoring GtkDrawingArea command \"drawingarea1:translate 1 2 3 4\""
    check_error "drawingarea1:scale" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale\""
    check_error "drawingarea1:scale " \
                "ignoring GtkDrawingArea command \"drawingarea1:scale \""
    check_error "drawingarea1:scale nnn" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale nnn\""
    check_error "drawingarea1:scale $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale $BIG_STRING\""
    check_error "drawingarea1:scale 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale 1\""
    check_error "drawingarea1:scale 1 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale 1 2\""
    check_error "drawingarea1:scale nnn 2 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale nnn 2 3\""
    check_error "drawingarea1:scale 1 x 3" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale 1 x 3\""
    check_error "drawingarea1:scale 1 2 3 4" \
                "ignoring GtkDrawingArea command \"drawingarea1:scale 1 2 3 4\""
    check_error "drawingarea1:rotate" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate\""
    check_error "drawingarea1:rotate " \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate \""
    check_error "drawingarea1:rotate 1" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate 1\""
    check_error "drawingarea1:rotate nnn 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate nnn 2\""
    check_error "drawingarea1:rotate $BIG_STRING 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate $BIG_STRING 2\""
    check_error "drawingarea1:rotate 1 x" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate 1 x\""
    check_error "drawingarea1:rotate 1 10 10" \
                "ignoring GtkDrawingArea command \"drawingarea1:rotate 1 10 10\""
    check_error "drawingarea1:snapshot" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot\""
    check_error "drawingarea1:snapshot " \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot \""
    check_error "drawingarea1:snapshot x" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot x\""
    check_error "drawingarea1:snapshot $BIG_STRING" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot $BIG_STRING\""
    check_error "drawingarea1:snapshot x.yz" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot x.yz\""
    check_error "drawingarea1:snapshot x.pdf 2" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot x.pdf 2\""
    check_error "drawingarea1:snapshot xsvg" \
                "ignoring GtkDrawingArea command \"drawingarea1:snapshot xsvg\""
fi

echo "-:main_quit" >$FIN
check_rm $FIN


if test $AUTOMATIC; then

    ## Logging to stderr
    read r 2< $FERR &
    ./pipeglade -i $FIN 2> $FERR -l - &
    # wait for $FIN to appear
    while test ! \( -e $FIN -a -e $FERR \); do :; done

    check_error "# Comment" \
                "##########	##### (New Pipeglade session) #####"
    check_error "" \
                "### (Idle) ###"
    check_error "-:main_quit" \
                "	# Comment"
    check_rm $FIN
    rm $FERR
fi


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

# check nr_of_feedback_msgs user_instruction command expected_feedback1 expected_feedback2 ...
check() {
    while test ! \( -e $FOUT \); do :; done
    # Flush stale pipeglade output
    while read -t .1 <$FOUT; do : ; done
    N=$1
    INSTRUCTION="$2"
    echo "$SEND ${3:0:300}"
    if test "$INSTRUCTION"; then
        echo -e "statusbar1:push_id =check= $INSTRUCTION" >$FIN
    fi
    while test ! \( -e $FIN \); do :; done
    echo -e "$3" >$FIN
    i=0
    while (( i<$N )); do
        read r <$FOUT
        if test "$r" != ""; then
            if grep -qFe "${4:0:300}" <<< ${r:0:300}; then
                count_ok
                echo " $OK  ($i)  ${r:0:300}"
            else
                count_fail
                echo " $FAIL($i)  ${r:0:300}"
                echo " $EXPECTED ${4:0:300}"
            fi
            shift
            (( i+=1 ))
        fi
    done
    if test "$INSTRUCTION"; then
        echo -e "statusbar1:pop_id =check=" >$FIN
    fi
}


if test $AUTOMATIC; then

    # Being impervious to locale
    LC_ALL=de_DE.UTF-8 ./pipeglade -i $FIN -o $FOUT -O $ERR_FILE -b >/dev/null
    check 0 "" \
          "drawingarea1:transform 1 1.5 1 1 1 1 1"
    check 0 "" \
          "progressbar1:set_fraction .5"
    check 1 "" \
          "scale1:set_value .5" \
          "scale1:value 0.50"
    sleep .5
    check 0 "" \
          "_:main_quit"
    sleep .5
    check_cmd "(! grep -qe \"ignoring GtkDrawingArea command\" $ERR_FILE)"
    check_cmd "(! grep -qe \"ignoring GtkProgressBar command\" $ERR_FILE)"
    check_cmd "(! grep -qe \"ignoring GtkScale command\" $ERR_FILE)"
    check_rm $FIN
    check_rm $FOUT
    rm $ERR_FILE

    # Logging to stderr while redirecting stderr
    ./pipeglade -i $FIN -o $FOUT -O $ERR_FILE -l - -b >/dev/null
    check 0 "" \
          "# Comment"
    check 0 "" \
          "BlahBlah"
    check 0 "" \
          "_:main_quit"

    check_cmd "grep -qe \"# Comment\" $ERR_FILE"
    check_cmd "grep -qe \"ignoring command\" $ERR_FILE"
    check_rm $FIN


    # check if stdout remains line buffered even if directed to file
    ./pipeglade -i $FIN >$OUT_FILE &
    # wait for $FIN and $OUT_FILE to appear
    while test ! \( -e $FIN -a -e $OUT_FILE \); do :; done
    echo "button1:force" >$FIN
    check_cmd "grep -qe 'button1:clicked' $OUT_FILE"
    echo "_:main_quit" >$FIN
    rm -f $OUT_FILE


    ./pipeglade -i $FIN -o $FOUT -b >$PID_FILE
    check_cmd "kill `cat $PID_FILE /dev/null` 2&>/dev/null"
    rm $FIN $FOUT


    ./pipeglade --display ${DISPLAY-:0} -i $FIN -o $FOUT -b >/dev/null
    check 0 "" \
          "# checking --display $DISPLAY\n _:main_quit"
    check_rm $FIN
    check_rm $FOUT


    ./pipeglade -u simple_dialog.ui -i $FIN -o $FOUT -b >$PID_FILE
    check_cmd "ps -p `cat $PID_FILE` >/dev/null"
    check 1 "" \
          "main_apply:force" \
          "main_apply:clicked"
    check 0 "" \
          "main_cancel:force"
    check_rm $FIN
    check_rm $FOUT
    check_cmd "! ps -p `cat $PID_FILE` >/dev/null"
    rm $PID_FILE


    ./pipeglade -u simple_dialog.ui -i $FIN -o $FOUT -b >/dev/null
    check 1 "" \
          "button1:force" \
          "button1:clicked"
    check 1 "" \
          "main_ok:force" \
          "main_ok:clicked"
    check_rm $FIN
    check_rm $FOUT


    ./pipeglade -u simple_open.ui -i $FIN -o $FOUT >/dev/null &
    # wait for $FIN and $OUT_FILE to appear
    while test ! \( -e $FIN -a -e $FOUT \); do :; done
    check 3 "" \
          "main_apply:force" \
          "main_apply:clicked" \
          "main:file" \
          "main:folder"
    check 0 "" \
          "main_cancel:force"
    check_rm $FIN
    check_rm $FOUT


    ./pipeglade -u simple_open.ui -i $FIN -o $FOUT >/dev/null &
    # wait for $FIN and $OUT_FILE to appear
    while test ! \( -e $FIN -a -e $FOUT \); do :; done
    check 2 "" \
          "main_ok:force" \
          "main_ok:clicked" \
          "main:file"
    check_rm $FIN
    check_rm $FOUT


    mkfifo -m 777 $FIN
    mkfifo -m 777 $FOUT
    touch $ERR_FILE $LOG
    chmod 777 $ERR_FILE $LOG
    ./pipeglade -i $FIN -o $FOUT -O $ERR_FILE -l $LOG -b >/dev/null
    check_cmd "$STAT_CMD $FIN | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $FOUT | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $ERR_FILE | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $LOG | grep -q '600$' 2>/dev/null"
    echo -e "_:main_quit" > $FIN
    check_rm $FIN
    check_rm $FOUT
    rm -f $ERR_FILE $LOG


    ./pipeglade -i $FIN -o $FOUT -O $ERR_FILE -l $LOG -b >/dev/null
    check_cmd "$STAT_CMD $FIN | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $FOUT | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $ERR_FILE | grep -q '600$' 2>/dev/null"
    check_cmd "$STAT_CMD $LOG | grep -q '600$' 2>/dev/null"
    echo -e "_:main_quit" > $FIN
    check_rm $FIN
    check_rm $FOUT
    rm -f $ERR_FILE $LOG

    ./pipeglade -u clock.ui -i $FIN -o $FOUT -b
    check 0 "" \
          "main:resize 500 600\n main:move 100 100"
    check_cmd  'test $(xdotool search --name pipeglade-clock getwindowgeometry --shell | grep -c -e "X=...$" -e "Y=...$" -e "WIDTH=500" -e "HEIGHT=600") -eq 4'
    check 0 "" \
          "main:resize\n main:move 0 0"
    check_cmd  'test $(xdotool search --name pipeglade-clock getwindowgeometry --shell | grep -c -e "X=.$" -e "Y=.$" -e "WIDTH=440" -e "HEIGHT=440") -eq 4'
    check 0 "" \
          "main:fullscreen"
    check_cmd  'test $(xdotool search --name pipeglade-clock getwindowgeometry --shell | grep -c -e "WIDTH=..." -e "HEIGHT=...") -eq 2'
    check 0 "" \
          "main:unfullscreen"
    check_cmd  'test $(xdotool search --name pipeglade-clock getwindowgeometry --shell | grep -c -e "WIDTH=440" -e "HEIGHT=440") -eq 2'
    check 0 "" \
          "main:set_title Another Title!"
    check_cmd  'xdotool search --name "Another Title!" getwindowname | grep -qe "Another Title!"'
    check 0 "" \
          "_:main_quit"

fi

echo "####	# Initial line to check if -l option appends" >$LOG
LC_NUMERIC=de_DE.UTF-8 ./pipeglade -i $FIN -o $FOUT -l $LOG -b >/dev/null


if test $AUTOMATIC; then

    check 0 "" \
          "socket1:id"
    read XID <$FOUT
    XID=${XID/socket1:id }
    (sleep .5; ./pipeglade -u simple_dialog.ui -e $XID <<< "main_cancel:force") >/dev/null &
    check 2 "" \
          "" \
          "socket1:plug-added" \
          "socket1:plug-removed"
    (sleep .5; ./pipeglade -u simple_dialog.ui -e $XID <<< "main_cancel:force") >/dev/null &
    check 2 "" \
          "" \
          "socket1:plug-added" \
          "socket1:plug-removed"

    check 1 "" \
          "entry1:set_text FFFF" \
          "entry1:text FFFF"
    check 1 "" \
          "entry1:block 1\n entry1:set_text XXXXX\n entry1:block 0\n entry1:set_text EEEE" \
          "entry1:text EEEE"
    check 1 "" \
          "entry1:set_text $BIG_STRING" \
          "entry1:text $BIG_STRING"
    check 1 "" \
          "entry1:set_text" \
          "entry1:text"
    check 1 "" \
          "entry1:set_text FFFF" \
          "entry1:text FFFF"
    check 1 "" \
          "entry1:set_text " \
          "entry1:text"
    check 0 "" \
          "entry1:set_placeholder_text hint hint" # not much of a test
    check 1 "" \
          "entry1:set_text FFFF" \
          "entry1:text FFFF"
    check 1 "" \
          "entry1:set_text GGGG" \
          "entry1:text GGGG"
    check 1 "" \
          "entry1:force" \
          "entry1:text GGGG"
    check 1 "" \
          "spinbutton1:block 1\n spinbutton1:set_text 29.0\n spinbutton1:block 0\n spinbutton1:set_text 28.0\n" \
          "spinbutton1:text 28.0"
    check 2 "" \
          "spinbutton1:set_text 33.0\n spinbutton1:set_range 50 60\n" \
          "spinbutton1:text 33.0" \
          "spinbutton1:text 50.0"
    check 1 "" \
          "radiobutton2:block 1\n radiobutton2:set_active 1\n radiobutton2:block 0" \
          "radiobutton1:0"
    check 2 "" \
          "radiobutton1:set_active 1" \
          "radiobutton2:0" \
          "radiobutton1:1"
    check 1 "" \
          "switch1:set_active 1\n switch1:block 1\n switch1:set_active 0" \
          "switch1:1"
    check 1 "" \
          "switch1:set_active 1\n switch1:block 0\n switch1:set_active 0" \
          "switch1:0"
    check 0 "" \
          "progressbar1:set_text $BIG_STRING"
    check 0 "" \
          "progressbar1:set_text This Is A Progressbar."
    check 3 "" \
          "label1:ping\n button1:ping\n main:ping" \
          "label1:ping" \
          "button1:ping" \
          "main:ping"

fi

check 1 "" \
      "togglebutton1:set_active 1" \
      "togglebutton1:1"
check 1 "" \
      "togglebutton1:block 1\n togglebutton1:set_active 0\n togglebutton1:block 0\n togglebutton1:set_active 1" \
      "togglebutton1:1"
check 1 "" \
      "calendar1:block 1\n calendar1:select_date 1752-05-17\n calendar1:block 0\n calendar1:select_date 1752-05-18" \
      "calendar1:clicked 1752-05-18"
check 1 "" \
      "calendar1:select_date 1752-03-29" \
      "calendar1:clicked 1752-03-29"

if test $INTERACTIVE; then
    check 1 "Open what should now be named \"EXPANDER\" and click the \"button inside expander\"" \
          "expander1:set_expanded 0\n expander1:set_label EXPANDER" \
          "button6:clicked"
    check 0 "" \
          "expander1:set_expanded 0"
fi

check 12 "" \
      "treeview2:set_visible 0\n treeview1:set 2 0 1\n treeview1:set 2 1 -30000\n treeview1:set 2 2 66\n treeview1:set 2 3 -2000000000\n treeview1:set 2 4 4000000000\n treeview1:set 2 5 -2000000000\n treeview1:set 2 6 4000000000\n treeview1:set 2 7 3.141\n treeview1:set 2 8 3.141\n treeview1:set 2 9 TEXT\n treeview1:set_cursor 2" \
      "treeview1:clicked" \
      "treeview1:gboolean 2 0 1" \
      "treeview1:gint 2 1 -30000" \
      "treeview1:guint 2 2 66" \
      "treeview1:glong 2 3 -2000000000" \
      "treeview1:glong 2 4 4000000000" \
      "treeview1:glong 2 5 -2000000000" \
      "treeview1:gulong 2 6 4000000000" \
      "treeview1:gfloat 2 7 3.141000" \
      "treeview1:gdouble 2 8 3.141000" \
      "treeview1:gchararray 2 9 TEXT" \
      "treeview1:gchararray 2 10 cyan"
mkdir -p $DIR
check 0 "" \
      "treeview1:save $DIR/$FILE1"
check 0 "" \
      "treeview1:save $DIR/$FILE1.bak"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:insert_row 0\n treeview1:insert_row 2\n treeview1:set_cursor 4" \
      "treeview1:clicked" \
      "treeview1:gboolean 4 0 1" \
      "treeview1:gint 4 1 -30000" \
      "treeview1:guint 4 2 66" \
      "treeview1:glong 4 3 -2000000000" \
      "treeview1:glong 4 4 4000000000" \
      "treeview1:glong 4 5 -2000000000" \
      "treeview1:gulong 4 6 4000000000" \
      "treeview1:gfloat 4 7 3.141000" \
      "treeview1:gdouble 4 8 3.141000" \
      "treeview1:gchararray 4 9 TEXT" \
      "treeview1:gchararray 4 10 cyan"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:move_row 4 0\n treeview1:set_cursor 0" \
      "treeview1:clicked" \
      "treeview1:gboolean 0 0 1" \
      "treeview1:gint 0 1 -30000" \
      "treeview1:guint 0 2 66" \
      "treeview1:glong 0 3 -2000000000" \
      "treeview1:glong 0 4 4000000000" \
      "treeview1:glong 0 5 -2000000000" \
      "treeview1:gulong 0 6 4000000000" \
      "treeview1:gfloat 0 7 3.141000" \
      "treeview1:gdouble 0 8 3.141000" \
      "treeview1:gchararray 0 9 TEXT" \
      "treeview1:gchararray 0 10 cyan"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:move_row 0 2\n treeview1:set_cursor 1" \
      "treeview1:clicked" \
      "treeview1:gboolean 1 0 1" \
      "treeview1:gint 1 1 -30000" \
      "treeview1:guint 1 2 66" \
      "treeview1:glong 1 3 -2000000000" \
      "treeview1:glong 1 4 4000000000" \
      "treeview1:glong 1 5 -2000000000" \
      "treeview1:gulong 1 6 4000000000" \
      "treeview1:gfloat 1 7 3.141000" \
      "treeview1:gdouble 1 8 3.141000" \
      "treeview1:gchararray 1 9 TEXT" \
      "treeview1:gchararray 1 10 cyan"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:insert_row end\n treeview1:move_row 1 end\n treeview1:set_cursor 6" \
      "treeview1:clicked" \
      "treeview1:gboolean 6 0 1" \
      "treeview1:gint 6 1 -30000" \
      "treeview1:guint 6 2 66" \
      "treeview1:glong 6 3 -2000000000" \
      "treeview1:glong 6 4 4000000000" \
      "treeview1:glong 6 5 -2000000000" \
      "treeview1:gulong 6 6 4000000000" \
      "treeview1:gfloat 6 7 3.141000" \
      "treeview1:gdouble 6 8 3.141000" \
      "treeview1:gchararray 6 9 TEXT" \
      "treeview1:gchararray 6 10 cyan"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:remove_row 0\n treeview1:remove_row 2\n treeview1:set_cursor 4" \
      "treeview1:clicked" \
      "treeview1:gboolean 4 0 1" \
      "treeview1:gint 4 1 -30000" \
      "treeview1:guint 4 2 66" \
      "treeview1:glong 4 3 -2000000000" \
      "treeview1:glong 4 4 4000000000" \
      "treeview1:glong 4 5 -2000000000" \
      "treeview1:gulong 4 6 4000000000" \
      "treeview1:gfloat 4 7 3.141000" \
      "treeview1:gdouble 4 8 3.141000" \
      "treeview1:gchararray 4 9 TEXT" \
      "treeview1:gchararray 4 10 cyan"
check 1 "" \
      "treeview1:set_cursor" \
      "treeview1:clicked"
check 12 "" \
      "treeview1:move_row 0 end\n treeview1:set_cursor 3" \
      "treeview1:clicked" \
      "treeview1:gboolean 3 0 1" \
      "treeview1:gint 3 1 -30000" \
      "treeview1:guint 3 2 66" \
      "treeview1:glong 3 3 -2000000000" \
      "treeview1:glong 3 4 4000000000" \
      "treeview1:glong 3 5 -2000000000" \
      "treeview1:gulong 3 6 4000000000" \
      "treeview1:gfloat 3 7 3.141000" \
      "treeview1:gdouble 3 8 3.141000" \
      "treeview1:gchararray 3 9 TEXT" \
      "treeview1:gchararray 3 10 cyan"
check 24 "" \
      "treeview1:remove_row 3" \
      "treeview1:clicked" \
      "treeview1:gboolean 3 0 0" \
      "treeview1:gint 3 1 0" \
      "treeview1:guint 3 2 0" \
      "treeview1:glong 3 3 0" \
      "treeview1:glong 3 4 0" \
      "treeview1:glong 3 5 0" \
      "treeview1:gulong 3 6 0" \
      "treeview1:gfloat 3 7 0.000000" \
      "treeview1:gdouble 3 8 0.000000" \
      "treeview1:gchararray 3 9 abc" \
      "treeview1:gchararray 3 10 magenta" \
      "treeview1:clicked" \
      "treeview1:gboolean 3 0 0" \
      "treeview1:gint 3 1 0" \
      "treeview1:guint 3 2 0" \
      "treeview1:glong 3 3 0" \
      "treeview1:glong 3 4 0" \
      "treeview1:glong 3 5 0" \
      "treeview1:gulong 3 6 0" \
      "treeview1:gfloat 3 7 0.000000" \
      "treeview1:gdouble 3 8 0.000000" \
      "treeview1:gchararray 3 9 abc" \
      "treeview1:gchararray 3 10 magenta"

if test $INTERACTIVE; then
    check 1 "Click column col4 in the lowest line visible in the scrolled area and type 444 <Enter> (scroll)" \
          "treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:insert_row 2\n treeview1:scroll 24 0" \
          "treeview1:gint 24 1 444"
    check 12 "Click column col3 in the highest line visible in the scrolled area (scroll)" \
          "treeview1:scroll 1 0" \
          "treeview1:clicked" \
          "treeview1:gboolean 1 0 1" \
          "treeview1:gint 1 1 3" \
          "treeview1:guint 1 2 0" \
          "treeview1:glong 1 3 0" \
          "treeview1:glong 1 4 0" \
          "treeview1:glong 1 5 0" \
          "treeview1:gulong 1 6 0" \
          "treeview1:gfloat 1 7 0.000000" \
          "treeview1:gdouble 1 8 0.000000" \
          "treeview1:gchararray 1 9 jkl" \
          "treeview1:gchararray 1 10 green"

    check 1 "Click the header of column \"col3\"" \
          "" \
          "treeviewcolumn3:clicked"
fi

check 2 "" \
      "treeview1:clear\n button1:force" \
      "treeview1:clicked" \
      "button1:clicked"

check 12 "" \
      "treeview2:set_visible 1\n treeview2:insert_row end\n treeview2:insert_row 0 as_child\n treeview2:insert_row 0:0 as_child\n treeview2:insert_row 0:0\n treeview2:set 100:1:0 0 1\n treeview2:set 100:1:0 1 -30000\n treeview2:set 100:1:0 2 33\n treeview2:set 100:1:0 3 -2000000000\n treeview2:set 100:1:0 4 4000000000\n treeview2:set 100:1:0 5 -2000000000\n treeview2:set 100:1:0 6 4000000000\n treeview2:set 100:1:0 7 3.141\n treeview2:set 100:1:0 8 3.141\n treeview2:set 100:1:0 9 TEXT\n treeview2:expand_all\n treeview2:set_cursor 100:1:0" \
      "treeview2:clicked" \
      "treeview2:gboolean 100:1:0 0 1" \
      "treeview2:gint 100:1:0 1 -30000" \
      "treeview2:guint 100:1:0 2 33" \
      "treeview2:glong 100:1:0 3 -2000000000" \
      "treeview2:glong 100:1:0 4 4000000000" \
      "treeview2:glong 100:1:0 5 -2000000000" \
      "treeview2:gulong 100:1:0 6 4000000000" \
      "treeview2:gfloat 100:1:0 7 3.141000" \
      "treeview2:gdouble 100:1:0 8 3.141000" \
      "treeview2:gchararray 100:1:0 9 TEXT" \
      "treeview2:gchararray 100:1:0 10"
check 1 "" \
      "treeview2:set_cursor" \
      "treeview2:clicked"
check 12 "" \
      "treeview2:insert_row 0\n treeview2:insert_row 0\n treeview2:set 102:1 3 876543210\n treeview2:set 102 3 448822\n treeview2:collapse\n treeview2:set_cursor 102" \
      "treeview2:clicked" \
      "treeview2:gboolean 102 0 0" \
      "treeview2:gint 102 1 0" \
      "treeview2:guint 102 2 0" \
      "treeview2:glong 102 3 448822" \
      "treeview2:glong 102 4 0" \
      "treeview2:glong 102 5 0" \
      "treeview2:gulong 102 6 0" \
      "treeview2:gfloat 102 7 0.000000" \
      "treeview2:gdouble 102 8 0.000000" \
      "treeview2:gchararray 102 9" \
      "treeview2:gchararray 102 10"
check 1 "" \
      "treeview2:set_cursor" \
      "treeview2:clicked"
check 0 "" \
      "treeview2:save $DIR/$FILE2"
check 0 "" \
      "treeview2:save $DIR/$FILE2.bak"
check 12 "" \
      "treeview2:insert_row 0\n treeview2:collapse\n treeview2:set_cursor 103" \
      "treeview2:clicked" \
      "treeview2:gboolean 103 0 0" \
      "treeview2:gint 103 1 0" \
      "treeview2:guint 103 2 0" \
      "treeview2:glong 103 3 448822" \
      "treeview2:glong 103 4 0" \
      "treeview2:glong 103 5 0" \
      "treeview2:gulong 103 6 0" \
      "treeview2:gfloat 103 7 0.000000" \
      "treeview2:gdouble 103 8 0.000000" \
      "treeview2:gchararray 103 9" \
      "treeview2:gchararray 103 10"
check 1 "" \
      "treeview2:set_cursor" \
      "treeview2:clicked"

if test $INTERACTIVE; then
    check 12 "Click the lowest line visible in the scrolled area (1)" \
          "treeview2:expand_all 103\n treeview2:scroll 103:1:0 0" \
          "treeview2:clicked" \
          "treeview2:gboolean 103:1:0 0 1" \
          "treeview2:gint 103:1:0 1 -30000" \
          "treeview2:guint 103:1:0 2 33" \
          "treeview2:glong 103:1:0 3 -2000000000" \
          "treeview2:glong 103:1:0 4 4000000000" \
          "treeview2:glong 103:1:0 5 -2000000000" \
          "treeview2:gulong 103:1:0 6 4000000000" \
          "treeview2:gfloat 103:1:0 7 3.141000" \
          "treeview2:gdouble 103:1:0 8 3.141000" \
          "treeview2:gchararray 103:1:0 9 TEXT" \
          "treeview2:gchararray 103:1:0 10"
    check 1 "" \
          "treeview2:set_cursor" \
          "treeview2:clicked"
    check 12 "Click the lowest visible line (2)" \
          "treeview2:collapse\n treeview2:expand 103\n treeview2:scroll 103:1 0" \
          "treeview2:clicked" \
          "treeview2:gboolean 103:1 0 0" \
          "treeview2:gint 103:1 1 0" \
          "treeview2:guint 103:1 2 0" \
          "treeview2:glong 103:1 3 876543210" \
          "treeview2:glong 103:1 4 0" \
          "treeview2:glong 103:1 5 0" \
          "treeview2:gulong 103:1 6 0" \
          "treeview2:gfloat 103:1 7 0.000000" \
          "treeview2:gdouble 103:1 8 0.000000" \
          "treeview2:gchararray 103:1 9" \
          "treeview2:gchararray 103:1 10"
    check 1 "" \
          "treeview2:set_cursor" \
          "treeview2:clicked"
    check 12 "Click the lowest visible line (3)" \
          "treeview2:collapse\n treeview2:expand_all\n treeview2:scroll 103:1:0 0" \
          "treeview2:clicked" \
          "treeview2:gboolean 103:1:0 0 1" \
          "treeview2:gint 103:1:0 1 -30000" \
          "treeview2:guint 103:1:0 2 33" \
          "treeview2:glong 103:1:0 3 -2000000000" \
          "treeview2:glong 103:1:0 4 4000000000" \
          "treeview2:glong 103:1:0 5 -2000000000" \
          "treeview2:gulong 103:1:0 6 4000000000" \
          "treeview2:gfloat 103:1:0 7 3.141000" \
          "treeview2:gdouble 103:1:0 8 3.141000" \
          "treeview2:gchararray 103:1:0 9 TEXT" \
          "treeview2:gchararray 103:1:0 10"
    check 1 "" \
          "treeview2:set_cursor" \
          "treeview2:clicked"
    check 12 "Click the lowest visible line (4)" \
          "treeview2:expand_all\n treeview2:collapse 103:1\n treeview2:scroll 103:1 0" \
          "treeview2:clicked" \
          "treeview2:gboolean 103:1 0 0" \
          "treeview2:gint 103:1 1 0" \
          "treeview2:guint 103:1 2 0" \
          "treeview2:glong 103:1 3 876543210" \
          "treeview2:glong 103:1 4 0" \
          "treeview2:glong 103:1 5 0" \
          "treeview2:gulong 103:1 6 0" \
          "treeview2:gfloat 103:1 7 0.000000" \
          "treeview2:gdouble 103:1 8 0.000000" \
          "treeview2:gchararray 103:1 9" \
          "treeview2:gchararray 103:1 10"
    check 1 "" \
          "treeview2:set_cursor" \
          "treeview2:clicked"

    check 1 "Click the header of column \"col23\"" \
          "" \
          "treeviewcolumn23:clicked"

fi

if test $AUTOMATIC; then

    check 12 "" \
          "treeview1:clear\n treeview1:set 1 9 $BIG_STRING\n treeview1:set_cursor 1" \
          "treeview1:clicked" \
          "treeview1:gboolean 1 0 0" \
          "treeview1:gint 1 1 0" \
          "treeview1:guint 1 2 0" \
          "treeview1:glong 1 3 0" \
          "treeview1:glong 1 4 0" \
          "treeview1:glong 1 5 0" \
          "treeview1:gulong 1 6 0" \
          "treeview1:gfloat 1 7 0.000000" \
          "treeview1:gdouble 1 8 0.000000" \
          "treeview1:gchararray 1 9 $BIG_STRING" \
          "treeview1:gchararray 1 10"
    check 12 "" \
          "treeview1:clear\n treeview1:set 1 9 ABC\\\\nDEF\\\\nGHI\n treeview1:set_cursor 1" \
          "treeview1:clicked" \
          "treeview1:clicked" \
          "treeview1:gboolean 1 0 0" \
          "treeview1:gint 1 1 0" \
          "treeview1:guint 1 2 0" \
          "treeview1:glong 1 3 0" \
          "treeview1:glong 1 4 0" \
          "treeview1:glong 1 5 0" \
          "treeview1:gulong 1 6 0" \
          "treeview1:gfloat 1 7 0.000000" \
          "treeview1:gdouble 1 8 0.000000" \
          "treeview1:gchararray 1 9 ABCnDEFnGHI" \
          "treeview1:gchararray 1 10"
    check 0 "" \
          "treeview1:clear\n treeview2:clear"
    check 0 "" \
          "_:load $DIR/$FILE1"
    rm -f $DIR/$FILE1
    sleep .5
    check 1 "" \
          "treeview1:save $DIR/$FILE1\n button1:force" \
          "button1:clicked"
    check_cmd "cmp $DIR/$FILE1 $DIR/$FILE1.bak"
    check 0 "" \
          "treeview1:clear\n treeview2:clear"
    check 0 "" \
          "_:load $DIR/$FILE2"
    sleep .5
    rm -f $DIR/$FILE2
    sleep .5
    check 1 "" \
          "treeview2:save $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    check_cmd "cmp $DIR/$FILE2 $DIR/$FILE2.bak"
    cat >$DIR/$FILE3 <<< "_:load $DIR/$FILE1.bak"
    cat >>$DIR/$FILE3 <<< "_:load $DIR/$FILE2.bak"
    cat >$DIR/$FILE4 <<< "_:load $DIR/$FILE3"
    cat >$DIR/$FILE5 <<< "_:load $DIR/$FILE4"
    cat >$DIR/$FILE6 <<< "_:load $DIR/$FILE5"
    rm -f $DIR/$FILE1 $DIR/$FILE2
    sleep .5
    check 0 "" \
          "treeview1:clear\n treeview2:clear"
    check 0 "" \
          "_:load $DIR/$FILE6"
    rm -f $DIR/$FILE1 $DIR/$FILE2
    sleep .5
    check 1 "" \
          "treeview1:save $DIR/$FILE1\n treeview2:save $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    check_cmd "cmp $DIR/$FILE1 $DIR/$FILE1.bak"
    check_cmd "cmp $DIR/$FILE2 $DIR/$FILE2.bak"
    rm -rf $DIR
    check 0 "" \
          "treeview1:set 100 9 XXXYYY"
    check 0 "" \
          "treeview2:clear\n treeview2:set 2 0 1"

    check 0 "" \
          "notebook1:set_current_page 2"
    check 1 "" \
          "nonexistent_send_text:force" \
          "nonexistent_send_text:clicked"
    check 1 "" \
          "nonexistent_send_selection:force" \
          "nonexistent_send_selection:clicked"
    check 1 "" \
          "nonexistent_ok:force" \
          "nonexistent_ok:clicked"
    check 1 "" \
          "nonexistent_apply:force" \
          "nonexistent_apply:clicked"
    check 1 "" \
          "nonexistent_cancel:force" \
          "nonexistent_cancel:clicked"
    check 0 "" \
          "notebook1:set_current_page 1"
    check 1 "" \
          "textview1_send_text:force" \
          "textview1_send_text:text some textnetcn"
    check 1 "" \
          "textview1:place_cursor 5\n textview1:insert_at_cursor MORE \n textview1_send_text:force" \
          "textview1_send_text:text some MORE textnetcn"
    check 1 "" \
          "textview1:place_cursor_at_line 1\n textview1:insert_at_cursor ETC \n textview1_send_text:force" \
          "textview1_send_text:text some MORE textnETC etcn"
    mkdir -p $DIR
    check 1 "" \
          "textview1:save $DIR/$FILE1\n button1:force" \
          "button1:clicked"
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
    check 0 "" \
          "_:load $DIR/$FILE2"
    check 0 "" \
          "textview1:save $DIR/$FILE1"
    check 0 "" \
          "textview1:save $DIR/$FILE1"
    check 0 "" \
          "textview1:delete"
    check 0 "" \
          "textview2:delete"
    check 0 "" \
          "_:load $DIR/$FILE3"
    check 0 "" \
          "_:load $DIR/$FILE1"
    check 0 "" \
          "textview2:save $DIR/$FILE3"
    check 1 "" \
          "textview1:save $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    check_cmd "cmp $DIR/$FILE1 $DIR/$FILE2"
    echo "textview1:insert_at_cursor I'm a text containing backslashes:\\nONE\\\\\nTWO\\\\\\\\\\nTHREE\\\\\\\\\\\\\\nEnd" > $DIR/$FILE1
    check 0 "" \
          "textview1:delete\n _:load $DIR/$FILE1"
    check 1 "" \
          "textview1:save $DIR/$FILE1\n textview1:save $DIR/$FILE2\n textview1:delete\n _:load $DIR/$FILE1\n button1:force" \
          "button1:clicked"
    rm $DIR/$FILE1
    sleep .5
    check 1 "" \
          "textview1:save $DIR/$FILE1\n button1:force" \
          "button1:clicked"
    check_cmd "test 96 = `wc -c $DIR/$FILE1 | awk '{print $1}'`"
    check_cmd "cmp $DIR/$FILE1 $DIR/$FILE2"

fi

check 1 "" \
      "textview1:delete\n textview1_send_text:force" \
      "textview1_send_text:text"

if test $INTERACTIVE; then

    check 1 "Highlight the lowest visible character and press \"send_selection\"" \
          "textview1:place_cursor_at_line 1 \ntextview1:insert_at_cursor A\\\\nB\\\\nC\\\\nD\\\\nE\\\\nF\\\\nG\\\\nH\\\\nI\\\\nJ\\\\nK\\\\nL\\\\nM\\\\nN\\\\nO\\\\nP\\\\nQ\\\\nR\\\\nS\\\\nT\\\\nU\\\\nV\\\\nW\\\\nX\\\\nY\\\\nZ\\\\na\\\\nb\\\\nc\\\\nd\\\\ne\\\\nf\\\\ng\\\\nh\\\\ni\\\\nj\\\\nk\\\\nl\\\\nm\\\\nn\\\\no\\\\np\\\\nq\\\\nr\\\\ns\\\\nt\\\\nu\\\\nv\\\\nw\\\\nx\\\\ny\\\\nz \n textview1:place_cursor_at_line 46 \n textview1:scroll_to_cursor\n notebook1:set_current_page 1" \
          "textview1_send_selection:text u"
    check 1 "Again, highlight the lowest visible character and press \"send_selection\"" \
          "textview1:place_cursor end\n textview1:scroll_to_cursor" \
          "textview1_send_selection:text z"
    check 1 "Highlight the highest visible character and press \"send_selection\"" \
          "textview1:place_cursor 0 \n textview1:scroll_to_cursor" \
          "textview1_send_selection:text A"

fi

if test $AUTOMATIC; then
    check 1 "" \
          "treeview2:set 100:10:5 2 8888888\n treeview2:save $DIR/$FILE1\n textview2:save $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    cp $DIR/$FILE1 $DIR/$FILE4
    check_cmd "cmp $DIR/$FILE2 $DIR/$FILE3"
    check 1 "" \
          "treeview2:clear\n textview2:delete\n _:load $DIR/$FILE1\n _:load $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    rm $DIR/$FILE1 $DIR/$FILE2
    sleep .5
    check 1 "" \
          "treeview2:save $DIR/$FILE1\n textview2:save $DIR/$FILE2\n button1:force" \
          "button1:clicked"
    check_cmd "cmp $DIR/$FILE1 $DIR/$FILE4"
    check_cmd "cmp $DIR/$FILE2 $DIR/$FILE3"
    # rm -rf $DIR
    sleep .5
    check 2 "" \
          "scale1:set_value 10\n scale1:set_increments 5 20\n scale1:force" \
          "scale1:value 10.000000" \
          "scale1:value 10.000000"
    check 2 "" \
          "scale1:block 1\n scale1:set_range 20 22\n scale1:set_value 21\n scale1:block 0\n scale1:set_value 10\n scale1:set_value 100" \
          "scale1:value 20.000000" \
          "scale1:value 22.000000"
    check 1 "" \
          "scale1:set_fill_level\n scale1:set_fill_level 20.5\n scale1:set_value 21.3" \
          "scale1:value 21.300000"
    check 6 "" \
          "open_dialog:set_filename q.png\n file:force\n open_dialog_invoke:force\n open_dialog_apply:force\n open_dialog_ok:force" \
          "file:active _File" \
          "open_dialog_apply:clicked" \
          "open_dialog:file $PWD/q.png" \
          "open_dialog:folder $PWD" \
          "open_dialog_ok:clicked" \
          "open_dialog:file $PWD/q.png" \
          "open_dialog:folder $PWD"
    check 2 "" \
          "file:force\n open_dialog_invoke:force\n open_dialog_cancel:force" \
          "file:active _File" \
          "open_dialog_cancel:clicked"
    check 3 "" \
          "save_as_dialog:set_current_name /somewhere/crazy_idea\n file:force\n save_as_dialog_invoke:force\n save_as_dialog_ok:force" \
          "file:active _File" \
          "save_as_dialog_ok:clicked" \
          "save_as_dialog:file /somewhere/crazy_idea" \
          "save_as_dialog:folder"
    check 1 "" \
          "nonexistent_invoke:force" \
          "nonexistent_invoke:active nonexistent"

    # _:load
    mkdir -p $DIR
    for i in $WEIRD_PATHS; do
        cat >$i <<< "entry1:set_text RsT${i}UVwX"
    done
    for i in $WEIRD_PATHS; do
        check 1 "" \
              "_:load $i" \
              "entry1:text RsT${i}UVwX"
        rm -f $i
    done

fi

if test $INTERACTIVE; then

    check 1 "Press the \"button\" which should now be renamed \"OK\"" \
          "button1:set_label OK" \
          "button1:clicked"
    check 1 "Press the \"togglebutton\" which should now be renamed \"on/off\"" \
          "togglebutton1:set_label on/off" \
          "togglebutton1:0"
    check 1 "" \
          "togglebutton1:force" \
          "togglebutton1:1"
    check 1 "Press the \"checkbutton\" which should now be renamed \"REGISTER\"" \
          "checkbutton1:set_label REGISTER" \
          "checkbutton1:1"
    check 1 "" \
          "checkbutton1:force" \
          "checkbutton1:0"
    check 2 "Press the \"radiobutton\" which should now be renamed \"RADIO\"" \
          "radiobutton2:set_label RADIO" \
          "radiobutton1:0" \
          "radiobutton2:1"
    check 2 "" \
          "radiobutton1:force" \
          "radiobutton2:0" \
          "radiobutton1:1"
    check 1 "Click the widget whose label font is now Bold Italic 20" \
          "switch1:style font:Bold Italic 20" \
          "switch1:1"
    check 1 "Click the widget whose label has turned red" \
          "switch1:style color:red" \
          "switch1:0"
    check 1 "Click the widget whose background has turned yellow" \
          "checkbutton1:style background-color:yellow" \
          "checkbutton1:1"
    check 1 "Press \"OK\" if font and colors changed in previous steps are back to normal\n switch1:style" \
          "checkbutton1:style" \
          "button1:clicked"
    check 1 "" \
          "switch1:force" \
          "switch1:1"
    check 1 "Press \"OK\" if the \"lorem ipsum dolor ...\" text inside \"frame1\" now reads \"LABEL\"" \
          "label1:set_text LABEL" \
          "button1:clicked"
    check 1 "Press \"OK\" if the label of the frame around \"LABEL\" now reads \"LOREM IPSUM\"" \
          "frame1:set_label LOREM IPSUM" \
          "button1:clicked"
    check 1 "Press \"OK\" if the green dot has turned red" \
          "image1:set_from_icon_name gtk-no" \
          "button1:clicked"
    check 1 "Press \"OK\" if the red dot has turned into a green \"Q\"" \
          "image1:set_from_file q.png" \
          "button1:clicked"
    check 1 "Select \"FIRST\" from the combobox" \
          "comboboxtext1:prepend_text FIRST" \
          "comboboxtext1_entry:text FIRST"
    check 1 "Select \"LAST\" from the combobox" \
          "comboboxtext1:append_text LAST" \
          "comboboxtext1_entry:text LAST"
    check 1 "Select \"AVERAGE\" from the combobox" \
          "comboboxtext1:insert_text 3 AVERAGE" \
          "comboboxtext1_entry:text AVERAGE"
    check 1 "Select the second entry from the combobox" \
          "comboboxtext1:remove 0" \
          "comboboxtext1_entry:text def"
    check 2 "Left-click the \"+\" of the spinbutton" \
          "spinbutton1:set_range 0 100\n spinbutton1:set_text 33" \
          "spinbutton1:text 33.00" \
          "spinbutton1:text 34.00"
    check 1 "Left-click the \"+\" of the spinbutton again" \
          "spinbutton1:set_increments 2 4" \
          "spinbutton1:text 36.00"
    check 1 "Middle-click the \"+\" of the spinbutton" \
          "" \
          "spinbutton1:text 40.00"
    check 1 "" \
          "spinbutton1:force" \
          "spinbutton1:text 40.00"
    check 1 "Using the file chooser button (now labelled \"etc\"), select \"File System\" (= \"/\")" \
          "filechooserbutton1:set_filename /etc/" \
          "filechooserbutton1:file /"
    check 1 "" \
          "filechooserbutton1:force" \
          "filechooserbutton1:file /"
    check 1 "Click \"Select\"\n fontbutton1:set_font_name Sans Bold 40" \
          "fontbutton1:force" \
          "fontbutton1:font Sans Bold 40"
    check 1 "Click \"Select\" (1)\n colorbutton1:set_color yellow" \
          "colorbutton1:force" \
          "colorbutton1:color rgb(255,255,0)"
    check 1 "Click \"Select\" (2)\n colorbutton1:set_color rgb(0,255,0)" \
          "colorbutton1:force" \
          "colorbutton1:color rgb(0,255,0)"
    check 1 "Click \"Select\" (3)\n colorbutton1:set_color #00f" \
          "colorbutton1:force" \
          "colorbutton1:color rgb(0,0,255)"
    check 1 "Click \"Select\" (4)\n colorbutton1:set_color #ffff00000000" \
          "colorbutton1:force" \
          "colorbutton1:color rgb(255,0,0)"
    check 1 "Click \"Select\" (5)\n colorbutton1:set_color rgba(0,255,0,.5)" \
          "colorbutton1:force" \
          "colorbutton1:color rgba(0,255,0,0.5)"
    check 1 "Close the dialog by hitting Escape" \
          "printdialog:print nonexistent.ps" \
          "printdialog:closed"
    check 1 "Press \"OK\" if both 1752-03-13 and 1752-03-14 are marked on the calendar" \
          "calendar1:mark_day 13\n calendar1:mark_day 14" \
          "button1:clicked"
    check 1 "Press \"OK\" if 1752-03-13 and 1752-03-14 are no longer marked on the calendar" \
          "calendar1:clear_marks" \
          "button1:clicked"
    check 3 "Hover over the calendar and do what the tooltip says" \
          "calendar1:set_tooltip_text Double-click on 1752-03-13" \
          "calendar1:clicked 1752-03-13" \
          "calendar1:clicked 1752-03-13" \
          "calendar1:doubleclicked 1752-03-13"
    check 0 "" \
          "calendar1:set_tooltip_text"
    check 1 "" \
          "calendar1:force" \
          "calendar1:clicked 1752-03-13"

fi

check 0 "" \
      "drawingarea1:transform =100 1 0 0 1 0 0"
check 0 "" \
      "drawingarea1:set_source_rgba =101 green"
check 0 "" \
      "drawingarea1:set_source_rgba =102 red"
check 0 "" \
      "drawingarea1:rectangle =1 0 0 150 150\n drawingarea1:fill 1"
check 0 "" \
      "drawingarea1:remove 1\n drawingarea1:remove 2\n drawingarea1:remove 3\n drawingarea1:remove 4"
check 0 "" \
      "drawingarea1:rectangle 1 0 0 150 150\n drawingarea1:fill 1"
check 0 "" \
      "drawingarea1:arc 1 80 80 60 30 60\n drawingarea1:set_source_rgba 1 red\n drawingarea1:stroke_preserve 1\n drawingarea1:line_to 1 80 80\n drawingarea1:fill 1"
check 0 "" \
      "drawingarea1:arc_negative 1 80 80 70 30 60\n drawingarea1:set_source_rgba 1 green\n drawingarea1:stroke_preserve 1\n drawingarea1:rel_line_to 1 -50 -50\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:curve_to 1 30 30 90 120 120 30\n drawingarea1:set_source_rgba 1 blue\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:move_to 1 160 160\n drawingarea1:rel_curve_to 1 30 30 90 120 120 30\n drawingarea1:set_source_rgba 1 orange\n drawingarea1:stroke_preserve 1"
check 0 "" \
      "drawingarea1:move_to 1 0 0\n drawingarea1:rel_move_to 1 0 155\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 10\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:move_to 1 0 160\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 20 5\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:move_to 1 0 165\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 5 20\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:move_to 1 0 170\n drawingarea1:rel_line_to 1 300 0\n drawingarea1:set_dash 1 3 3 3 3 3 15\n drawingarea1:stroke 1"
check 0 "" \
      "drawingarea1:set_dash 1"
check 0 "" \
      "drawingarea1:set_source_rgba 103 brown\n drawingarea1:set_line_width 1 15"

if test $INTERACTIVE; then
    check 1 "Press \"OK\" if the brown shape is rounded" \
          "drawingarea1:set_line_join 2 round\n drawingarea1:set_line_cap 2 round\n drawingarea1:move_to 1 160 20\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1" \
          "button1:clicked"
    check 1 "Press \"OK\" if the second brown shape is shorter and bevelled and the square is mostly blue" \
          "drawingarea1:set_line_join 3 bevel\n drawingarea1:set_line_cap 3 butt\n drawingarea1:move_to 1 160 70\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1\n drawingarea1:set_source_rgba =102 blue" \
          "button1:clicked"
    check 1 "Press \"OK\" if the third brown shape is square everything is a bit smaller" \
          "drawingarea1:set_line_join 3 miter\n drawingarea1:set_line_cap 3 square\n drawingarea1:move_to 1 160 120\n drawingarea1:rel_line_to 1 20 0\n drawingarea1:rel_line_to 1 0 20\n drawingarea1:stroke 1\n drawingarea1:set_source_rgba =101 magenta\n drawingarea1:scale =100 .7 .7" \
          "button1:clicked"
    check 1 "Press \"OK\" if the first brown shape is no longer rounded and everything rotated a bit" \
          "drawingarea1:remove 2\n drawingarea1:rotate 105<100 10" \
          "button1:clicked"
    check 1 "Press \"OK\" if all three brown shapes look the same" \
          "drawingarea1:remove 3" \
          "button1:clicked"

fi

check 0 "" \
      "drawingarea1:move_to 5 50 50\n drawingarea1:line_to 5 200 10\n drawingarea1:line_to 5 150 200\n drawingarea1:close_path 1\n drawingarea1:set_source_rgba 5 rgba(0,255,0,.2)\n drawingarea1:fill_preserve 1"
check 0 "" \
      "drawingarea1:move_to 5 10 50\n drawingarea1:set_source_rgba 5 cyan\n drawingarea1:set_font_size 5 30\n drawingarea1:show_text 5 Xyz 789\n drawingarea1:set_font_size 5 10\n drawingarea1:show_text 5 Abc 123"
check 0 "" \
      "drawingarea1:move_to 5 10 75\n drawingarea1:set_source_rgba 5 red\n drawingarea1:set_font_face 5 italic bold Courier\n drawingarea1:set_font_size 5 30\n drawingarea1:show_text 5 Xyz 789\n drawingarea1:set_font_size 5 10\n drawingarea1:show_text 5 Abc 123"
check 0 "" \
      "drawingarea1:remove 1\n drawingarea1:remove 2\n drawingarea1:remove 3\n drawingarea1:remove 4"

check 0 "" \
      "drawingarea2:rotate 55<500 5\n drawingarea2:scale 33 .7 .7\n drawingarea2:translate 77 30 30\n drawingarea2:transform 44"
check 0 "" \
      "drawingarea2:rectangle 1<500 0 0 150 150\n drawingarea2:fill 1"
check 0 "" \
      "drawingarea2:arc 1 80 80 60 30 60\n drawingarea2:set_source_rgba 1 red\n drawingarea2:stroke_preserve 1\n drawingarea2:line_to 1 80 80\n drawingarea2:fill 1"
check 0 "" \
      "drawingarea2:arc_negative 1 80 80 70 30 60\n drawingarea2:set_source_rgba 1 green\n drawingarea2:stroke_preserve 1\n drawingarea2:rel_line_to 1 -50 -50\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:curve_to 1 30 30 90 120 120 30\n drawingarea2:set_source_rgba 1 blue\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:move_to 1 160 160\n drawingarea2:rel_curve_to 1 30 30 90 120 120 30\n drawingarea2:set_source_rgba 1 orange\n drawingarea2:stroke_preserve 1"
check 0 "" \
      "drawingarea2:move_to 1 0 0\n drawingarea2:rel_move_to 1 0 155\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 10\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:move_to 1 0 160\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 20 5\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:move_to 1 0 165\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 5 20\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:move_to 1 0 170\n drawingarea2:rel_line_to 1 300 0\n drawingarea2:set_dash 1 3 3 3 3 3 15\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:set_dash 1"
check 0 "" \
      "drawingarea2:set_source_rgba 1 brown\n drawingarea2:set_line_width 1 15"
check 0 "" \
      "drawingarea2:set_line_cap 2 round\n drawingarea2:move_to 1 160 20\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:set_line_join 3 bevel\n drawingarea2:set_line_cap 3 butt\n drawingarea2:move_to 1 160 70\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:set_line_join 3 miter\n drawingarea2:set_line_cap 3 square\n drawingarea2:move_to 1 160 120\n drawingarea2:rel_line_to 1 20 0\n drawingarea2:rel_line_to 1 0 20\n drawingarea2:stroke 1"
check 0 "" \
      "drawingarea2:remove 2"
check 0 "" \
      "drawingarea2:remove 3"

if test $INTERACTIVE; then

    check 1 "Press \"OK\" if the drawing looks tilted and displaced" \
          "drawingarea2:remove 44" \
          "button1:clicked"
    check 1 "Press \"OK\" if the drawing doesn't look tilted anymore" \
          "drawingarea2:remove 55" \
          "button1:clicked"
    check 1 "Press \"OK\" if the drawing has grown a bit" \
          "drawingarea2:remove 33" \
          "button1:clicked"
    check 1 "Press \"OK\" if the drawing has moved into the NE corner" \
          "drawingarea2:remove 77" \
          "button1:clicked"

fi

check 0 "" \
      "drawingarea1:set_source_rgba 6 red\n drawingarea1:set_font_size 6 20\n drawingarea1:transform 99 .985 -.174 .174 .985 0 0"
check 0 "" \
      "drawingarea1:move_to 6 100 100\n drawingarea1:rel_move_for 6 c CENTER\n drawingarea1:show_text 6 CENTER"
check 0 "" \
      "drawingarea1:set_source_rgba 6 blue\n drawingarea1:set_font_size 6 20"
check 0 "" \
      "drawingarea1:move_to 6 100 100\n drawingarea1:rel_move_for 6 nw NORTHWEST\n drawingarea1:show_text 6 NORTHWEST"
check 0 "" \
      "drawingarea1:move_to 6 100 100\n drawingarea1:rel_move_for 6 ne NORTHEAST\n drawingarea1:show_text 6 NORTHEAST"
check 0 "" \
      "drawingarea1:move_to 6 100 100\n drawingarea1:rel_move_for 6 se SOUTHEAST\n drawingarea1:show_text 6 SOUTHEAST"
check 0 "" \
      "drawingarea1:move_to 6 100 100\n drawingarea1:rel_move_for 6 sw SOUTHWEST\n drawingarea1:show_text 6 SOUTHWEST"
check 0 "" \
      "drawingarea1:set_source_rgba 6 magenta\n drawingarea1:set_font_size 6 20"
check 0 "" \
      "drawingarea1:move_to 6 100 140\n drawingarea1:rel_move_for 6 s SOUTH\n drawingarea1:show_text 6 SOUTH"
check 0 "" \
      "drawingarea1:move_to 6 100 140\n drawingarea1:rel_move_for 6 n NORTH\n drawingarea1:show_text 6 NORTH"
check 0 "" \
      "drawingarea1:set_source_rgba 6 green\n drawingarea1:set_font_size 6 20"
check 0 "" \
      "drawingarea1:move_to 6 100 140\n drawingarea1:rel_move_for 6 e EAST\n drawingarea1:show_text 6 EAST"
check 0 "" \
      "drawingarea1:move_to 600 100 140\n drawingarea1:rel_move_for 6 w WEST\n drawingarea1:show_text 6 WEST\n drawingarea1:set_font_size 800<600 30"
check 0 "" \
      "drawingarea1:snapshot $EPS_FILE\n drawingarea1:snapshot $EPSF_FILE\n drawingarea1:snapshot $PDF_FILE\n drawingarea1:snapshot $PS_FILE\n drawingarea1:snapshot $SVG_FILE"

check_cmd "file -b $EPS_FILE | grep -qe EPS"
check_cmd "file -b $EPSF_FILE | grep -qe EPS"
check_cmd "file -b $PS_FILE | grep -qe PostScript"
check_cmd "file -b $SVG_FILE | grep -qe SVG"
check 0 "" \
      "notebook1:set_current_page 0\n image2:set_from_file $SVG_FILE"

if test $INTERACTIVE; then

    check 2 "Hit Backspace, Enter" \
          "eventbox1:grab_focus" \
          "eventbox1:key_press BackSpace" \
          "eventbox1:key_press Return"
    check 6 "Inside the DrawingArea, left-click, middle-click, right-click (Don't move the mouse while clicking)" \
          "" \
          "eventbox1:button_press 1" \
          "eventbox1:button_release 1" \
          "eventbox1:button_press 2" \
          "eventbox1:button_release 2" \
          "eventbox1:button_press 3" \
          "eventbox1:button_release 3"
    check 3 "Inside the DrawingArea and all within one second, hold the left button down, move around a bit, and release it again" \
          "" \
          "eventbox1:button_press 1" \
          "eventbox1:motion" \
          "eventbox1:motion"
    sleep 1.5
    check 1 "Hit Space" \
          "button1:grab_focus" \
          "button1:clicked"

    check 1 "Press the biggest button if there is a spinning spinner" \
          "spinner1:start\n no_button:set_size_request 400 200" \
          "no_button:clicked"
    check 1 "Press \"OK\" if the spinner has stopped" \
          "spinner1:stop" \
          "button1:clicked"
    check 1 "Press \"OK\" if the \"No\" button is back to normal size" \
          "no_button:set_size_request" \
          "button1:clicked"

    check 0 "" \
          "notebook1:set_current_page 3"
    check 1 "Click into page 4 (vscroll)" \
          "scrolledwindow8:vscroll 4500" \
          "button_sw:clicked"
    check 1 "Click into page 4 (hscroll)" \
          "scrolledwindow8:hscroll 4500" \
          "button_se:clicked"
    check 1 "Click into page 4 (hscroll_to_range, vscroll_to_range)" \
          "scrolledwindow8:hscroll_to_range 1600 2900\n scrolledwindow8:vscroll_to_range 1600 2900" \
          "button_c:clicked"

    check 1 "Press \"OK\" if there is now a \"Disconnect\" button" \
          "button2:set_visible 1\n button2:set_sensitive 0" \
          "button1:clicked"
    check 1 "Press \"Disconnect\"" \
          "button2:set_sensitive 1" \
          "button2:clicked"

    check 1 "Press \"BIG BUTTON\" inside the window titled \"PRESS ME\"" \
          "dialog1:set_title PRESS ME\n dialog1:set_visible 1\n dialog1:resize 800 800\n dialog1:move 50 50" \
          "button3:clicked"
    check 1 "" \
          "button3:set_label PRESS THIS GIANT BUTTON NOW\n dialog1:fullscreen" \
          "button3:clicked"
    check 1 "" \
          "button3:set_label Hit Escape to close this window\n button3:set_sensitive 0" \
          "dialog1:closed"
    check 0 "" \
          "dialog1:set_visible 0"

    check 1 "Press \"OK\" if the progress bar shows 90%" \
          "progressbar1:set_fraction .9\n progressbar1:set_text" \
          "button1:clicked"
    check 1 "Press \"OK\" if the progress bar text reads \"The End\"" \
          "progressbar1:set_text The End" \
          "button1:clicked"
    check 1 "" \
          "statusbar1:push_id Id100 Press \"No\"\n statusbar1:push_id ABC nonsense #1\n statusbar1:push_id DEF nonsense #2.1\n statusbar1:push_id DEF nonsense 2.2\n statusbar1:pop\n statusbar1:pop\n statusbar1:pop_id 1\n statusbar1:pop_id ABC\n statusbar1:pop_id DEF\n statusbar1:pop_id DEF\n statusbar1:push_id GHI nonsense 3.1\n statusbar1:push_id GHI nonsense 3.2\n statusbar1:remove_all_id GHI" \
          "no_button:clicked"

fi

check 0 "" \
      "statusbar1:remove_all_id ZZZ"


echo "_:main_quit" >$FIN
check_rm $FIN
check_rm $FOUT


check_cmd "head -n 2 $LOG | tail -n 1 | grep -q '##### (New Pipeglade session) #####'"
check_cmd "tail -n 4 $LOG | head -n 1 | grep -q '### (Idle) ###'"
check_cmd "tail -n 3 $LOG | head -n 1 | grep -q 'statusbar1:remove_all_id ZZZ'"
check_cmd "tail -n 2 $LOG | head -n 1 | grep -q '### (Idle) ###'"
# check_cmd "tail -n 1 $LOG | grep '_:main_quit'"

if test $AUTOMATIC; then
    unset -v BIG_STRING
    unset -v BIG_NUM
    unset -v WEIRD_PATH
    for i in {1..10}; do
        echo "treeview1:set $i 3 $RANDOM"
        echo "treeview1:set $i 8 $RANDOM"
        echo "treeview1:set $i 9 row $i"
    done > $DIR/$FILE4
    for i in {1..10}; do
        echo "treeview2:set $i 3 $RANDOM"
        echo "treeview2:set $i 8 $RANDOM"
        echo "treeview2:set $i 9 row $i"
    done > $DIR/$FILE5
    cut -f2- $LOG > $DIR/$FILE6
    echo -e "g/_:main_quit/d\ng|$FILE6|d\ng/:load/s|$FILE1|$FILE4|\ng/:load/s|$FILE2|$FILE5|\nwq" | ed -s $DIR/$FILE6
    for i in {1..5}; do
        cat $DIR/$FILE6
    done >$BIG_INPUT
    cp "$BIG_INPUT" "$BIG_INPUT2"
    echo "_:load $BIG_INPUT2" >>$BIG_INPUT
    echo "_:main_quit" >>$BIG_INPUT
    echo -e "g/_:load/d\ng/^#/d\nwq" | ed -s $BIG_INPUT2
    ./pipeglade -i $FIN -o $FOUT -O $BIG_INPUT_ERR -b >/dev/null
    cat "$FOUT" >/dev/null &
    cat "$BIG_INPUT" > "$FIN"
    while test \( -e $FOUT -a -e $FIN \); do sleep .5; done
    check_cmd "test $(grep -v -e WARNING -e '^$' $BIG_INPUT_ERR | wc -l) -eq 0"
    cat $BIG_INPUT_ERR
    rm -f $BIG_INPUT_ERR
    ./pipeglade -O $BIG_INPUT_ERR <$BIG_INPUT >/dev/null
    check_cmd "test $(grep -v -e WARNING -e '^$' $BIG_INPUT_ERR | wc -l) -eq 0"
fi


echo "
# BATCH FOUR
#
# Tests of project metadata
######################################################################
"

# Does the manual page cover all implemented actions and no unimplemented ones?
check_cmd 'test "`make prog-actions`" == "`make man-actions`"'

# Is the manual page table of contents complete and correct?
check_cmd 'test "`make man-widgets`" == "`make man-toc`"'

# Are the correct widgets in buildables.txt marked done?
check_cmd 'test "`make man-widgets`" == "`make done-list`"'

# Is our collection of test widgets complete?
check_cmd 'test "`make man-widgets | sed s/Gtk// | tr \"[:upper:]\" \"[:lower:]\"`" == "`make examples-list | sed s/\\.ui$//`"'


# 
echo "
# BATCH FIVE
#
# Possible and impossible combinations of widgets and actions.  Not
# crashing means test passed, here.
######################################################################
"

echo -e "main:ping" > mainping
echo -e "_:main_quit" > mainquit
# check_alive command
check_alive() {
    echo "$SEND ${1}"
    echo -e "$1" >$FIN
    while read -t .5 <$FOUT; do : ; done
    if test -p $FIN; then
        timeout -k0 1 cp mainping $FIN
    fi
    if test -p $FOUT; then
        :>tempfout
        timeout -k0 1 cp $FOUT tempfout
        # read -t .5 r <tempfout
    fi
    if grep -q "main:ping" tempfout; then
        count_ok
        echo " $OK"
    else
        count_fail
        echo " $FAIL"
    fi
}


for wid in `make examples-list`; do
    PID=`./pipeglade -i $FIN -o $FOUT -b -u widget-examples/$wid`
    cmd=""
    for act in `make prog-actions`; do
        if test "$act" != "main_quit"; then
            cmd="$cmd\n ${wid/\.ui/}1:$act"
        fi
    done
    check_alive "$cmd"
    timeout -k0 1 cp mainquit $FIN
    kill $PID
done


echo "PASSED: $OKS/$TESTS; FAILED: $FAILS/$TESTS"
