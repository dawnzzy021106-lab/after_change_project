#!/bin/bash
###############################################################################
# cross_node_iperf_test.sh
# Purpose: Test cross-node network bandwidth using iperf3.
#          Run on 192.168.1.13 (control node). SSH's to other nodes to start
#          iperf3 servers and run client tests.
#
#          Each direction: 5 runs, reports max/min/avg throughput.
#
# Execution (on control node):
#   cd ~/zzy/ec_prototype-master/after_change_project/v5.5_claude_cross_speed
#   bash cross_node_iperf_test.sh
#
# Manual servers (skip script start/stop; you start iperf3 -s on each node first):
#   bash start_iperf_servers.sh
#   MANUAL_SERVERS=1 bash cross_node_iperf_test.sh
#
# Output:
#   ~/zzy/ec_prototype-master/iperf_results/
#     summary.csv   - per-run throughput
#     stats.csv     - max/min/avg per direction
#     iperf_*.json  - raw iperf3 JSON per run
###############################################################################

set -euo pipefail

# ---------------------------------------------------------------------------
# Safety: refuse to run as root to avoid unintended system changes
# ---------------------------------------------------------------------------
if [[ $(id -u) -eq 0 ]]; then
    echo "[ERROR] Do NOT run this script as root. Use a regular user (e.g. hadoop)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Project root on control node (iperf_results written here)
PROJECT_ROOT="${HOME}/zzy/ec_prototype-master"
CONTROL_NODE="192.168.1.13"
IPERF_PORT=5201
# iperf3 -n accepts "32M"; avoid numfmt (older coreutils reject "32MB")
BYTES="32M"
INTERVAL=10
RESULTS_DIR="${PROJECT_ROOT}/iperf_results"
SUMMARY_FILE="${RESULTS_DIR}/summary.csv"
STATS_FILE="${RESULTS_DIR}/stats.csv"
RUNS=5
SSH_USER="hadoop"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5"
DRY_RUN=0
# 1 = do not start/stop servers (use manually started iperf3 -s on all nodes)
MANUAL_SERVERS=${MANUAL_SERVERS:-0}

# Hardcoded nodes
NODE_IPS=(
    "192.168.1.13"
    "192.168.1.14"
    "192.168.1.20"
    "192.168.1.25"
    "192.168.1.21"
)

