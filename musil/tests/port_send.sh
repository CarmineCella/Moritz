#!/bin/sh
# port_send.sh — musil --send finds the token of a running port and is answered; without it, it is refused.
# Usage: port_send.sh path/to/musil
MUSIL=$1
PORT=47791
"$MUSIL" --serve $PORT -e "sleep 4" 2>/dev/null &
SERVER=$!
sleep 1
reply=$(echo '(* 6 7)' | "$MUSIL" --send $PORT)
refused=$(echo 'var x 1' | "$MUSIL" --send $PORT --token wrong-token)
kill $SERVER 2>/dev/null
wait $SERVER 2>/dev/null
[ "$reply" = "42" ] || { echo "FAIL --send: got '$reply'"; exit 1; }
case "$refused" in *refused*) ;; *) echo "FAIL a wrong token was not refused: '$refused'"; exit 1 ;; esac
echo "port_send: ok"
