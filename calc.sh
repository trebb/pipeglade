#! /usr/bin/env bash

coproc dc 2>&1
./pipeglade -i calc.in -o calc.out -b -u calc.ui -O calc.err -l calc.log >/dev/null

NUMKEYS=(0 1 2 3 4 5 6 7 8 9 A B C D E F)
NUMPAD=("${NUMKEYS[@]}" neg point div mul minus plus squareroot power)
STORAGEKEYS=(recall store)
OTHERKEYS=(enter edit swap clear drop cancel)
VARKEYS=(var_2 var_3 var_a var_b var_c var_d var_e var_f var_g var_h var_i \
               var_j var_k var_l var_m var_n var_o var_p var_q var_r var_s \
               var_t var_u var_v var_w var_x var_y var_z)

varkeys_set_visible()
# expose second keybord level
{
    for i in "${VARKEYS[@]}"; do
        echo "$i:set_visible $1" >calc.in
    done
}

printstack()
{
    echo "entry:set_placeholder_text" >calc.in
    # tell dc to print its stack
    echo "f"
    i=0
    unset DC_OUT
    # read dc's response, which may include error messages
    while read -t .1 DC_OUT[$i]; do (( i++ )); done
    # check for specific errors
    if [[ "${DC_OUT[0]}" =~ "stack" || "${DC_OUT[0]}" =~ "zero" ]]; then
        echo "entry:set_placeholder_text ${DC_OUT[0]}"
        echo "stack:grab_focus" # unfocussing entry
    else
        # display stack in a GtkTreeView
        echo "stack:clear"
        echo "stack:set 5 1"
        ROW=4
        while [[ $i -gt 0 ]]; do
            (( i-- ))
            (( ROW++ ))
            echo "stack:set $ROW 0 $i"
            echo "stack:set $ROW 1 ${DC_OUT[$i]}"
        done
        echo "stack:scroll $ROW 1"
    fi >calc.in
}

edittop()
# put top of stack into GtkEntry
{
    echo "p s!"                 # talking to dc,
    read -t .1 DC_OUT           # reading dc's response,
    # and sending it to pipeglade
    echo "entry:set_text $DC_OUT" >calc.in
}


# initial window dressing
echo "precision:set_value 5" >calc.in
for i in "${NUMPAD[@]}"; do
    echo "$i:style border-radius:20px; border-color:darkblue; font-weight:bold; font-size:16px"
done >calc.in
for i in "${OTHERKEYS[@]}"; do
    echo "$i:style border-radius:20px; font-weight:bold"
done >calc.in
for i in "${STORAGEKEYS[@]}"; do
    echo "$i:style border-radius:20px; border-color:darkgreen; font-weight:bold"
done >calc.in
for i in "${VARKEYS[@]}"; do
    echo "$i:style border-radius:10px; border-color:darkgreen; font-style:italic; font-size:16px"
done >calc.in
echo "off:style color:darkred; border-radius:20px; border-color:darkred; font-weight:bold" >calc.in
echo "entry:style font:monospace 12" >calc.in
echo "main:style border-radius:20px" >calc.in


{
    # main loop; stdin and stdout are connected to the dc coprocess
    printstack
    while true; do
        # receive feedback from GUI
        read IN <calc.out
        # if feedback came from a button, it ends ":clicked"
        C="${IN%:clicked}"      # remove ":clicked"
        if [[ "${#C}" -eq 1 ]]; then
            # our digit buttons all have one-character names
            # string the digits together
            NUM="$NUM$C"
            # and put them into our GtkEntry
            echo "entry:set_text $NUM" >calc.in
        elif [[ "$IN" =~ "entry:text " ]]; then
            # feedback from our GtkEntry
            CURRENT_ENTRY="${IN#entry:text }"
            NUM="$CURRENT_ENTRY"
            # dc uses '_' as a negative sign
            CURRENT_ENTRY="${CURRENT_ENTRY//-/_}"
        elif [[ "$IN" =~ "var_" ]]; then
            # freedback from variable buttons
            VAR="${IN#var_}"
            VAR="${VAR%:clicked}"
            if [[ "$VARMODE" == "recall" ]]; then
                echo "L$VAR"
            elif [[ "$VARMODE" == "store" ]]; then
                echo "S$VAR"
            fi
            printstack
            varkeys_set_visible 0
        elif [[ "$IN" =~ "radix:value " ]]; then
            # feedback from the radix scale
            RADIX="${IN#radix:value }"
            RADIX="${RADIX/.[0-9]*}"
            # telling dc what to do
            echo "A i $RADIX o $RADIX i"
            printstack
            # graying out meaningless digit keys
            for i in "${NUMKEYS[@]:2:(( $RADIX - 1 ))}"; do
                echo "$i:set_sensitive 1" >calc.in
            done
            for i in "${NUMKEYS[@]:$RADIX}"; do
                echo "$i:set_sensitive 0" >calc.in
            done
        elif [[ $IN =~ "precision:value " ]]; then
            # feedback from the precision scale
            PRECISION="${IN#precision:value }"
            PRECISION="${PRECISION/.[0-9]*}"
            echo "$PRECISION k"
        elif [[ $IN == "main:closed" ]]; then
            # exit gracefully when GUI gets killed by window manager
            exit
        elif [[ -n $C ]]; then
            # here, $C is a multi-character button name that doesn't look like "var_x"
            case "$C" in
                point)
                    NUM="$NUM."
                    echo "entry:set_text $NUM" >calc.in
                    ;;
                neg)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY _1 *"
                        edittop
                    else
                        echo "_1 *"
                        unset NUM
                        unset CURRENT_ENTRY
                        printstack
                    fi
                    ;;
                edit)
                    edittop
                    printstack
                    ;;
                enter)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    else
                        echo "d"
                    fi
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                div)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "/"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                mul)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "*"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                minus)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "-"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                plus)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "+"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                power)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "^"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                squareroot)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    echo "v"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                swap)
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                    fi
                    # portability kludge. The parentheses are dc variable names
                    echo "s(s)L(L)"
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                drop)
                    if [[ -z "$CURRENT_ENTRY" ]]; then
                        # portabilty kludge (and memory leak)
                        echo "s!"
                    fi
                    echo "entry:set_text" >calc.in
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                clear)
                    echo "c"
                    echo "entry:set_text" >calc.in
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                cancel)
                    varkeys_set_visible 0
                    echo "entry:set_text" >calc.in
                    unset NUM
                    unset CURRENT_ENTRY
                    printstack
                    ;;
                recall)
                    VARMODE="recall"
                    varkeys_set_visible 1
                    ;;
                store)
                    VARMODE="store"
                    if [[ -n "$CURRENT_ENTRY" ]]; then
                        echo "LE: $CURRENT_ENTRY" >>ttt
                        echo "$CURRENT_ENTRY"
                        echo "entry:set_text" >calc.in
                        unset NUM
                        unset CURRENT_ENTRY
                    fi
                    varkeys_set_visible 1
                    ;;
                off)
                    echo "_:main_quit" >calc.in
                    exit
                    ;;
            esac
        fi
    done
} <&"${COPROC[0]}" >&"${COPROC[1]}"