# ---------------------------------------------------------------------------
# Color output helpers
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Logs must go to stderr: functions called inside $(...) must only print data on stdout
info()  { echo -e "${GREEN}[INFO]${NC}  $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*" >&2; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
check_prerequisites() {
    if ! command -v iperf3 &>/dev/null; then
        error "iperf3 not found. Install: sudo apt install iperf3"
        exit 1
    fi

    if ! command -v ssh &>/dev/null; then
        error "ssh not found."
        exit 1
    fi

    if [[ -z "$SSH_USER" ]]; then
        error "SSH_USER is empty. Set it in the script configuration."
        exit 1
    fi

    # Ensure results directory is under PROJECT_ROOT (safety: never write elsewhere)
    case "$RESULTS_DIR" in
        "$PROJECT_ROOT"/*) ;;  # OK
        *) error "RESULTS_DIR must be under PROJECT_ROOT ($PROJECT_ROOT). Got: $RESULTS_DIR" ; exit 1 ;;
    esac

    if [[ ! -d "$PROJECT_ROOT" ]]; then
        warn "PROJECT_ROOT does not exist yet: $PROJECT_ROOT (will create iperf_results on run)"
    fi

    if ! is_local_host "$CONTROL_NODE"; then
        warn "This host is not $CONTROL_NODE. iperf3 for control node will be started via SSH."
        warn "Recommended: run this script on the control node ($CONTROL_NODE)."
    fi

    info "All prerequisites satisfied."
}

# ---------------------------------------------------------------------------
# Generate all unique pairs of nodes
# ---------------------------------------------------------------------------
generate_pairs() {
    PAIRS=()
    for ((i = 0; i < ${#NODE_IPS[@]}; i++)); do
        for ((j = i + 1; j < ${#NODE_IPS[@]}; j++)); do
            PAIRS+=("${NODE_IPS[$i]}|${NODE_IPS[$j]}")
        done
    done
    info "Total node pairs: ${#PAIRS[@]} (bidirectional: $(( ${#PAIRS[@]} * 2 )) directions)"
    info "Runs per direction: $RUNS"
    info "Total tests: $(( ${#PAIRS[@]} * 2 * RUNS ))"
}

# ---------------------------------------------------------------------------
# SSH helper
# ---------------------------------------------------------------------------
ssh_run() {
    local host="$1" user="$2" cmd="$3"
    ssh ${SSH_OPTS} "${user}@${host}" "$cmd"
}

# True if $1 is an IP assigned to this machine
is_local_host() {
    local ip="$1"
    ip -4 addr show 2>/dev/null | grep -qw "inet ${ip}/" && return 0
    hostname -I 2>/dev/null | tr ' ' '\n' | grep -qx "$ip" && return 0
    return 1
}

# Parse iperf3 --json bits/sec from stdout (may include SSH noise)
parse_iperf3_bps() {
    local raw="$1"
    echo "$raw" | python3 -c "
import sys, json
text = sys.stdin.read()
start = text.find('{')
if start < 0:
    print(0)
    raise SystemExit
text = text[start:]
try:
    data = json.loads(text)
except Exception:
    print(0)
    raise SystemExit
if data.get('error'):
    print(0)
    raise SystemExit
end = data.get('end') or {}
for block_name in ('sum_sent', 'sum_received', 'sum'):
    block = end.get(block_name)
    if not isinstance(block, dict):
        continue
    for field in ('bits_per_second', 'throughput'):
        if field in block:
            print(int(float(block[field])))
            raise SystemExit
print(0)
" 2>/dev/null || echo "0"
}

# ---------------------------------------------------------------------------
# Start iperf3 server on a remote host (idempotent)
# ---------------------------------------------------------------------------
start_iperf_server() {
    local host="$1"
    info "Starting iperf3 server on $host ..."

    ssh_run "$host" "$SSH_USER" \
        "pkill -f 'iperf3 -s -p $IPERF_PORT' 2>/dev/null || true; \
         nohup iperf3 -s -p $IPERF_PORT -D --logfile /tmp/iperf3_server_\${HOSTNAME}.log 2>&1 & \
         sleep 1; echo OK" >/dev/null 2>&1

    if ssh_run "$host" "$SSH_USER" \
        "pgrep -f 'iperf3 -s -p $IPERF_PORT' >/dev/null" 2>/dev/null; then
        info "  iperf3 server running on $host"
        return 0
    else
        error "  Failed to start iperf3 server on $host"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Stop iperf3 server on a remote host
# ---------------------------------------------------------------------------
stop_iperf_server() {
    local host="$1"
    if is_local_host "$host"; then
        pkill -f "iperf3 -s -p $IPERF_PORT" 2>/dev/null || true
    else
        ssh_run "$host" "$SSH_USER" \
            "pkill -f 'iperf3 -s -p $IPERF_PORT' 2>/dev/null || true" 2>/dev/null
    fi
    info "Stopped iperf3 server on $host"
}

start_iperf_server_local() {
    pkill -f "iperf3 -s -p $IPERF_PORT" 2>/dev/null || true
    nohup iperf3 -s -p "$IPERF_PORT" -D --logfile /tmp/iperf3_server_local.log 2>&1 &
    sleep 1
    pgrep -f "iperf3 -s -p $IPERF_PORT" >/dev/null
}

# Verify server is listening (pgrep alone is not enough)
verify_iperf_server() {
    local host="$1"
    if is_local_host "$host"; then
        ss -tln 2>/dev/null | grep -q ":${IPERF_PORT} " && return 0
        netstat -tln 2>/dev/null | grep -q ":${IPERF_PORT} " && return 0
        return 1
    fi
    ssh_run "$host" "$SSH_USER" \
        "ss -tln 2>/dev/null | grep -q ':${IPERF_PORT} ' || netstat -tln 2>/dev/null | grep -q ':${IPERF_PORT} '" \
        2>/dev/null
}

# ---------------------------------------------------------------------------
# Run a single iperf3 test: client_host -> server_host
# Prints throughput in Mbps to stdout
# ---------------------------------------------------------------------------
run_single_test() {
    local client="$1" server="$2"
    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S%N)

    local cmd="iperf3 -c ${server} -p ${IPERF_PORT} -n ${BYTES} --json"

    if [[ $DRY_RUN -eq 1 ]]; then
        echo "[DRY-RUN] Would execute on ${client}: $cmd"
        return 0
    fi

    if ! verify_iperf_server "$server"; then
        warn "  No listener on ${server}:${IPERF_PORT} before test"
    fi

    local raw_output
    if is_local_host "$client"; then
        raw_output=$(iperf3 -c "${server}" -p "${IPERF_PORT}" -n "${BYTES}" --json 2>&1) || true
    else
        raw_output=$(ssh_run "$client" "$SSH_USER" "$cmd" 2>&1) || true
    fi

    # Save raw output (JSON plus possible SSH messages)
    echo "$raw_output" > "${RESULTS_DIR}/iperf_${client}_to_${server}_${timestamp}.json"

    # Parse throughput (bits/sec -> Mbps)
    local bps mbps
    bps=$(parse_iperf3_bps "$raw_output")
    mbps=$(awk "BEGIN {printf \"%.2f\", ${bps}/1000000}")

    if [[ "$bps" == "0" || "$mbps" == "0.00" ]]; then
        warn "  Connection failed or could not parse throughput (see ${RESULTS_DIR}/iperf_${client}_to_${server}_${timestamp}.json)"
        echo "FAILED"
        return 1
    fi

    echo "${mbps}"
    return 0
}

# ---------------------------------------------------------------------------
# Run RUNS consecutive tests for one direction, collect results
# Prints: max,min,avg,valid_count
# Also appends per-run data to the summary CSV
# ---------------------------------------------------------------------------
run_repeated_tests() {
    local client="$1" server="$2"
    local all_mbps=()
    local run_num=0

    for ((r = 1; r <= RUNS; r++)); do
        run_num=$r
        info "  Run ${r}/${RUNS}: ${client} -> ${server}"
        local mbps
        mbps=$(run_single_test "$client" "$server")

        # Only accept a numeric Mbps value (ignore any accidental stdout noise)
        if [[ "$mbps" == "FAILED" || ! "$mbps" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            warn "  Run ${r} failed, skipping (got: ${mbps:-empty})"
            all_mbps+=(0)
            echo "${client},${server},${r},FAILED" >> "$SUMMARY_FILE"
        else
            info "  Throughput: ${mbps} Mbps"
            all_mbps+=("$mbps")
            echo "${client},${server},${r},${mbps}" >> "$SUMMARY_FILE"
        fi

        # Interval between runs (skip after last run)
        if [[ $r -lt $RUNS && $DRY_RUN -eq 0 ]]; then
            sleep $INTERVAL
        fi
    done

    # Compute stats using awk
    local stats
    stats=$(printf '%s\n' "${all_mbps[@]}" | awk '
    BEGIN { n=0; sum=0; min=999999999; max=-1 }
    {
        val = $1 + 0
        if (val > 0) {
            n++
            sum += val
            if (val < min) min = val
            if (val > max) max = val
        }
    }
    END {
        if (n > 0) {
            printf "%.2f,%.2f,%.2f,%d", max, min, sum/n, n
        } else {
            print "0.00,0.00,0.00,0"
        }
    }')

    echo "${stats}"
}

# ---------------------------------------------------------------------------
# Main test loop
# ---------------------------------------------------------------------------
run_all_tests() {
    mkdir -p "$RESULTS_DIR"

    # CSV headers
    echo "client_ip,server_ip,run,throughput_mbps" > "$SUMMARY_FILE"
    echo "client_ip,server_ip,max_mbps,min_mbps,avg_mbps,valid_runs" > "$STATS_FILE"

    local server_hosts=("${NODE_IPS[@]}")

    if [[ "$MANUAL_SERVERS" -eq 1 ]]; then
        info "=== MANUAL_SERVERS=1: skipping automatic server start/stop ==="
        info "    Ensure iperf3 -s -p ${IPERF_PORT} is running on every node (see start_iperf_servers.sh)"
    else
        info "=== Starting iperf3 servers on all nodes ==="
        server_hosts=()
        for host in "${NODE_IPS[@]}"; do
            if [[ "$host" == "$CONTROL_NODE" ]] && is_local_host "$CONTROL_NODE"; then
                if start_iperf_server_local; then
                    info "  iperf3 server running locally on $host"
                    server_hosts+=("$host")
                else
                    warn "  Failed to start local iperf3 server on $host"
                fi
                continue
            fi

            if start_iperf_server "$host"; then
                server_hosts+=("$host")
            else
                warn "  Skipping pairs involving $host"
            fi
        done
    fi

    info "=== Verifying iperf3 servers (port ${IPERF_PORT}) ==="
    for host in "${NODE_IPS[@]}"; do
        if verify_iperf_server "$host"; then
            info "  OK  $host:${IPERF_PORT} listening"
        else
            error "  FAIL $host:${IPERF_PORT} not listening — tests to/from this host will fail"
        fi
    done

    info "=== Running cross-node bandwidth tests ==="
    local test_idx=0
    local total=$(( ${#PAIRS[@]} * 2 ))

    for pair in "${PAIRS[@]}"; do
        IFS='|' read -r node_a node_b <<< "$pair"

        # --- Direction 1: node_a -> node_b (RUNS times) ---
        echo ""
        echo -e "${CYAN}[$((test_idx+1))/$total]${NC} ${CYAN}${node_a} ---> ${node_b}${NC} (${RUNS} runs)"
        local stats_ab
        stats_ab=$(run_repeated_tests "$node_a" "$node_b")
        IFS=',' read -r max_ab min_ab avg_ab runs_ab <<< "$stats_ab"
        echo "$node_a,$node_b,$max_ab,$min_ab,$avg_ab,$runs_ab" >> "$STATS_FILE"

        [[ $DRY_RUN -eq 0 ]] && sleep $INTERVAL
        test_idx=$((test_idx + 1))

        # --- Direction 2: node_b -> node_a (RUNS times) ---
        echo ""
        echo -e "${CYAN}[$((test_idx+1))/$total]${NC} ${CYAN}${node_b} ---> ${node_a}${NC} (${RUNS} runs)"
        local stats_ba
        stats_ba=$(run_repeated_tests "$node_b" "$node_a")
        IFS=',' read -r max_ba min_ba avg_ba runs_ba <<< "$stats_ba"
        echo "$node_b,$node_a,$max_ba,$min_ba,$avg_ba,$runs_ba" >> "$STATS_FILE"

        [[ $DRY_RUN -eq 0 ]] && sleep $INTERVAL
        test_idx=$((test_idx + 1))

        # Print per-direction stats
        echo ""
        info "  ${node_a} -> ${node_b}  | Max: ${max_ab}  Min: ${min_ab}  Avg: ${avg_ab}  (${runs_ab}/${RUNS} valid)"
        info "  ${node_b} -> ${node_a}  | Max: ${max_ba}  Min: ${min_ba}  Avg: ${avg_ba}  (${runs_ba}/${RUNS} valid)"
        echo ""
    done

    if [[ "$MANUAL_SERVERS" -eq 1 ]]; then
        info "=== MANUAL_SERVERS=1: leaving iperf3 servers running ==="
    else
        info "=== Cleaning up iperf3 servers ==="
        for host in "${server_hosts[@]}"; do
            stop_iperf_server "$host"
        done
    fi
}

# ---------------------------------------------------------------------------
# Print summary table
# ---------------------------------------------------------------------------
print_summary() {
    echo ""
    info "========== Test Summary =========="
    echo ""
    printf "${YELLOW}%-18s %-18s %10s %10s %10s${NC}\n" "CLIENT" "SERVER" "MAX" "MIN" "AVG"
    printf "${YELLOW}%-18s %-18s %10s %10s %10s${NC}\n" "------" "------" "---" "---" "---"

    tail -n +2 "$STATS_FILE" | while IFS=',' read -r client server max_v min_v avg_v runs; do
        printf "%-18s %-18s %9s Mbps %9s Mbps %9s Mbps  (%s runs)\n" \
            "$client" "$server" "$max_v" "$min_v" "$avg_v" "$runs"
    done

    echo ""
    info "Results directory : $RESULTS_DIR"
    info "Summary CSV      : $SUMMARY_FILE"
    info "Stats CSV        : $STATS_FILE"
    echo ""

    # Overall aggregate statistics
    if command -v python3 &>/dev/null; then
        info "========== Overall Statistics =========="
        python3 -c "
import csv

values = []
with open('$STATS_FILE', newline='') as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            avg = float(row['avg_mbps'])
            if avg > 0:
                values.append(avg)
        except (ValueError, KeyError):
            pass

if values:
    overall_avg = sum(values) / len(values)
    mn  = min(values)
    mx  = max(values)
    print(f'  Directions tested : {len(values)}')
    print(f'  Avg of averages   : {overall_avg:.2f} Mbps')
    print(f'  Best avg direction: {mx:.2f} Mbps')
    print(f'  Worst avg dir     : {mn:.2f} Mbps')
else:
    print('  No valid data.')
"
    fi
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
main() {
    echo -e "${CYAN}============================================================${NC}"
    echo -e "${CYAN}   Cross-Node Network Bandwidth Test (iperf3)              ${NC}"
    echo -e "${CYAN}============================================================${NC}"
    echo ""
    info "Project root    : $PROJECT_ROOT"
    info "Script location : $SCRIPT_DIR"
    info "Control node    : $CONTROL_NODE"
    info "Test nodes      : ${NODE_IPS[*]}"
    info "Transfer size   : 32MB per test"
    info "Runs per dir    : $RUNS"
    info "Interval        : ${INTERVAL}s"
    info "SSH user        : $SSH_USER"
    info "Results dir     : $RESULTS_DIR"
    info "Run from        : cd .../v5.5_claude_cross_speed && bash cross_node_iperf_test.sh"
    info "Manual servers  : MANUAL_SERVERS=${MANUAL_SERVERS} (1 = use pre-started iperf3 -s)"
    echo ""

    check_prerequisites
    generate_pairs
    run_all_tests
    print_summary
}

main "$@"
