#!/usr/bin/env bash
###############################################################################
# Run repair experiments for three flow-repair parallelism settings.
#
# Default command per setting:
#   ./project/build/run_client 90 <generated-config.ini> 2
#
# The generated config is written under project/, because run_client resolves the
# config path relative to project/build/../.
#
# Useful overrides:
#   STRIPE_NUM=90 FAILED_NUM=2 VALUES="5 10 15" bash run_parallel_config_tests.sh
#   EC_RANDOM_SEED=20260601 bash run_parallel_config_tests.sh
#   FIX_LAYOUT=0 bash run_parallel_config_tests.sh
#   BUILD=0 bash run_parallel_config_tests.sh
###############################################################################

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/project"
RUN_CLIENT="${SCRIPT_DIR}/project/build/run_client"
RUN_CLIENT_CMD="./project/build/run_client"
BASE_CONFIG="${BASE_CONFIG:-${PROJECT_DIR}/config.ini}"
RESULT_ROOT="${RESULT_ROOT:-${SCRIPT_DIR}/fig/parallel_config_tests}"

STRIPE_NUM="${STRIPE_NUM:-90}"
FAILED_NUM="${FAILED_NUM:-2}"
VALUES="${VALUES:-5 10 15}"
FIX_LAYOUT="${FIX_LAYOUT:-1}"
EC_RANDOM_SEED="${EC_RANDOM_SEED:-20260601}"
BUILD="${BUILD:-1}"

mkdir -p "${RESULT_ROOT}"

timestamp="$(date +%Y%m%d_%H%M%S)"
RESULT_DIR="${RESULT_ROOT}/stripe${STRIPE_NUM}_failed${FAILED_NUM}_${timestamp}"
COMBINED_OUTPUT="${RESULT_DIR}/all_results.txt"
SUMMARY_CSV="${RESULT_DIR}/summary.csv"

mkdir -p "${RESULT_DIR}"

if [[ ! -f "${BASE_CONFIG}" ]]; then
  echo "[ERROR] base config not found: ${BASE_CONFIG}" >&2
  exit 1
fi

if [[ "${BUILD}" == "1" ]]; then
  if command -v cmake >/dev/null 2>&1; then
    echo "[INFO] Building run_client ..."
    cmake --build "${PROJECT_DIR}/build" --target run_client -j "$(nproc 2>/dev/null || echo 4)"
  elif [[ -x "${RUN_CLIENT}" ]]; then
    echo "[WARN] cmake not found; using existing ${RUN_CLIENT}."
    echo "[WARN] Rebuild outside this container if recent C++ changes must take effect."
  else
    echo "[ERROR] cmake not found and run_client is not executable: ${RUN_CLIENT}" >&2
    echo "[ERROR] Install cmake in the container, or run with a prebuilt project/build/run_client." >&2
    exit 1
  fi
fi

if [[ ! -x "${RUN_CLIENT}" ]]; then
  echo "[ERROR] run_client not found or not executable: ${RUN_CLIENT}" >&2
  exit 1
fi

set_config_value() {
  local src="$1"
  local dst="$2"
  local key="$3"
  local value="$4"

  awk -v key="${key}" -v value="${value}" '
    BEGIN { replaced = 0 }
    $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      sub("=.*", "= " value)
      replaced = 1
    }
    { print }
    END {
      if (!replaced) {
        print key " = " value
      }
    }
  ' "${src}" > "${dst}.tmp"
  mv "${dst}.tmp" "${dst}"
}

extract_summary() {
  local config_value="$1"
  local output_file="$2"

  awk -v cfg="${config_value}" '
    BEGIN {
      mode = "";
      total = decoding = network = inner = io = meta = count = ios = "";
    }
    /^=+ .* =+$/ {
      line = $0;
      gsub(/^=+[[:space:]]*/, "", line);
      gsub(/[[:space:]]*=+$/, "", line);
      mode = line;
    }
    /Total Time:/ {
      total = $NF;
      gsub(/s$/, "", total);
    }
    /Decoding \(Pure\):/ {
      decoding = $NF;
      gsub(/s$/, "", decoding);
    }
    /Network \(Pure\):/ {
      network = $NF;
      gsub(/s$/, "", network);
    }
    /Network \(Inner\):/ {
      inner = $NF;
      gsub(/s$/, "", inner);
    }
    /Disk I\/O \(Agg\):/ {
      io = $NF;
      gsub(/s$/, "", io);
    }
    /Meta \(Coord\):/ {
      meta = $NF;
      gsub(/s$/, "", meta);
    }
    /Cross-Cluster-Count:/ {
      count = $NF;
    }
    /I\/Os:/ {
      ios = $NF;
      if (mode != "" && total != "") {
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", cfg, mode, total, decoding, network, inner, io, meta, count, ios;
      }
      total = decoding = network = inner = io = meta = count = ios = "";
    }
  ' "${output_file}" >> "${SUMMARY_CSV}"
}

