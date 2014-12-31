#! /usr/bin/env bash

export LC_ALL=C

FIN=to-g.fifo
FOUT=from-g.fifo

./pipeglade -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

while test -e $FIN -a -e $FOUT; do
    read line <$FOUT
    echo "textview2:text_view:insert_at_cursor $line\\n" >$FIN
    echo "textview2:text_view:scroll_to_cursor\\n" >$FIN
done
