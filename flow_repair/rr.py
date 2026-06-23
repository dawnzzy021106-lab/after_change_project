#!/usr/bin/env python3
"""
Random Repair (RR) plan generator.

repair.cpp writes failures_map as an Available matrix, one row per failed stripe and
one column per cluster. This script reads that matrix and writes cluster_data.bin
using the same binary layout as complete_min_cost_max_flow.cpp:

  size_t main_size
  int    main_help_clusterID[main_size]
  size_t outer_size
  repeated outer_size times:
    size_t inner_size
    repeated inner_size times:
      int cluster_id
      int chunk_num

The cluster IDs written here are 1-based, matching repair.cpp's flow_repair path
before it maps cluster IDs to local partition IDs.
"""

from __future__ import annotations

import argparse
import random
import re
import struct
from pathlib import Path
from typing import List, Sequence, Tuple

DEFAULT_AVAILABLE_MATRIX = Path("/home/hadoop/zzy/ec_prototype-master/flow_repair/Available_matrix")
DEFAULT_OUTPUT = Path("/home/hadoop/zzy/ec_prototype-master/flow_repair/cluster_data.bin")


def read_available_matrix(filename: str | Path) -> List[List[int]]:
    """Read Available = {{...}, {...}} matrix emitted by repair.cpp."""
    path = Path(filename)
    text = path.read_text(encoding="utf-8")

    start = text.find("Available")
    if start < 0:
        raise ValueError(f"Cannot find 'Available' in {path}")

    brace_start = text.find("{", start)
    brace_end = text.rfind("};")
    if brace_start < 0 or brace_end < 0 or brace_end <= brace_start:
        raise ValueError(f"Invalid Available matrix format in {path}")

    body = text[brace_start + 1 : brace_end]
    rows: List[List[int]] = []
    for row_text in re.findall(r"\{([^{}]*)\}", body):
        row = [int(token) for token in re.findall(r"-?\d+", row_text)]
        if row:
            rows.append(row)

    if not rows:
        raise ValueError(f"No matrix rows parsed from {path}")

    width = len(rows[0])
    for idx, row in enumerate(rows):
        if len(row) != width:
            raise ValueError(
                f"Available matrix is ragged: row 0 has {width} columns, "
                f"row {idx} has {len(row)} columns"
            )

    return rows


def _choose_main_cluster(row: Sequence[int], rng: random.Random) -> int:
    """Choose a 0-based main/helper-destination cluster with local available chunks."""
    candidates = [idx for idx, chunk_num in enumerate(row) if chunk_num > 0]
    if not candidates:
        raise ValueError(f"No available chunks in stripe row: {list(row)}")
    return rng.choice(candidates)


def build_random_repair_plan(
    available: Sequence[Sequence[int]], ec_k: int, rng: random.Random
) -> Tuple[List[int], List[List[Tuple[int, int]]]]:
    """
    Build RR plan from Available matrix.

    For each failed stripe:
      1. randomly select one available cluster as main_help_clusterID;
      2. count chunks already available in that main cluster;
      3. randomly order the remaining available clusters;
      4. add (clusterID, chunkNum) pairs until ec_k chunks are covered.

    The main cluster itself is not included in other_help_clusterID_chunkNum_pairs,
    because repair.cpp treats main_help_clusterID separately.
    """
    main_help_cluster_id: List[int] = []
    other_help_pairs: List[List[Tuple[int, int]]] = []

    for stripe_idx, row in enumerate(available):
        main_idx = _choose_main_cluster(row, rng)
        main_help_cluster_id.append(main_idx + 1)  # 1-based cluster ID

        selected: List[Tuple[int, int]] = []
        covered_chunks = row[main_idx]

        helpers = [idx for idx, chunk_num in enumerate(row) if idx != main_idx and chunk_num > 0]
        rng.shuffle(helpers)

        for helper_idx in helpers:
            if covered_chunks >= ec_k:
                break
            chunk_num = int(row[helper_idx])
            selected.append((helper_idx + 1, chunk_num))
            covered_chunks += chunk_num

        if covered_chunks < ec_k:
            raise ValueError(
                f"Stripe {stripe_idx} has only {covered_chunks} available chunks, "
                f"less than ec_k={ec_k}; row={list(row)}, main_cluster={main_idx + 1}"
            )

        other_help_pairs.append(selected)

    return main_help_cluster_id, other_help_pairs


def save_cluster_data(
    filename: str | Path,
    main_help_cluster_id: Sequence[int],
    other_help_cluster_id_chunk_num_pairs: Sequence[Sequence[Tuple[int, int]]],
) -> None:
    """Write cluster_data.bin in Coordinator::loadRepairData-compatible format."""
    path = Path(filename)
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("wb") as f:
        f.write(struct.pack("Q", len(main_help_cluster_id)))
        for cluster_id in main_help_cluster_id:
            f.write(struct.pack("i", int(cluster_id)))

        f.write(struct.pack("Q", len(other_help_cluster_id_chunk_num_pairs)))
        for inner_vec in other_help_cluster_id_chunk_num_pairs:
            f.write(struct.pack("Q", len(inner_vec)))
            for cluster_id, chunk_num in inner_vec:
                f.write(struct.pack("ii", int(cluster_id), int(chunk_num)))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a random repair plan from Available_matrix")
    parser.add_argument("--available", default=str(DEFAULT_AVAILABLE_MATRIX), help="Available_matrix path")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), help="cluster_data.bin output path")
    parser.add_argument("--ec-k", type=int, default=10, help="Number of chunks required to repair a stripe")
    parser.add_argument("--seed", type=int, default=None, help="Optional deterministic random seed")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    rng = random.Random(args.seed)

    available = read_available_matrix(args.available)
    main_help_cluster_id, other_pairs = build_random_repair_plan(available, args.ec_k, rng)
    save_cluster_data(args.output, main_help_cluster_id, other_pairs)

    print(f"Successfully read Available matrix: {len(available)} rows x {len(available[0])} columns")
    print(f"Generated RR plan for {len(main_help_cluster_id)} failed stripes")
    print(f"Saved cluster data to: {args.output}")


if __name__ == "__main__":
    main()
