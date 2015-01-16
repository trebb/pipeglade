#! /usr/bin/env bash

export LC_ALL=C

FIN=to-g.fifo
FOUT=from-g.fifo

rm -f $FIN $FOUT

./pipeglade -i $FIN -o $FOUT &

# wait for $FIN and $FOUT to appear
while test ! \( -e $FIN -a -e $FOUT \); do :; done

while test -e $FIN -a -e $FOUT; do
    read line <$FOUT
    echo "textview2:insert_at_cursor $line\\n" >$FIN
    echo "textview2:scroll_to_cursor\\n" >$FIN
done
