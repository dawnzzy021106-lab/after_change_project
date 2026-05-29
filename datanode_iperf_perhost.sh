#!/usr/bin/env bash
# datanode_iperf_perhost.sh
# 在本机：为 XML 中属于本机的每个 datanode 起 iperf3 -s -p <映射端口>，
# 再依次 iperf3 -c 127.0.0.1 -p <映射端口> 做 client 测试。
#
# 依赖: bash, python3, iperf3
#
# 映射（可调）:
#   IPERF_PORT_FN: 默认 40000+rpc_port；若坚持与数据面同口，需先停 Datanode 并设 USE_DATA_PORT=1（见下）
#
# 用法（在 192.168.1.13 上）:
#   export THIS_HOST=192.168.1.13
#   ./datanode_iperf_perhost.sh clusters.xml start-servers
#   ./datanode_iperf_perhost.sh clusters.xml client-test
#   ./datanode_iperf_perhost.sh clusters.xml stop-servers
#
# 或显式传主机:
#   ./datanode_iperf_perhost.sh clusters.xml start-servers 192.168.1.13

set -euo pipefail

XML="${1:?第一个参数: clusters.xml}"
CMD="${2:?第二个参数: start-servers | stop-servers | client-test}"
THIS_HOST="${3:-${THIS_HOST:-}}"

IPERF_SECONDS="${IPERF_SECONDS:-10}"
IPERF_PARALLEL="${IPERF_PARALLEL:-1}"
SOCKET_PORT_OFFSET="${SOCKET_PORT_OFFSET:-500}"
# 0=用 40000+rpc 作为 iperf 监听端口；1=用 rpc+500（与业务同口，须先停 Datanode）
USE_DATA_PORT="${USE_DATA_PORT:-0}"

if [[ -z "$THIS_HOST" ]]; then
  echo "请设置 THIS_HOST=本机IP，或: $0 <xml> <cmd> <THIS_HOST>" >&2
  exit 1
fi

rpc_to_iperf_port() {
  local rpc="$1"
  if [[ "$USE_DATA_PORT" == "1" ]]; then
    echo $((rpc + SOCKET_PORT_OFFSET))
  else
    local base="${IPERF_PORT_BASE:-40000}"
    echo $((base + rpc))
  fi
}

# 输出本机 THIS_HOST 上所有 datanode 的: cluster_id rpc_port iperf_port
list_ports_py='
import sys, xml.etree.ElementTree as ET
host = sys.argv[2]
tree = ET.parse(sys.argv[1])
for cl in tree.findall("cluster"):
    cid = cl.get("id", "")
    for dn in cl.find("datanodes").findall("datanode"):
        uri = dn.get("uri", "")
        if not uri or ":" not in uri:
            continue
        dip, dport = uri.rsplit(":", 1)
        if dip != host:
            continue
        print(cid, dport)
'

mapfile -t LINES < <(python3 -c "$list_ports_py" "$XML" "$THIS_HOST")
if [[ ${#LINES[@]} -eq 0 ]]; then
  echo "在 $XML 中未找到 datanode IP == $THIS_HOST 的条目" >&2
  exit 1
fi

declare -a IPERF_PORTS=()
for line in "${LINES[@]}"; do
  cid="${line%% *}"
  rpc="${line#* }"
  p="$(rpc_to_iperf_port "$rpc")"
  IPERF_PORTS+=("$p")
  echo "# cluster=$cid rpc=$rpc app_tcp=$((rpc + SOCKET_PORT_OFFSET)) iperf_tcp=$p"
done

start_servers() {
  for p in "${IPERF_PORTS[@]}"; do
    if ss -ltn "sport = :$p" 2>/dev/null | grep -q ":$p"; then
      echo "WARN: 端口 $p 已被监听，跳过启动（或先 stop-servers）" >&2
      continue
    fi
    echo "启动: iperf3 -s -p $p -D"
    iperf3 -s -p "$p" -D
  done
  echo "已尝试在后台启动 ${#IPERF_PORTS[@]} 个 iperf3 server（重复端口已跳过）"
}

stop_servers() {
  for p in "${IPERF_PORTS[@]}"; do
    # 仅结束监听在映射端口上的 iperf3（粗暴：按端口杀）
    if command -v fuser >/dev/null 2>&1; then
      fuser -k "${p}/tcp" 2>/dev/null || true
    fi
  done
  pkill -f "iperf3 -s -p" 2>/dev/null || true
  echo "已尝试释放本机相关 iperf3 server（若仍占用请手工检查）"
}

client_test() {
  for p in "${IPERF_PORTS[@]}"; do
    echo "=========================================="
    echo "CLIENT -> 127.0.0.1:$p (${IPERF_SECONDS}s, -P ${IPERF_PARALLEL})"
    iperf3 -c 127.0.0.1 -p "$p" -t "${IPERF_SECONDS}" -P "${IPERF_PARALLEL}" -f m
    echo "=========================================="
  done
}

case "$CMD" in
  start-servers) start_servers ;;
  stop-servers)  stop_servers ;;
  client-test)   client_test ;;
  *)
    echo "未知命令: $CMD （start-servers | stop-servers | client-test）" >&2
    exit 1
    ;;
esac