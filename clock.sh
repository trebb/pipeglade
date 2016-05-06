#!/usr/bin/env bash

(
    HL=100
    ML=195
    SL=195
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
    echo "drawingarea1:arc 10030 0 0 $HL -90 -90"
    echo "drawingarea1:stroke 10000"
    # minute hand
    echo "drawingarea1:set_source_rgba 10000 rgba(0,255,0,.7)"
    echo "drawingarea1:set_dash 10000"
    echo "drawingarea1:set_line_width 10000 25"
    echo "drawingarea1:move_to 10000 0 0"
    echo "drawingarea1:arc 10040 0 0 $ML -90 -90"
    echo "drawingarea1:stroke 10000"
    # second hand
    echo "drawingarea1:set_source_rgba 10000 cyan"
    echo "drawingarea1:set_dash 10000 4 4"
    echo "drawingarea1:set_line_width 10000 2"
    echo "drawingarea1:move_to 10000 0 0"
    echo "drawingarea1:arc 10050 0 0 $SL -90 -90"
    echo "drawingarea1:stroke 10000"
    ## now turning the hands by replacing some of the commands above
    H0=0
    M0=0
    S0=0
    while true; do
        D=`date +%F`
        H=$(((`date +"%s"`/1200%72*10)+270+720))
        M=$(((`date +"%s"`/20%180*2)+270+360))
        S=$((((`date +"%s"`%60)*6)+270))
        # date and hour hand
        if [[ H -ne H0 ]]; then
            echo "drawingarea1:rel_move_for =10010 n $D"
            echo "drawingarea1:show_text =10020 $D"
            echo "drawingarea1:arc =10030 0 0 $HL $H $H"
            H0=$H
        fi
        # minute hand
        if [[ M -ne M0 ]]; then
            echo "drawingarea1:arc =10040 0 0 $ML $M $M"
            M0=$M
        fi
        # second hand
        echo "drawingarea1:arc =10050 0 0 $SL $S $S"
        S0=$S
        sleep 1
    done
) | ./pipeglade -u clock.ui
