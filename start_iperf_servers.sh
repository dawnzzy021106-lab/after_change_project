#!/bin/bash
###############################################################################
# start_iperf_servers.sh
# Start iperf3 server on all test nodes, then verify port 5201 is listening.
#
# Usage (on control node):
#   cd ~/zzy/ec_prototype-master/after_change_project/v5.5_claude_cross_speed
#   bash start_iperf_servers.sh
#
# Then run tests without script-managed servers:
#   MANUAL_SERVERS=1 bash cross_node_iperf_test.sh
###############################################################################

# Do not use set -e: SSH/iperf3 may fail on one host; we report and continue
set -uo pipefail

IPERF_PORT=5201
SSH_USER="hadoop"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"

NODE_IPS=(
    "192.168.1.13"
    "192.168.1.14"
    "192.168.1.20"
    "192.168.1.25"
    "192.168.1.21"
)

info() { echo "[INFO]  $*" >&2; }
warn() { echo "[WARN]  $*" >&2; }
err()  { echo "[ERROR] $*" >&2; }

is_local_host() {
    local ip="$1"
    ip -4 addr show 2>/dev/null | grep -qw "inet ${ip}/" && return 0
    hostname -I 2>/dev/null | tr ' ' '\n' | grep -qx "$ip" && return 0
    return 1
}

# Returns 0 if start command succeeded (not whether port is up)
start_on_host() {
    local host="$1"
    local rc=0

    info "Starting iperf3 server on $host ..."

    if is_local_host "$host"; then
        pkill -f "iperf3 -s -p ${IPERF_PORT}" 2>/dev/null || true
        sleep 1
        if ! iperf3 -s -p "$IPERF_PORT" -D --logfile /tmp/iperf3_server_local.log 2>/tmp/iperf3_start_local.err; then
            warn "  iperf3 start failed on $host (local): $(tail -1 /tmp/iperf3_start_local.err 2>/dev/null)"
            rc=1
        fi
    else
        local out
        out=$(ssh ${SSH_OPTS} "${SSH_USER}@${host}" \
            "pkill -f 'iperf3 -s -p ${IPERF_PORT}' 2>/dev/null || true; sleep 1; \
             iperf3 -s -p ${IPERF_PORT} -D --logfile /tmp/iperf3_server_\$(hostname).log 2>/tmp/iperf3_start.err; \
             echo START_RC=\$?" 2>&1) || rc=$?

        if [[ $rc -ne 0 ]]; then
            err "  SSH to $host failed (exit $rc): ${out:-no output}"
            return 1
        fi
        if [[ "$out" == *START_RC=0* ]]; then
            : # ok
        else
            warn "  Remote start on $host returned: $out"
            rc=1
        fi
    fi

    sleep 2
    return $rc
}

verify_on_host() {
    local host="$1"
    if is_local_host "$host"; then
        ss -tln 2>/dev/null | grep -q ":${IPERF_PORT} " && return 0
        netstat -tln 2>/dev/null | grep -q ":${IPERF_PORT} " && return 0
        return 1
    fi
    ssh ${SSH_OPTS} "${SSH_USER}@${host}" \
        "ss -tln 2>/dev/null | grep -q ':${IPERF_PORT} ' || netstat -tln 2>/dev/null | grep -q ':${IPERF_PORT} '" \
        2>/dev/null
}

failed=0
for host in "${NODE_IPS[@]}"; do
    start_on_host "$host" || warn "  start_on_host reported errors for $host (will still verify port)"

    if verify_on_host "$host"; then
        info "  OK  $host:${IPERF_PORT} listening"
    else
        err "  FAIL $host:${IPERF_PORT} not listening"
        err "       Debug: ssh ${SSH_USER}@${host} 'pgrep -af iperf3; ss -tlnp | grep ${IPERF_PORT}; cat /tmp/iperf3_start.err 2>/dev/null'"
        failed=$((failed + 1))
    fi
done

if [[ $failed -gt 0 ]]; then
    err "$failed node(s) failed — fix before running cross_node_iperf_test.sh"
    exit 1
fi

info "All servers ready. Run: MANUAL_SERVERS=1 bash cross_node_iperf_test.sh"
