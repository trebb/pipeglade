#!/usr/bin/env bash

(
    H0=0
    M0=0
    S0=0
    C=220
    TX=155
    TY=250
    HL=100
    ML=195
    SL=195
    R=215

    echo "drawingarea1:set_line_cap 10000 round"
    echo "drawingarea1:set_font_size 10000 20"
    echo "drawingarea1:arc 10000 $C $C $R 0 360"
    echo "drawingarea1:set_source_rgba 10000 black"
    echo "drawingarea1:fill 10000"
    while true; do
        D=`date +%F`
        H=$(((`date +"%s"`/1200%72*10)+270+720))
        M=$(((`date +"%s"`/20%180*2)+270+360))
        S=$((((`date +"%s"`%60)*6)+270))
    # date and hour hand
        if [[ H -ne H0 ]]; then
            echo "drawingarea1:move_to $H $TX $TY"
            echo "drawingarea1:set_source_rgba $H white"
            echo "drawingarea1:show_text $H $D"
            echo "drawingarea1:move_to $H $C $C"
            echo "drawingarea1:arc $H $C $C $HL $H $H"
            echo "drawingarea1:set_source_rgba $H rgba(255,255,0,.8)"
            echo "drawingarea1:set_dash $H"
            echo "drawingarea1:set_line_width $H 30"
            echo "drawingarea1:stroke $H"
            echo "drawingarea1:remove $H0"
            H0=$H
        fi
    # minute hand
        if [[ M -ne M0 ]]; then
            echo "drawingarea1:move_to $M $C $C"
            echo "drawingarea1:arc $M $C $C $ML $M $M"
            echo "drawingarea1:set_source_rgba $M rgba(0,255,0,.7)"
            echo "drawingarea1:set_dash $M"
            echo "drawingarea1:set_line_width $M 25"
            echo "drawingarea1:stroke $M"
            echo "drawingarea1:remove $M0"
            M0=$M
        fi
    # second hand
        echo "drawingarea1:move_to $S $C $C"
        echo "drawingarea1:arc $S $C $C $SL $S $S"
        echo "drawingarea1:set_source_rgba $S cyan"
        echo "drawingarea1:set_line_width $S 2"
        echo "drawingarea1:set_dash $S 4 4"
        echo "drawingarea1:stroke $S"
        echo "drawingarea1:remove $S0"
        S0=$S

        sleep 1
    done
) | ./pipeglade -u clock.ui
