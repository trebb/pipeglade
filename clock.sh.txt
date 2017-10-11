#!/usr/bin/env bash

IMG=$1                        # optional image file; exit when written

(
    HRLN=100
    MNLN=195
    SCLN=195
    R=215
    ## drawing a clock face
    echo "drawingarea1:translate 10000 220 220"
    echo "drawingarea1:set_line_cap 10000 round"
    echo "drawingarea1:set_source_rgba 10000 black"
    echo "drawingarea1:arc 10000 0 0 $R 0 360"
    echo "drawingarea1:fill 10000"
    # date
    echo "drawingarea1:set_font_face 10000 normal bold"
    echo "drawingarea1:set_font_size 10000 25"
    echo "drawingarea1:set_source_rgba 10000 white"
    echo "drawingarea1:move_to 10000 0 30"
    echo "drawingarea1:rel_move_for 10010 n Today"
    echo "drawingarea1:show_text 10020 Today"
    # hour hand
    echo "drawingarea1:set_source_rgba 10000 rgba(255,255,0,.8)"
    echo "drawingarea1:set_dash 10000"
    echo "drawingarea1:set_line_width 10000 30"
    echo "drawingarea1:move_to 10000 0 0"
    echo "drawingarea1:arc 10030 0 0 $HRLN -90 -90"
    echo "drawingarea1:stroke 10000"
    # minute hand
    echo "drawingarea1:set_source_rgba 10000 rgba(0,255,0,.7)"
    echo "drawingarea1:set_dash 10000"
    echo "drawingarea1:set_line_width 10000 25"
    echo "drawingarea1:move_to 10000 0 0"
    echo "drawingarea1:arc 10040 0 0 $MNLN -90 -90"
    echo "drawingarea1:stroke 10000"
    # second hand
    echo "drawingarea1:set_source_rgba 10000 cyan"
    echo "drawingarea1:set_dash 10000 4 4"
    echo "drawingarea1:set_line_width 10000 2"
    echo "drawingarea1:move_to 10000 0 0"
    echo "drawingarea1:arc 10050 0 0 $SCLN -90 -90"
    echo "drawingarea1:stroke 10000"
    ## now turning the hands by replacing some of the commands above
    HR0=0
    MN0=0
    SC0=0
    while true; do
        D=`date +%F`
        HR=$(((`date +"%s"`/1200%72*10)+270+720))
        MN=$(((`date +"%s"`/20%180*2)+270+360))
        SC=$((((`date +"%s"`%60)*6)+270))
        # date and hour hand
        if [[ HR -ne HR0 ]]; then
            echo "drawingarea1:rel_move_for =10010 n $D"
            echo "drawingarea1:show_text =10020 $D"
            echo "drawingarea1:arc =10030 0 0 $HRLN $HR $HR"
            HR0=$H
        fi
        # minute hand
        if [[ MN -ne MN0 ]]; then
            echo "drawingarea1:arc =10040 0 0 $MNLN $MN $MN"
            MN0=$MN
        fi
        # second hand
        echo "drawingarea1:arc =10050 0 0 $SCLN $SC $SC"
        SC0=$SC
        if [ -z "$IMG" ]; then  # running clock
            sleep 1
        else                    # write image file and exit
            echo "main:snapshot $IMG"
            echo "_:main_quit"
        fi
    done
) | ./pipeglade -u clock.ui
