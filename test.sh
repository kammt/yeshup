#!/bin/sh
# Test suite for yeshup. Run via `make test`.
# Uses ./yeshup-test, built to hang up after 1-2 s instead of 2-10 min.

Y=./yeshup
YT=./yeshup-test
fails=0
pass() { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
check() { # description expected actual
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (expected $2, got $3)"; fi
}

$Y --help >/dev/null;               check "--help exits 0" 0 $?
$Y --version >/dev/null;            check "--version exits 0" 0 $?
$Y 2>/dev/null;                     check "no command exits 125" 125 $?
$Y sh -c 'exit 3';                  check "exit status is passed through" 3 $?
$Y -- sh -c 'exit 4';               check "-- separates options" 4 $?
$Y /nonexistent/cmd 2>/dev/null;    check "missing command exits 127" 127 $?
$Y /dev/null 2>/dev/null;           check "non-executable exits 126" 126 $?
out=$($Y echo hello world);         check "stdout is untouched" "hello world" "$out"

# Self-inflicted hangup kills a plain command, and the status says SIGHUP.
start=$(date +%s)
$YT sleep 30
st=$?
el=$(( $(date +%s) - start ))
check "random hangup kills command (status 129)" 129 $st
if [ $el -ge 1 ] && [ $el -le 3 ]; then pass "hangup arrives within window (${el}s)"
else fail "hangup arrives within window (took ${el}s)"; fi

# The command can catch the hangup.
$YT sh -c 'trap "exit 9" HUP; while :; do sleep 0.1; done'
check "command can catch the random hangup" 9 $?

# An inherited ignored SIGHUP (e.g. from nohup) is undone.
sh -c "trap '' HUP; exec $YT sleep 30"
check "ignored SIGHUP (nohup) is undone" 129 $?

# SIGHUP sent to yeshup is passed through (real binary: no random hangup soon).
$Y sh -c 'trap "exit 7" HUP; while :; do sleep 0.1; done' &
pid=$!; sleep 0.5; kill -HUP $pid; wait $pid
check "SIGHUP to yeshup is passed through" 7 $?

# SIGTERM sent to yeshup is forwarded too.
$Y sleep 30 &
pid=$!; sleep 0.5; kill -TERM $pid; wait $pid
check "SIGTERM to yeshup is forwarded" 143 $?

echo
if [ $fails -eq 0 ]; then echo "all tests passed"; else echo "$fails test(s) failed"; exit 1; fi