{
  echo "Parallel config experiment"
  echo "Started: $(date '+%F %T %z')"
  echo "Workspace: ${SCRIPT_DIR}"
  echo "Command template: ./project/build/run_client ${STRIPE_NUM} <generated-config.ini> ${FAILED_NUM}"
  echo "Base config: ${BASE_CONFIG}"
  echo "Values: ${VALUES}"
  if [[ "${FIX_LAYOUT}" == "1" ]]; then
    echo "Fixed random seed: EC_RANDOM_SEED=${EC_RANDOM_SEED}"
  else
    echo "Fixed random seed: disabled"
  fi
  echo
} > "${COMBINED_OUTPUT}"

echo "parallel_value,mode,total_time_s,decoding_s,network_pure_s,network_inner_s,disk_io_s,meta_s,cross_cluster_count,ios" > "${SUMMARY_CSV}"

for value in ${VALUES}; do
  run_name="parallel_${value}_${value}_${value}"
  run_dir="${RESULT_DIR}/${run_name}"
  run_config="${PROJECT_DIR}/generated_${run_name}.ini"
  run_output="${run_dir}/output.txt"

  mkdir -p "${run_dir}"
  cp "${BASE_CONFIG}" "${run_config}"
  set_config_value "${run_config}" "${run_config}" "flow_repair_parallel_unordered" "${value}"
  set_config_value "${run_config}" "${run_config}" "flow_repair_parallel_ordered" "${value}"
  set_config_value "${run_config}" "${run_config}" "flow_repair_parallel_join_ordered" "${value}"
  cp "${run_config}" "${run_dir}/config.ini"

  rm -f "${PROJECT_DIR}"/repair_timeline_stripe_*.svg \
        "${PROJECT_DIR}"/repair_timeline_block_*.svg

  {
    echo "================================================================"
    echo "[RUN] ${run_name}"
    echo "[CONFIG] flow_repair_parallel_* = ${value}"
    echo "[CONFIG] ${run_config}"
    echo "[START] $(date '+%F %T %z')"
    echo "================================================================"
  } | tee -a "${COMBINED_OUTPUT}"

  set +e
  if [[ "${FIX_LAYOUT}" == "1" ]]; then
    (cd "${SCRIPT_DIR}" && EC_RANDOM_SEED="${EC_RANDOM_SEED}" "${RUN_CLIENT_CMD}" "${STRIPE_NUM}" "$(basename "${run_config}")" "${FAILED_NUM}") > "${run_output}" 2>&1
  else
    (cd "${SCRIPT_DIR}" && "${RUN_CLIENT_CMD}" "${STRIPE_NUM}" "$(basename "${run_config}")" "${FAILED_NUM}") > "${run_output}" 2>&1
  fi
  status=$?
  set -e

  {
    cat "${run_output}"
    echo
    echo "[END] $(date '+%F %T %z')"
    echo "[EXIT_STATUS] ${status}"
    echo
  } >> "${COMBINED_OUTPUT}"

  extract_summary "${value}" "${run_output}"

  cp "${PROJECT_DIR}"/repair_timeline_stripe_*.svg "${run_dir}/" 2>/dev/null || true
  cp "${PROJECT_DIR}"/repair_timeline_block_*.svg "${run_dir}/" 2>/dev/null || true

  rm -f "${run_config}"

  if [[ "${status}" -ne 0 ]]; then
    echo "[ERROR] ${run_name} failed with status ${status}. See ${run_output}" >&2
    echo "[ERROR] Last 80 lines from ${run_output}:" >&2
    tail -n 80 "${run_output}" >&2 || true
    exit "${status}"
  fi

  echo "[OK] ${run_name} finished. Output: ${run_output}"
done

echo
echo "[DONE] Results directory: ${RESULT_DIR}"
echo "[DONE] Combined output: ${COMBINED_OUTPUT}"
echo "[DONE] Summary CSV: ${SUMMARY_CSV}"
