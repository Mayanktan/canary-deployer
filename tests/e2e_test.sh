#!/usr/bin/env bash
# End-to-end test: starts a real controller on spare ports with an empty data
# folder, sends traffic through it with loadgen, and checks that
#   - a good release rolls out to every instance
#   - a buggy release and a crashing release are caught by the canary and rolled back
#   - an instance killed with SIGKILL is restarted
#   - users saw no "unavailable" requests during all of the above
#   - the controller remembers the deployed version across a restart
#   - stopping the controller stops every instance (no leftover processes)
#
# Usage: ./tests/e2e_test.sh       (or: make test)
# Set CONTROLLER=bin/tsan/controller to run it against a ThreadSanitizer build.

set -u
cd "$(dirname "$0")/.."

CONTROLLER=${CONTROLLER:-bin/controller}
PROXY_PORT=28080
API_PORT=28081
BASE_PORT=29001
DATA=$(mktemp -d)
PASSED=0
FAILED=0
CONTROLLER_PID=
LOADGEN_PID=

cleanup() {
    [ -n "$LOADGEN_PID" ] && kill "$LOADGEN_PID" 2>/dev/null
    [ -n "$CONTROLLER_PID" ] && kill "$CONTROLLER_PID" 2>/dev/null && wait "$CONTROLLER_PID" 2>/dev/null
    rm -rf "$DATA"
}
trap cleanup EXIT

check() {  # check "description" command...
    local name=$1
    shift
    if "$@"; then
        PASSED=$((PASSED + 1)); printf '  \033[32mPASS\033[0m %s\n' "$name"
    else
        FAILED=$((FAILED + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$name"
    fi
}

wait_until() {  # wait_until SECONDS command...   retries the command every 0.2 s
    local tries=$(($1 * 5))
    shift
    for _ in $(seq 1 "$tries"); do
        "$@" && return 0
        sleep 0.2
    done
    return 1
}

status_json()       { curl -s "127.0.0.1:$API_PORT/status"; }
latest_phase()      { status_json | grep -o '"phase":"[a-z ]*"' | head -1 | cut -d'"' -f4; }
deploy_finished()   { status_json | grep -q '"in_progress":false'; }
count_ready()       { status_json | grep -o "\"version\":\"$1\",\"state\":\"ready\"" | wc -l | tr -d ' '; }
count_instances()   { status_json | grep -o '"state":"' | wc -l | tr -d ' '; }
all_run()           { [ "$(count_ready "$1")" = 3 ] && [ "$(count_instances)" = 3 ]; }
reason_mentions()   { status_json | grep -o '"reason":"[^"]*"' | head -1 | grep -q "$1"; }
no_leftovers()      { ! pgrep -f "demo-service --port 290" > /dev/null; }
controller_exited() { ! kill -0 "$CONTROLLER_PID" 2>/dev/null; }
deployctl()         { ./bin/deployctl -p "$API_PORT" "$@" > /dev/null; }
fails()             { ! "$@"; }

start_controller() {
    "$CONTROLLER" -p $PROXY_PORT -a $API_PORT -b $BASE_PORT -w 4 -d "$DATA" >> "$DATA/controller.out" 2>&1 &
    CONTROLLER_PID=$!
}

stop_controller() {
    kill -TERM "$CONTROLLER_PID"
    wait_until 20 controller_exited
}

deploy_and_wait() {  # deploy_and_wait VERSION EXPECTED_PHASE
    deployctl deploy "$1" && sleep 0.5 && wait_until 40 deploy_finished && [ "$(latest_phase)" = "$2" ]
}

echo "Controller: $CONTROLLER"
start_controller
check "controller starts 3 instances of v1" wait_until 30 all_run v1

./bin/loadgen -p $PROXY_PORT -r 100 > "$DATA/loadgen.out" 2>&1 &
LOADGEN_PID=$!
sleep 1

check "good release v2 rolls out"                        deploy_and_wait v2 "succeeded"
check "  ...and all 3 instances run v2"                  wait_until 5 all_run v2
check "buggy release v3 is rolled back"                  deploy_and_wait v3 "rolled back"
check "  ...because of its error rate"                   reason_mentions "error rate"
check "  ...and users are back on v2"                    wait_until 5 all_run v2
check "crashing release v4 is rolled back"               deploy_and_wait v4 "rolled back"
check "  ...because the canary crashed"                  reason_mentions "crashed"

VICTIM=$(status_json | grep -o "\"port\":[0-9]*,\"pid\":[0-9]*" | head -1 | grep -o '[0-9]*$')
kill -9 "$VICTIM"
check "an instance killed with SIGKILL is restarted"     wait_until 10 grep -q "restart #1" "$DATA/controller.out"
check "  ...and all 3 instances are ready again"         wait_until 10 all_run v2

check "deploying an unknown release is refused"          fails deployctl deploy nope
check "deploying the running version is refused"         fails deployctl deploy v2

kill -INT "$LOADGEN_PID"
wait "$LOADGEN_PID" 2>/dev/null
LOADGEN_PID=
check "users never saw an unavailable service"           grep -q "unavailable: 0$" <(tail -1 "$DATA/loadgen.out")
tail -1 "$DATA/loadgen.out" | sed 's/^/        loadgen:/'

check "SIGTERM stops the controller"                     stop_controller
check "  ...and every instance with it"                  wait_until 5 no_leftovers
start_controller
check "after a restart the controller runs v2 again"     wait_until 30 all_run v2
check "  ...and remembers its deployment history"        [ "$(curl -s 127.0.0.1:$API_PORT/history | grep -o '"id":' | wc -l | tr -d ' ')" = 3 ]
stop_controller
CONTROLLER_PID=

if grep -q "ThreadSanitizer" "$DATA/controller.out"; then
    FAILED=$((FAILED + 1))
    printf '  \033[31mFAIL\033[0m ThreadSanitizer reported a problem:\n'
    grep -A 20 "WARNING: ThreadSanitizer" "$DATA/controller.out" | head -60
fi

echo
echo "$PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
