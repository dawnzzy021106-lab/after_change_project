#include "lrc.h"

using namespace ECProject;

void LocallyRepairableCode::init_coding_parameters(CodingParameters cp)
{
  k = cp.k;
  l = cp.l;
  g = cp.g;
  m = l + g;
  local_or_column = cp.local_or_column;
}

void LocallyRepairableCode::get_coding_parameters(CodingParameters& cp)
{
  cp.k = k;
  cp.l = l;
  cp.g = g;
  cp.m = m;
  cp.local_or_column = local_or_column;
}

void LocallyRepairableCode::make_full_matrix(int *matrix, int kk)
{
  std::vector<int> encoding_matrix(m * k, 0);
	make_encoding_matrix(encoding_matrix.data());
	for (int i = 0; i < g; i++) {
		memcpy(&matrix[i * kk], &encoding_matrix[i * k], k * sizeof(int));
	}
  for (int i = 0; i < l; i++) {
    int min_idx = 0;
    int group_size = get_group_size(i, min_idx);
    std::vector<int> group_matrix(1 * group_size, 0);
    make_group_matrix(group_matrix.data(), i);
    memcpy(&matrix[(g + i) * kk + min_idx], group_matrix.data(),
        group_size * sizeof(int));
  }
}

void LocallyRepairableCode::encode(char **data_ptrs, char **coding_ptrs,
                                   int block_size)
{
  std::vector<int> lrc_matrix((g + l) * k, 0);
  make_encoding_matrix(lrc_matrix.data());
  // print_matrix(lrc_matrix.data(), g + l, k, "encode_matrix");
  jerasure_matrix_encode(k, g + l, w, lrc_matrix.data(), data_ptrs,
                         coding_ptrs, block_size);
}

void LocallyRepairableCode::decode(char **data_ptrs, char **coding_ptrs,
                                   int block_size, int *erasures, int failed_num)
{
  if (local_or_column) {
    int group_id = erasures[failed_num];
    erasures[failed_num] = -1;
    decode_local(data_ptrs, coding_ptrs, block_size, erasures, failed_num, group_id);
  } else {
    decode_global(data_ptrs, coding_ptrs, block_size, erasures, failed_num);
  }
}

void LocallyRepairableCode::decode_global(char **data_ptrs, char **coding_ptrs,
        int block_size, int *erasures, int failed_num)
{
  std::vector<int> lrc_matrix((g + l) * k, 0);
  make_encoding_matrix(lrc_matrix.data());
  int ret = 0;
  ret = jerasure_matrix_decode(k, g + l, w, lrc_matrix.data(), failed_num,
                               erasures, data_ptrs, coding_ptrs, block_size);
  if (ret == -1) {
    std::cout << "[Decode] Failed!" << std::endl;
    return;
  }
}

void LocallyRepairableCode::decode_local(char **data_ptrs, char **coding_ptrs,
        int block_size, int *erasures, int failed_num, int group_id)
{
  int min_idx = 0;
  int group_size = get_group_size(group_id, min_idx);
  std::vector<int> group_matrix(1 * group_size, 0);
  make_group_matrix(group_matrix.data(), group_id);
  int ret = 0;
  ret = jerasure_matrix_decode(group_size, 1, w, group_matrix.data(), failed_num,
                               erasures, data_ptrs, coding_ptrs, block_size);
  if (ret == -1) {
    std::cout << "[Decode] Failed!" << std::endl;
    return;
  }
}

void LocallyRepairableCode::encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme)
{
  if (partial_scheme) {
    if (local_or_column) {
      encode_partial_blocks_local(data_ptrs, coding_ptrs, block_size, data_idxs,
                                  parity_idxs, failure_idxs);
    } else {
      if (parity_idxs.size() == failure_idxs.size()) {
        std::vector<int> lrc_matrix((k + g + l) * (k + g + l), 0);
        get_identity_matrix(lrc_matrix.data(), k + g + l, k + g + l);
        make_full_matrix(&(lrc_matrix.data())[k * (k + g + l)], k + g + l);
        // print_matrix(lrc_matrix.data(), k + g + l, k + g + l, "full_matrix");
        encode_partial_blocks_for_failures_(k + g + l, lrc_matrix.data(), data_ptrs,
                                            coding_ptrs, block_size, data_idxs,
                                            parity_idxs, failure_idxs);
      } else {
        std::vector<int> lrc_matrix((k + g + l) * k, 0);
        get_identity_matrix(lrc_matrix.data(), k, k);
        make_encoding_matrix(&(lrc_matrix.data())[k * k]);
        encode_partial_blocks_for_failures_v2_(k, lrc_matrix.data(), data_ptrs,
                                               coding_ptrs, block_size, data_idxs,
                                               failure_idxs, live_idxs);
      }
    }
  } else {
    int parity_num = (int)parity_idxs.size();
    for (int i = 0; i < parity_num; i++) {
      my_assert(parity_idxs[i] >= k);
      bool flag = false;
      if (parity_idxs[i] < k + g) {
        for (auto idx : data_idxs) {
          if (idx < k || idx == parity_idxs[i]) {
            flag = true;
            break;
          }
        }
      } else {
        int gid = bid2gid(parity_idxs[i]);
        for (auto idx : data_idxs) {
          int t_gid = bid2gid(idx);
          if (t_gid == gid || idx == parity_idxs[i]) {
            flag = true;
            break;
          }
        }
      }
      partial_flags[i] = flag;
    }
    std::vector<int> lrc_matrix((k + g + l) * (k + g + l), 0);
    get_identity_matrix(lrc_matrix.data(), k + g + l, k + g + l);
    make_full_matrix(&(lrc_matrix.data())[k * (k + g + l)], k + g + l);
    // print_matrix(lrc_matrix.data(), k + g + l, k + g + l, "full_matrix");
    encode_partial_blocks_for_parities_(
        k + g + l, lrc_matrix.data(), data_ptrs, coding_ptrs,
        block_size, data_idxs, parity_idxs);
  }
}

void LocallyRepairableCode::encode_partial_blocks_local(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs)
{
  my_assert((int)parity_idxs.size() == 1);
  my_assert(parity_idxs[0] >= k + g);
  int group_id = parity_idxs[0] - k - g;
  int min_idx = 0;
  int group_size = get_group_size(group_id, min_idx);

  parity_idxs[0] = group_size;
  for (size_t i = 0; i < failure_idxs.size(); ++i) {
    int idx = failure_idxs[i];
    if (idx >= k + g)
        failure_idxs[i] = group_size;
    else
        failure_idxs[i] = idx - min_idx;
  }
  for (size_t i = 0; i < data_idxs.size(); ++i) {
    int idx = data_idxs[i];
    if (idx >= k + g)
        data_idxs[i] = group_size;
    else
        data_idxs[i] = idx - min_idx;
  }
  std::vector<int> group_matrix((group_size + 1) * (group_size + 1), 0);
  get_identity_matrix(group_matrix.data(), group_size + 1, group_size + 1);
  make_group_matrix(&(group_matrix.data())[group_size * (group_size + 1)], group_id);
  encode_partial_blocks_for_failures_(group_size + 1, group_matrix.data(),
                                      data_ptrs, coding_ptrs, block_size,
                                      data_idxs, parity_idxs, failure_idxs);
}

void LocallyRepairableCode::decode_with_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> failure_idxs, std::vector<int> parity_idxs)
{

  for (auto idx : parity_idxs) {
		my_assert(idx >= k);
	}
  std::vector<int> lrc_matrix((k + g + l) * (k + g + l), 0);
  get_identity_matrix(lrc_matrix.data(), k + g + l, k + g + l);
  make_full_matrix(&(lrc_matrix.data())[k * (k + g + l)], k + g + l);
  // print_matrix(lrc_matrix.data(), k + g + l, k + g + l, "full_matrix");
  decode_with_partial_blocks_(k + g + l, lrc_matrix.data(), data_ptrs, coding_ptrs,
															block_size, failure_idxs, parity_idxs);
}

int LocallyRepairableCode::num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs)
{
  int cnt = 0;
  int parity_num = (int)parity_idxs.size();
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_idxs[i] >= k);
    bool flag = false;
    if (parity_idxs[i] < k + g) {
      for (auto idx : data_idxs) {
        if (idx < k || idx == parity_idxs[i]) {
          flag = true;
          break;
        }
      }
    } else {
      int gid = bid2gid(parity_idxs[i]);
      for (auto idx : data_idxs) {
        int t_gid = bid2gid(idx);
        if (t_gid == gid || idx == parity_idxs[i]) {
          flag = true;
          break;
        }
      }
    }
    if (flag) {
      cnt++;
    }
  }
  return cnt;
}

void LocallyRepairableCode::partition_random()
{
  int n = k + l + g;
	std::vector<int> blocks;
  for(int i = 0; i < n; i++) {
    blocks.push_back(i);
  }

	int cnt = 0;
  while(cnt < n) {
		// at least subject to single-region fault tolerance
    int random_partition_size = random_range(1, g + 1);  
    int partition_size = std::min(random_partition_size, n - cnt);
    std::vector<int> partition;
    for(int i = 0; i < partition_size; i++, cnt++) {
      int ran_idx = random_index(n - cnt);
      int block_idx = blocks[ran_idx];
      partition.push_back(block_idx);
      auto it = std::find(blocks.begin(), blocks.end(), block_idx);
      blocks.erase(it);
    }
    partition_plan.push_back(partition);
  }
}

void LocallyRepairableCode::partition_optimal()
{
  std::vector<std::vector<int>> stripe_groups;
  grouping_information(stripe_groups);

  std::vector<std::vector<int>> remaining_groups;
  for (int i = 0; i < l; i++) { // partition blocks in every local group
    std::vector<int> local_group = stripe_groups[i];
    int group_size = int(local_group.size());
    // every g + 1 blocks a partition
    for (int j = 0; j < group_size; j += g + 1) {
      if (j + g + 1 > group_size) { // derive the remain group
        std::vector<int> remain_group;
        for (int ii = j; ii < group_size; ii++) {
          remain_group.push_back(local_group[ii]);
        }
        remaining_groups.push_back(remain_group);
        break;
      }
      std::vector<int> partition;
      for (int ii = j; ii < j + g + 1; ii++) {
        partition.push_back(local_group[ii]);
      }
      partition_plan.push_back(partition);
    }
  }

  int theta = l;
  if ((r + 1) % (g + 1) > 1) {
    theta = g / ((r + 1) % (g + 1) - 1);
  }
  int remaining_groups_num = int(remaining_groups.size());
  for (int i = 0; i < remaining_groups_num; i += theta) { // organize every θ remaining groups
    std::vector<int> partition;
    for (int j = i; j < i + theta && j < remaining_groups_num; j++) {
      std::vector<int> remain_group = remaining_groups[j];
      int remain_block_num = int(remain_group.size());
      for (int ii = 0; ii < remain_block_num; ii++) {
        partition.push_back(remain_group[ii]);
      }
    }
    partition_plan.push_back(partition);
  }

  // organize the global parity blocks in a single partition
  if (r % (g + 1) == 0 && remaining_groups_num) {
    int idx = (int)partition_plan.size() - 1;
    for (int i = k; i < k + g; i++) {
      partition_plan[idx].push_back(i);
    }
  } else {
    std::vector<int> partition;
    for (int i = k; i < k + g; i++) {
      partition.push_back(i);
    }
    partition_plan.push_back(partition);
  }
}

void LocallyRepairableCode::help_blocks_for_single_block_repair(
        int failure_idx,
        std::vector<int>& parity_idxs,
        std::vector<std::vector<int>>& help_blocks,
        bool partial_scheme)
{
	int parition_num = (int)partition_plan.size();
  if (!parition_num) {
    return;
  }

  if (local_or_column) {
    if (failure_idx >= k + g) {
      parity_idxs.push_back(failure_idx);
    }
    int gid = bid2gid(failure_idx);
    for (int i = 0; i < parition_num; i++) {
      std::vector<int> help_block;
      int cnt = 0;
      for (auto bid : partition_plan[i]) {
        if (bid2gid(bid) == gid && bid != failure_idx) {
          help_block.push_back(bid);
          cnt++;
          if (bid >= k + g) {
            parity_idxs.push_back(bid);
          }
        }
      }
      if (cnt > 0) {
        help_blocks.push_back(help_block);
      }
    }
  } else {
    if (failure_idx >= k) {
      parity_idxs.push_back(failure_idx);
    }
    if (partial_scheme) {
      int main_par_idx = 0;
      std::vector<std::pair<int, int>> partition_idx_to_num;
      for (int i = 0; i < parition_num; i++) {
        int cnt = 0;
        for (auto bid : partition_plan[i]) {
          if (bid < k + g && bid != failure_idx) {
            cnt++;
          }
          if (bid == failure_idx) {
            main_par_idx = i;
            cnt = 0;
            break;
          }
        }
        if (cnt > 0) {
          partition_idx_to_num.push_back(std::make_pair(i, cnt));
        }
      }
      std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
                cmp_descending);
      
      int cnt = 0;
      std::vector<int> main_help_block;
      for (auto idx : partition_plan[main_par_idx]) {
        if (idx != failure_idx && idx < k + g) {
          if (cnt < k) {
            main_help_block.push_back(idx);
            cnt++;
            if (idx >= k) {
              parity_idxs.push_back(idx);
            }
          } else {
            break;
          }
        }
      }
      if (cnt > 0) {
        help_blocks.push_back(main_help_block);
      }
      if (cnt == k) {
        return;
      }
      for (auto& pair : partition_idx_to_num) {
        std::vector<int> help_block;
        for (auto idx : partition_plan[pair.first]) {
          if (idx < k + g) {
            if (cnt < k) {
              help_block.push_back(idx);
              cnt++;
              if (idx >= k) {
                parity_idxs.push_back(idx);
              }
            } else {
              break;
            }
          }
        }
        if (cnt > 0 && cnt <= k) {
          help_blocks.push_back(help_block);
        }
        if (cnt == k) {
          return;
        }
      }
    } else {
      // repair global parity with data blocks by encoding
      for (auto partition : partition_plan) {
        std::vector<int> help_block;
        for (auto bid : partition) {
          if (bid < k) {
            help_block.push_back(bid);
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
    }
  }
}

void LocallyRepairableCode::help_blocks_for_multi_global_blocks_repair(
				std::vector<int> failure_idxs,
        std::vector<int>& parity_idxs,
        std::vector<std::vector<int>>& help_blocks,
        bool partial_scheme)
{
  int parition_num = (int)partition_plan.size();
  my_assert(parition_num > 0);
  int failed_num = (int)failure_idxs.size();
  for (auto idx : failure_idxs) {
    my_assert(idx >= k && idx < k + g);
    parity_idxs.push_back(idx);
  }
  if (partial_scheme) {
    std::vector<std::vector<int>> copy_partition;
    for (int i = 0; i < parition_num; i++) {
      std::vector<int> partition;
      for (auto idx : partition_plan[i]) {
        partition.push_back(idx);
      }
      copy_partition.push_back(partition);
    }

    int failures_cnt[parition_num] = {0};
    for (auto failure_idx : failure_idxs) {
      for (int i = 0; i < parition_num; i++) {
        auto it = std::find(copy_partition[i].begin(), copy_partition[i].end(),
                            failure_idx);
        if (it != copy_partition[i].end()) {
          failures_cnt[i]++;
          copy_partition[i].erase(it);	// remove the failures
          break;
        }
      }
    }
    for (int l_ = k + g; l_ < k + g + l; l_++) {
      for (int i = 0; i < parition_num; i++) {
        auto it = std::find(copy_partition[i].begin(), copy_partition[i].end(), l_);
        if (it != copy_partition[i].end()) {
          copy_partition[i].erase(it);	// remove local parity blocks
          break;
        }
      }
    }
    std::vector<std::pair<int, int>> main_partition_idx_to_num;
    std::vector<std::pair<int, int>> partition_idx_to_num;
    for (int i = 0; i < parition_num; i++) {
      int partition_size = (int)copy_partition[i].size();
      if (failures_cnt[i]) {
        main_partition_idx_to_num.push_back(std::make_pair(i, partition_size));
      } else {
        partition_idx_to_num.push_back(std::make_pair(i, partition_size));
      }
    }
    std::sort(main_partition_idx_to_num.begin(), main_partition_idx_to_num.end(),
              cmp_descending);
    std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
              cmp_descending);

    int cnt = 0;
    for (auto& pair : main_partition_idx_to_num) {
      std::vector<int> main_help_block;
      for (auto idx : copy_partition[pair.first]) {
        if (cnt < k) {
          main_help_block.push_back(idx);
          cnt++;
          if (idx >= k) {
            parity_idxs.push_back(idx);
          }
        } else {
          break;
        }
      }
      if (cnt > 0 && cnt <= k && main_help_block.size() > 0) {
        help_blocks.push_back(main_help_block);
      }
      if (cnt == k) {
        return;
      }
    }
    for (auto& pair : partition_idx_to_num) {
      std::vector<int> help_block;
      for (auto idx : copy_partition[pair.first]) {
        if (cnt < k) {
          help_block.push_back(idx);
          cnt++;
          if (idx >= k) {
            parity_idxs.push_back(idx);
          }
        } else {
          break;
        }
      }
      if (cnt > 0 && cnt <= k && help_block.size() > 0) {
          help_blocks.push_back(help_block);
      } 
      if (cnt == k) {
        return;
      }
    }
  } else {
    // repair global parity with data blocks by encoding
    for (auto partition : partition_plan) {
      std::vector<int> help_block;
      for (auto bid : partition) {
        if (bid < k) {
          help_block.push_back(bid);
        }
      }
      if (help_block.size() > 0) {
        help_blocks.push_back(help_block);
      }
    }
  }
}

void LocallyRepairableCode::repair_multi_data_blocks_with_global_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  for (auto idx : failure_idxs) { // only focus on data failures
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        help_blocks_map[k + g + i] = 1;
        group_fd_cnt[i] -= 1;
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
        }
        global_idx++;
      }
      
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
    }
  }
  for (int i = k; i < k + g; i++) {
    if (help_blocks_map[i]) {
      parity_idxs.push_back(i);
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void LocallyRepairableCode::repair_multi_data_blocks_with_local_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  for (auto idx : failure_idxs) { // only focus on data failures
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        help_blocks_map[k + g + i] = 1;
        group_fd_cnt[i] -= 1;
        parity_idxs.push_back(k + g + i);
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void LocallyRepairableCode::repair_multi_blocks_with_global_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  for (int i = 0; i < l; i++) { // focus on data failures
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        help_blocks_map[k + g + i] = 1;
        group_fd_cnt[i] -= 1;
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          parity_idxs.push_back(global_idx);
          group_fd_cnt[i] -= 1;
        }
        global_idx++;
      }
      
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      parity_idxs.push_back(i);
      if (i < k + g) {
        use_global_flag = true;
      } else {
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          use_global_flag = true;
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
    }
  }

  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void LocallyRepairableCode::repair_multi_blocks_with_local_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) { // focus on data failures
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        help_blocks_map[k + g + i] = 1;
        group_fd_cnt[i] -= 1;
        parity_idxs.push_back(k + g + i);
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      parity_idxs.push_back(i);
      if (i < k + g) {
        use_global_flag = true;
      } else {
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

bool LocallyRepairableCode::generate_repair_plan(
        std::vector<int> failure_idxs, std::vector<RepairPlan>& plans,
				bool partial_scheme, bool repair_priority, bool repair_method)
{
  bool decodable = check_if_decodable(failure_idxs);
  if (!decodable) {
    return false;
  }
	int failed_num = (int)failure_idxs.size();
	if (failed_num == 1) {
    RepairPlan plan;
    for (auto idx : failure_idxs) {
      plan.failure_idxs.push_back(idx);
    }
    int group_id = bid2gid(failure_idxs[0]);
    if (group_id < l) {
      local_or_column = true;
      plan.local_or_column = true;
    } else {
      local_or_column = false;
      plan.local_or_column = false;
    }
		help_blocks_for_single_block_repair(failure_idxs[0], plan.parity_idxs,
        plan.help_blocks, partial_scheme);
    plans.push_back(plan);
	} else {
    if (repair_method) {
      return repair_multi_blocks_method2(failure_idxs, plans, partial_scheme, repair_priority);
    } else {
      return repair_multi_blocks_method1(failure_idxs, plans, partial_scheme, repair_priority);
    }
  }
  return true;
}

bool LocallyRepairableCode::generate_flow_repair_plan(std::vector<int> failure_idxs,
															std::vector<RepairPlan>& plans,
															bool partial_scheme,
															bool repair_priority,
															bool repair_method,
                                      						std::vector<int> main_help_clusterID,
                	                  						std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
															int failure_stripe_ID,
															bool main_help_cluster_flag)
{
  bool decodable = check_if_decodable(failure_idxs);
  if (!decodable) {
    return false;
  }
	int failed_num = (int)failure_idxs.size();
	if (failed_num == 1) {
    RepairPlan plan;
    for (auto idx : failure_idxs) {
      plan.failure_idxs.push_back(idx);
    }
    int group_id = bid2gid(failure_idxs[0]);
    if (group_id < l) {
      local_or_column = true;
      plan.local_or_column = true;
    } else {
      local_or_column = false;
      plan.local_or_column = false;
    }
		help_blocks_for_single_block_repair(failure_idxs[0], plan.parity_idxs,
        plan.help_blocks, partial_scheme);
    plans.push_back(plan);
	} else {
    if (repair_method) {
      return repair_multi_blocks_method2(failure_idxs, plans, partial_scheme, repair_priority);
    } else {
      return repair_multi_blocks_method1(failure_idxs, plans, partial_scheme, repair_priority);
    }
  }
  return true;
}

bool LocallyRepairableCode::repair_multi_blocks_method1(
        std::vector<int> failure_idxs, std::vector<RepairPlan>& plans,
        bool partial_scheme, bool repair_priority)
{
  std::vector<int> failed_map(k + g + l, 0);
  std::vector<int> fb_group_cnt(l + 1, 0);
  int num_of_failed_blocks = int(failure_idxs.size());
  for (int i = 0; i < num_of_failed_blocks; i++) {
    int failed_idx = failure_idxs[i];
    failed_map[failed_idx] = 1;
    fb_group_cnt[bid2gid(failed_idx)] += 1;
  }

  int iter_cnt = 0;
  while (num_of_failed_blocks > 0) {
    for (int group_id = 0; group_id < l; group_id++) {
      // local repair
      if (fb_group_cnt[group_id] == 1) {
        int failed_idx = -1;
        for (int i = 0; i < k + g + l; i++) {
          if (failed_map[i] && bid2gid(i) == group_id) {
            failed_idx = i;
            break;
          }
        }
        RepairPlan plan;
        plan.local_or_column = true;
        plan.failure_idxs.push_back(failed_idx);
        local_or_column = true;
        help_blocks_for_single_block_repair(failed_idx, plan.parity_idxs,
            plan.help_blocks, partial_scheme);
        plans.push_back(plan);
        // update
        failed_map[failed_idx] = 0;
        fb_group_cnt[group_id] = 0;
        num_of_failed_blocks -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
            failure_idxs.end(), failed_idx));
      }
    }
      
    if (num_of_failed_blocks > 0) {
      RepairPlan plan;
      plan.local_or_column = false;
      int fg_cnt = 0;
      for (int i = 0; i < k + g + l; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
          if (i >= k && i < k + g) {
            fg_cnt++;
          }
        }
      }

      if (fg_cnt == num_of_failed_blocks) {
        if (fg_cnt == 1) {
          if (!repair_priority && bid2gid(k) == l - 1) {
            local_or_column = true;
          } else {
            local_or_column = false;
          }
          help_blocks_for_single_block_repair(plan.failure_idxs[0],
            plan.parity_idxs, plan.help_blocks, partial_scheme);
        } else if (!repair_priority && bid2gid(k) == l - 1) {
          repair_multi_blocks_with_local_priority(failure_idxs,
              plan.parity_idxs, plan.help_blocks);
        } else {
          help_blocks_for_multi_global_blocks_repair(plan.failure_idxs,
              plan.parity_idxs, plan.help_blocks, partial_scheme);
        }
      } else {
        if (repair_priority) {
          repair_multi_blocks_with_global_priority(failure_idxs,
              plan.parity_idxs, plan.help_blocks);
        } else {
          repair_multi_blocks_with_local_priority(failure_idxs,
              plan.parity_idxs, plan.help_blocks);
        }
      }
      if (plan.help_blocks.size() == 0) {
        std::cout << "Undecodable!!!" << std::endl;
        return false;
      }
      plans.push_back(plan);

      // update 
      for (int i = 0; i < k + g + l; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
    }
  }
  return true;
}

bool LocallyRepairableCode::repair_multi_blocks_method2(
        std::vector<int> failure_idxs, std::vector<RepairPlan>& plans,
        bool partial_scheme, bool repair_priority)
{
  std::vector<int> failed_map(k + g + l, 0);
  std::vector<int> fb_group_cnt(l + 1, 0);
  int num_of_failed_data = 0;
  int num_of_failed_blocks = int(failure_idxs.size());
  for (int i = 0; i < num_of_failed_blocks; i++) {
    int failed_idx = failure_idxs[i];
    failed_map[failed_idx] = 1;
    fb_group_cnt[bid2gid(failed_idx)] += 1;
    if (failed_idx < k) {
      num_of_failed_data += 1;
    }
  }

  int iter_cnt = 0;
  while (num_of_failed_blocks > 0) {
    for (int group_id = 0; group_id < l; group_id++) {
      // local repair
      if (fb_group_cnt[group_id] == 1) {
        int failed_idx = -1;
        for (int i = 0; i < k + g + l; i++) {
          if (failed_map[i] && bid2gid(i) == group_id) {
            failed_idx = i;
            break;
          }
        }
        RepairPlan plan;
        plan.local_or_column = true;
        plan.failure_idxs.push_back(failed_idx);
        local_or_column = true;
        help_blocks_for_single_block_repair(failed_idx, plan.parity_idxs,
            plan.help_blocks, partial_scheme);
        plans.push_back(plan);
        // update
        failed_map[failed_idx] = 0;
        fb_group_cnt[group_id] = 0;
        num_of_failed_blocks -= 1;
        if (failed_idx < k) {
          num_of_failed_data -= 1;
        }
        failure_idxs.erase(std::find(failure_idxs.begin(),
            failure_idxs.end(), failed_idx));
      }
    }
      
    if (num_of_failed_data > 0) {
      RepairPlan plan;
      plan.local_or_column = false;

      for (int i = 0; i < k; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
        }
      }
        
      if (repair_priority) {
        repair_multi_data_blocks_with_global_priority(failure_idxs,
            plan.parity_idxs, plan.help_blocks);
      } else {
        repair_multi_data_blocks_with_local_priority(failure_idxs,
            plan.parity_idxs, plan.help_blocks);
      }
      if (plan.help_blocks.size() == 0) {
        std::cout << "Undecodable!!!" << std::endl;
        return false;
      }
      plans.push_back(plan);

      // update 
      for (int i = 0; i < k; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
      num_of_failed_data = 0;
    }

    if (num_of_failed_blocks == 0) {
      break;
    }

    if (iter_cnt > 0 && num_of_failed_blocks > 0) {
      // global parity block repair 
      RepairPlan plan;
      plan.local_or_column = false;
      for (int i = k; i < k + g; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
        }
      }
      int failed_parity_num = (int)plan.failure_idxs.size();
      if (failed_parity_num == 1) {
        if (num_of_failed_blocks == failed_parity_num && bid2gid(k) == l - 1) {
          local_or_column = true;
        } else {
          local_or_column = false;
        }
        help_blocks_for_single_block_repair(plan.failure_idxs[0],
            plan.parity_idxs, plan.help_blocks, partial_scheme);
      } else if (!repair_priority && bid2gid(k) == l - 1) {
        repair_multi_blocks_with_local_priority(failure_idxs,
            plan.parity_idxs, plan.help_blocks);
      } else {
        help_blocks_for_multi_global_blocks_repair(plan.failure_idxs,
            plan.parity_idxs, plan.help_blocks, partial_scheme);
      }
      plans.push_back(plan); 
        
      // update
      for (int i = k; i < k + g; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
    }
    iter_cnt++;
    if (iter_cnt > 2) {
      std::cout << "generate repair plan error!" << std::endl;
      return false;
    }
  }
  return true;
}

bool Azu_LRC::check_if_decodable(std::vector<int> failure_idxs)
{
  int failed_num = (int)failure_idxs.size();
  std::unordered_map<int, int> b2g;
  std::vector<int> group_fd_cnt;
  std::vector<int> group_slp_cnt;
  int sgp_cnt = g;
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      b2g.insert(std::make_pair(idx, i));
      idx++;
    }
    b2g.insert(std::make_pair(k + g + i, i));
    group_fd_cnt.push_back(0);
    group_slp_cnt.push_back(1);
  }

  for (int i = 0; i < failed_num; i++) {
    int block_id = failure_idxs[i];
    if (block_id < k) {
      group_fd_cnt[b2g[block_id]] += 1;
    } else if (block_id < k + g && block_id >= k) {
      sgp_cnt -= 1;
    } else {
      group_slp_cnt[block_id - k - g] -= 1;
    }
  }
  for (int i = 0; i < l; i++) {
    if (group_slp_cnt[i] && group_slp_cnt[i] <= group_fd_cnt[i]) {
      group_fd_cnt[i] -= group_slp_cnt[i];
      group_slp_cnt[i] = 0;
    }
  }
  for (int i = 0; i < l; i++) {
    if (sgp_cnt >= group_fd_cnt[i]) {
      sgp_cnt -= group_fd_cnt[i];
      group_fd_cnt[i] = 0;
    } else {
      return false;
    }
  }
  return true;
}

void Azu_LRC::make_encoding_matrix(int *final_matrix)
{
  int *matrix = reed_sol_vandermonde_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++)
    {
      final_matrix[i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      if (i * r <= j && j < (i + 1) * r) {
        final_matrix[(i + g) * k + j] = 1;
      }
    }
  }

  free(matrix);
}

void Azu_LRC::make_group_matrix(int *group_matrix, int group_id)
{
  for (int i = 0; i < l; i++) {
    if (i == group_id) {
      int group_size = std::min(r, k - i * r);
      for(int j = 0; j < group_size; j++) {
        group_matrix[j] = 1;
      }
    }
  }
}

bool Azu_LRC::check_parameters()
{
  if (r * (l - 1) < k || !k || !l || !g)
    return false;
  return true;
}

int Azu_LRC::bid2gid(int block_id)
{
  int group_id = -1;
  if (block_id < k) {
    group_id = block_id / r;
  } else if (block_id >= k && block_id < k + g) {
    group_id = l; 
  } else {
    group_id = block_id - k - g;
  }
  return group_id;
}

int Azu_LRC::idxingroup(int block_id)
{
  if (block_id < k) {
    return block_id % r;
  } else if (block_id >= k && block_id < k + g) {
    return block_id - k;
  } else {
    if (block_id - k - g < l - 1) {
      return r;
    } else {
      return k % r == 0 ? r : k % r;
    }
  }
}

int Azu_LRC::get_group_size(int group_id, int& min_idx)
{
  min_idx = group_id * r;
  if (group_id < l - 1) {
    return r;
  } else if (group_id == l - 1) {
    return k % r == 0 ? r : k % r;
  } else {
    min_idx = k;
    return g;
  }
}

void Azu_LRC::grouping_information(std::vector<std::vector<int>>& groups)
{
  int idx = 0;
  for (int i = 0; i < l; i++) {
    std::vector<int> local_group;
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      local_group.push_back(idx++);
    }
    local_group.push_back(k + g + i);
    groups.push_back(local_group);
  }
  std::vector<int> local_group;
  for (int i = 0; i < g; i++) {
    local_group.push_back(idx++);
  }
  groups.push_back(local_group);
}

void Azu_LRC::partition_optimal_v2()
{
  std::vector<std::vector<int>> stripe_groups;
  grouping_information(stripe_groups);
            
  std::vector<std::vector<int>> remaining_groups;
  for (int i = 0; i < l; i++)  {  // partition blocks in every local group
    std::vector<int> local_group = stripe_groups[i];
    int group_size = int(local_group.size());
    // every g + 1 blocks a partition
    for (int j = 0; j < group_size; j += g + 1) {
      if (j + g + 1 > group_size) { // derive the remain group
        std::vector<int> remain_group;
        for (int ii = j; ii < group_size; ii++) {
          remain_group.push_back(local_group[ii]);
        }
        remaining_groups.push_back(remain_group);
        break;
      }
      std::vector<int> partition;
      for (int ii = j; ii < j + g + 1; ii++) {
        partition.push_back(local_group[ii]);
      }
      partition_plan.push_back(partition);
    }
  }
  int theta = l;
  if ((r + 1) % (g + 1) > 1) {
    theta = g / ((r + 1) % (g + 1) - 1);
  }
  int remaining_groups_num = int(remaining_groups.size());
  for (int i = 0; i < remaining_groups_num; i += theta) { // organize every θ remaining groups
    std::vector<int> partition;
    for (int j = i; j < i + theta && j < remaining_groups_num; j++) {
      std::vector<int> remain_group = remaining_groups[j];
      int remain_block_num = int(remain_group.size());
      for (int ii = 0; ii < remain_block_num; ii++) {
        partition.push_back(remain_group[ii]);
      }
    }
    partition_plan.push_back(partition);
  }
  // calculate free space
  std::vector<std::pair<int, int>> space_left_in_each_partition;
  int sum_left_space = 0;
  for (int i = 0; i < (int)partition_plan.size(); i++) {
    int num_of_blocks = (int)partition_plan[i].size();
    int num_of_group = 0;
    for (int j = 0; j < num_of_blocks; j++) {
      if (partition_plan[i][j] >= k + g) {
        num_of_group += 1;
      }
    }
    if (num_of_group == 0) {
      num_of_group = 1;
    }
    int max_space = g + num_of_group;
    int left_space = max_space - num_of_blocks;
    space_left_in_each_partition.push_back({i, left_space});
    sum_left_space += left_space;
  }
  // place the global parity blocks
  int left_g = g;
  int global_idx = k;
  if (sum_left_space >= g) {  // insert to partitions with free space
    std::sort(space_left_in_each_partition.begin(), space_left_in_each_partition.end(), cmp_descending);
    for (auto i = 0; i < space_left_in_each_partition.size() && left_g > 0; i++) {
      if (space_left_in_each_partition[i].second > 0) {
        int j = space_left_in_each_partition[i].first;
        int left_space = space_left_in_each_partition[i].second;
        if (left_g >= left_space) {
          left_g -= left_space;
        } else {
          left_space = left_g;
          left_g -= left_g;
        }
        while (left_space--) {
          partition_plan[j].push_back(global_idx++);
        }
      }
    }
    my_assert(left_g == 0);
  } else {  // a seperate new partition
    std::vector<int> partition;
    while (global_idx < k + g) {
      partition.push_back(global_idx++);
    }
    partition_plan.push_back(partition);
  }
}

std::string Azu_LRC::self_information()
{
	return "Azure_LRC(" + std::to_string(k) + "," + std::to_string(l) + \
         "," + std::to_string(g) + ")";
}
std::string Azu_LRC::type()
{
  return "Azure_LRC";
}

bool Azu_LRC_1::check_if_decodable(std::vector<int> failure_idxs)
{
  int failed_num = (int)failure_idxs.size();
  std::unordered_map<int, int> b2g;
  std::vector<int> group_fd_cnt;
  std::vector<int> group_slp_cnt;
  int sgp_cnt = g;
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      b2g.insert(std::make_pair(idx, i));
      idx++;
    }
    b2g.insert(std::make_pair(k + g + i, i));
    group_fd_cnt.push_back(0);
    group_slp_cnt.push_back(1);
  }

  for (int i = 0; i < failed_num; i++) {
    int block_id = failure_idxs[i];
    if (block_id < k) {
      group_fd_cnt[b2g[block_id]] += 1;
    } else if (block_id < k + g && block_id >= k) {
      sgp_cnt -= 1;
    } else {
      group_slp_cnt[block_id - k - g] -= 1;
    }
  }
  for (int i = 0; i < l; i++) {
    if (i < l - 1) {
      if (group_slp_cnt[i] && group_slp_cnt[i] <= group_fd_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      }
    } else {
      if (group_slp_cnt[i] && sgp_cnt == g - 1) {
        sgp_cnt += 1;
      }
    }
  }
  for (int i = 0; i < l; i++) {
    if (sgp_cnt >= group_fd_cnt[i]) {
      sgp_cnt -= group_fd_cnt[i];
      group_fd_cnt[i] = 0;
    } else {
      return false;
    }
  }
  return true;
}

void Azu_LRC_1::make_encoding_matrix(int *final_matrix)
{
  int *matrix = reed_sol_vandermonde_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  std::vector<int> l_matrix(l * (k + g), 0);
  std::vector<int> d_g_matrix((k + g) * k, 0);
  int idx = 0;
  for (int i = 0; i < l - 1; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      l_matrix[i * (k + g) + idx] = 1;
      idx++;
    }
  }
  for (int j = 0; j < g; j++) {
    l_matrix[(l - 1) * (k + g) + idx] = 1;
    idx++;
  }
  for (int i = 0; i < k; i++) {
    d_g_matrix[i * k + i] = 1;
  }
  idx = k * k;
  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      d_g_matrix[idx + i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  int *mix_matrix = jerasure_matrix_multiply(l_matrix.data(), d_g_matrix.data(),
                                             l, k + g, k + g, k, w);

  idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = mix_matrix[i * k + j];
    }
  }

  free(matrix);
  free(mix_matrix);
}

void Azu_LRC_1::make_group_matrix(int *group_matrix, int group_id)
{
  if (group_id == l - 1) {
    for (int j = 0; j < g; j++) {
      group_matrix[j] = 1;
    }
    return;
  }
  for (int i = 0; i < l - 1; i++) {
    if (i == group_id) {
      int group_size = std::min(r, k - i * r);
      for (int j = 0; j < group_size; j++) {
        group_matrix[j] = 1;
      }
    }
  }
}

bool Azu_LRC_1::check_parameters()
{
  if (r * (l - 2) < k || !k || !l || !g)
    return false;
  return true;
}

int Azu_LRC_1::bid2gid(int block_id)
{
  int group_id = -1;
  if (block_id < k) {
    group_id = block_id / r;
  } else if (block_id >= k && block_id < k + g) {
    group_id = l - 1; 
  } else {
    group_id = block_id - k - g;
  }
  return group_id;
}

int Azu_LRC_1::idxingroup(int block_id)
{
  if (block_id < k) {
    return block_id % r;
  } else if (block_id >= k && block_id < k + g) {
    return block_id - k;
  } else {
    if (block_id - k - g < l - 2) {
      return r;
    } else if(block_id - k - g == l - 2) {
      return k % r == 0 ? r : k % r;
    } else {
      return g;
    }
  }
}

int Azu_LRC_1::get_group_size(int group_id, int& min_idx)
{
  min_idx = group_id * r;
  if (group_id < l - 2) {
    return r;
  } else if (group_id == l - 2) {
    return k % r == 0 ? r : k % r;;
  } else {
    min_idx = k;
    return g;
  }
}

void Azu_LRC_1::grouping_information(std::vector<std::vector<int>>& groups)
{
  int idx = 0;
  for (int i = 0; i < l - 1; i++) {
    std::vector<int> local_group;
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      local_group.push_back(idx++);
    }
    local_group.push_back(k + g + i);
    groups.push_back(local_group);
  }
  std::vector<int> local_group;
  for (int i = 0; i < g; i++) {
    local_group.push_back(idx++);
  }
  local_group.push_back(k + g + l - 1);
  groups.push_back(local_group);
}

void Azu_LRC_1::partition_optimal()
{
  std::vector<std::vector<int>> stripe_groups;
  grouping_information(stripe_groups);

  for (int i = 0; i < l; i++) {
    std::vector<int> local_group = stripe_groups[i];
    int group_size = int(local_group.size());
    // every g + 1 blocks a partition
    for (int j = 0; j < group_size; j += g + 1) {
      std::vector<int> partition;
      for (int ii = j; ii < j + g + 1 && ii < group_size; ii++) {
        partition.push_back(local_group[ii]);
      }
      partition_plan.push_back(partition);
    }
  }
}

void Azu_LRC_1::repair_multi_blocks_with_global_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
    int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        parity_idxs.push_back(i);
        use_global_flag = true;
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          use_global_flag = true;
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      if (i == l - 1) {
        use_global_in_group = true;
      }
    }
  }

  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Azu_LRC_1::repair_multi_blocks_with_local_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          if (i == l - 1) {
            use_global_in_group = true;
          }
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  int cnt = 0;
  for (int i = k; i < k + g; i++) {
    if (failures_map[i]) {
      cnt++;
    }
  }
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        int gid = bid2gid(i);
        if (cnt == 1 && !failures_map[k + g + gid]) {
          use_global_in_group = true;
          for (auto idx : groups[gid]) {
            if (!failures_map[idx] && !help_blocks_map[idx]) {
              help_blocks_map[idx] = 1;
              if (idx >= k + g) {
                parity_idxs.push_back(idx);
              }
            }
          }
          cnt--;
        } else {
          parity_idxs.push_back(i);
          use_global_flag = true;
          cnt--;
        }
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

std::string Azu_LRC_1::self_information()
{
	return "Azure_LRC+1(" + std::to_string(k) + "," + std::to_string(l) + \
         "," + std::to_string(g) + ")";
}

std::string Azu_LRC_1::type()
{
  return "Azure_LRC_1";
}

bool Opt_LRC::check_if_decodable(std::vector<int> failure_idxs)
{
  int failed_num = (int)failure_idxs.size();
  std::unordered_map<int, int> b2g;
  std::vector<int> group_fd_cnt;
  std::vector<int> group_fgp_cnt;
  std::vector<int> group_slp_cnt;
  std::vector<bool> group_pure_flag;
  int sgp_cnt = g;
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      b2g.insert(std::make_pair(idx, i));
      idx++;
    }
    if (idx <= k || idx - group_size >= k) {
      group_pure_flag.push_back(true);
    } else {
      group_pure_flag.push_back(false);
    }
    b2g.insert(std::make_pair(k + g + i, i));
    group_fd_cnt.push_back(0);
    group_fgp_cnt.push_back(0);
    group_slp_cnt.push_back(1);
  }

  for (int i = 0; i < failed_num; i++) {
    int block_id = failure_idxs[i];
    if (block_id < k) {
      group_fd_cnt[b2g[block_id]] += 1;
    } else if (block_id < k + g && block_id >= k) {
      group_fgp_cnt[b2g[block_id]] += 1;
      sgp_cnt -= 1;
    } else {
      group_slp_cnt[block_id - k - g] -= 1;
    }
  }

  for (int i = 0; i < l; i++) {
    if (group_slp_cnt[i] && group_pure_flag[i]) {
      if (group_slp_cnt[i] <= group_fd_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      }
      if (group_slp_cnt[i] && group_slp_cnt[i] == group_fgp_cnt[i]) {
        group_fgp_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
        sgp_cnt += 1;
      }
    } else if (group_slp_cnt[i] && !group_pure_flag[i]) {
      if (group_fd_cnt[i] == 1 && !group_fgp_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      } else if (group_fgp_cnt[i] == 1 && !group_fd_cnt[i]) {
        group_fgp_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
        sgp_cnt += 1;
      }
    }
  }
  for (int i = 0; i < l; i++) {
    if (sgp_cnt >= group_fd_cnt[i]) {
      sgp_cnt -= group_fd_cnt[i];
      group_fd_cnt[i] = 0;
    } else {
      return false;
    }
  }
  return true;
}

void Opt_LRC::make_encoding_matrix(int *final_matrix)
{
  int *matrix = reed_sol_vandermonde_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  std::vector<int> l_matrix(l * (k + g), 0);
  std::vector<int> d_g_matrix((k + g) * k, 0);
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      l_matrix[i * (k + g) + idx] = 1;
      idx++;
    }
  }
  for (int i = 0; i < k; i++) {
    d_g_matrix[i * k + i] = 1;
  }
  idx = k * k;
  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      d_g_matrix[idx + i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  // print_matrix(l_matrix.data(), l, k + g, "l_matrix");
  // print_matrix(d_g_matrix.data(), k + g, k, "d_g_matrix");

  int *mix_matrix = jerasure_matrix_multiply(l_matrix.data(), d_g_matrix.data(),
                                             l, k + g, k + g, k, w);

  idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = mix_matrix[i * k + j];
    }
  }

  free(matrix);
  free(mix_matrix);
}

void Opt_LRC::make_encoding_matrix_v2(int *final_matrix)
{
  int *matrix = reed_sol_vandermonde_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[(i + 1) * k + j];
    }
  }

  std::vector<int> l_matrix(l * k, 0);
  int d_idx = 0;
  int l_idx = 0;
  for (int i = 0; i < l && d_idx < k; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size && d_idx < k; j++) {
      l_matrix[i * k + d_idx] = 1;
      d_idx++;
    }
    l_idx = i;
  }

  int g_idx = 0;
  for (int i = l_idx; i < l; i++) {
    int sub_g_num = -1;
    int group_size = std::min(r, k + g - i * r);
    if (group_size < r) {// must be the last group
      sub_g_num = g - g_idx;
    } else {
      sub_g_num = (i + 1) * r - (k + g_idx);
    }
    std::vector<int> sub_g_matrix(sub_g_num * k, 0);
    for (int j = 0; j < sub_g_num; j++) {
      for (int jj = 0; jj < k; jj++) {
        sub_g_matrix[j * k + jj] = matrix[(g_idx + 1) * k + jj];
      }
      g_idx++;
    }
    for (int j = 0; j < sub_g_num; j++) {
      galois_region_xor((char *)&sub_g_matrix[j * k], (char *)&l_matrix[i * k], 4 * k);
    }
  }

  int idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = l_matrix[i * k + j];
    }
  }
  free(matrix);
}

void Opt_LRC::make_group_matrix(int *group_matrix, int group_id)
{
  for (int i = 0; i < l; i++) {
    if (i == group_id) {
      int group_size = std::min(r, k + g - i * r);
      for (int j = 0; j < group_size; j++) {
        group_matrix[j] = 1;
      }
    }
  }
}

bool Opt_LRC::check_parameters()
{
  if (r * (l - 1) < k + g || !k || !l || !g)
    return false;
  return true;
}

int Opt_LRC::bid2gid(int block_id)
{
  int group_id = -1;
  if (block_id < k + g) {
    group_id = block_id / r;
  } else {
    group_id = block_id - k - g;
  }
  return group_id;
}

int Opt_LRC::idxingroup(int block_id)
{
  if (block_id < k + g) {
    return block_id % r;
  } else {
    if (block_id - k - g < l - 1) {
      return r;
    } else {
      return (k + g) % r == 0 ? r : (k + g) % r;
    }
  }
}

int Opt_LRC::get_group_size(int group_id, int& min_idx)
{
  min_idx = group_id * r;
  if (group_id < l - 1) {
    return r;
  } else {
    return (k + g) % r == 0 ? r : (k + g) % r;
  }
}

void Opt_LRC::grouping_information(std::vector<std::vector<int>>& groups)
{
  int idx = 0;
  for (int i = 0; i < l; i++) {
    std::vector<int> local_group;
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      local_group.push_back(idx++);
    }
    local_group.push_back(k + g + i);
    groups.push_back(local_group);
  }
}

void Opt_LRC::partition_optimal()
{
  std::vector<std::vector<int>> stripe_groups;
  grouping_information(stripe_groups);

  for (int i = 0; i < l; i++) {
    std::vector<int> local_group = stripe_groups[i];
    int group_size = int(local_group.size());
    // every g + 1 blocks a partition
    for (int j = 0; j < group_size; j += g + 1) {
      std::vector<int> partition;
      for (int ii = j; ii < j + g + 1 && ii < group_size; ii++) {
        partition.push_back(local_group[ii]);
      }
      partition_plan.push_back(partition);
    }
  }
}

void Opt_LRC::repair_multi_data_blocks_with_global_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      if (i == l - 1) {
        use_global_in_group = true;
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_LRC::repair_multi_data_blocks_with_local_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          if (i == l - 1) {
            use_global_in_group = true;
          }
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_LRC::repair_multi_blocks_with_global_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        parity_idxs.push_back(i);
        use_global_flag = true;
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          use_global_flag = true;
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      if (i == l - 1) {
        use_global_in_group = true;
      }
    }
  }

    
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_LRC::repair_multi_blocks_with_local_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          if (i == l - 1) {
            use_global_in_group = true;
          }
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  int cnt = 0;
  for (int i = k; i < k + g; i++) {
    if (failures_map[i]) {
      cnt++;
    }
  }
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        int gid = bid2gid(i);
        if (cnt == 1 && !failures_map[k + g + gid]) {
          use_global_in_group = true;
          for (auto idx : groups[gid]) {
            if (!failures_map[idx] && !help_blocks_map[idx]) {
              help_blocks_map[idx] = 1;
              if (idx >= k + g) {
                parity_idxs.push_back(idx);
              }
            }
          }
          cnt--;
        } else {
          parity_idxs.push_back(i);
          use_global_flag = true;
          cnt--;
        }
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

std::string Opt_LRC::self_information()
{
	return "Optimal_LRC(" + std::to_string(k) + "," + std::to_string(l) + \
         "," + std::to_string(g) + ")";
}

std::string Opt_LRC::type()
{
  return "Optimal_LRC";
}

void Opt_Cau_LRC::encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme)
{
  if (partial_scheme) {
    if (local_or_column) {
      encode_partial_blocks_local(data_ptrs, coding_ptrs, block_size, data_idxs,
                                  parity_idxs, failure_idxs);
    } else {
      if (parity_idxs.size() == failure_idxs.size()) {
        std::vector<int> lrc_matrix((k + g + l) * (k + g + l), 0);
        get_identity_matrix(lrc_matrix.data(), k + g + l, k + g + l);
        make_full_matrix(&(lrc_matrix.data())[k * (k + g + l)], k + g + l);
        encode_partial_blocks_for_failures_(k + g + l, lrc_matrix.data(),
                                            data_ptrs, coding_ptrs,
                                            block_size, data_idxs,
                                            parity_idxs, failure_idxs);
      } else {
        std::vector<int> lrc_matrix((k + g + l) * k, 0);
        get_identity_matrix(lrc_matrix.data(), k, k);
        make_encoding_matrix(&(lrc_matrix.data())[k * k]);
        encode_partial_blocks_for_failures_v2_(k, lrc_matrix.data(), data_ptrs,
                                               coding_ptrs, block_size, data_idxs,
                                               failure_idxs, live_idxs);
      }
    }
  } else {
    int parity_num = (int)parity_idxs.size();
    for (int i = 0; i < parity_num; i++) {
      my_assert(parity_idxs[i] >= k);
      bool flag = false;
      if (parity_idxs[i] < k + g) {
        for (auto idx : data_idxs) {
          if (idx < k || idx >= k + g || idx == parity_idxs[i]) {
            flag = true;
            break;
          }
        }
      } else {
        int gid = bid2gid(parity_idxs[i]);
        for (auto idx : data_idxs) {
          int t_gid = bid2gid(idx);
          if (t_gid == gid || (idx >= k && idx < k + g) || idx == parity_idxs[i]) {
            flag = true;
            break;
          }
        }
      }
      partial_flags[i] = flag;
    }
    std::vector<int> lrc_matrix((k + g + l) * (k + g + l), 0);
    get_identity_matrix(lrc_matrix.data(), k + g + l, k + g + l);
    make_full_matrix(&(lrc_matrix.data())[k * (k + g + l)], k + g + l);
    encode_partial_blocks_for_parities_(k + g + l, lrc_matrix.data(),
                                        data_ptrs, coding_ptrs, block_size,
                                        data_idxs, parity_idxs);
  }
}

void Opt_Cau_LRC::encode_partial_blocks_local(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs)
{
  my_assert((int)parity_idxs.size() == 1);
  my_assert(parity_idxs[0] >= k + g);
  int group_id = parity_idxs[0] - k - g;
  int min_idx = 0;
  int group_size = get_group_size(group_id, min_idx);

  // change into the index for encoding a local parity block
  parity_idxs[0] = group_size;
  for (size_t i = 0; i < failure_idxs.size(); ++i){
    int idx = failure_idxs[i];
    if (idx >= k + g) {
      failure_idxs[i] = group_size;
    } else if (idx >= k && idx < k + g) {
      failure_idxs[i] = group_size - g + idx - k;
    } else {
      failure_idxs[i] = idx - min_idx;
    }
  }
  for (size_t i = 0; i < data_idxs.size(); ++i) {
    int idx = data_idxs[i];
    if (idx >= k + g) {
      data_idxs[i] = group_size;
    } else if (idx >= k && idx < k + g) {
      data_idxs[i] = group_size - g + idx - k;

    } else {
      data_idxs[i] = idx - min_idx;
    }
  }

  std::vector<int> group_matrix((group_size + 1) * (group_size + 1), 0);
  get_identity_matrix(group_matrix.data(), group_size + 1, group_size + 1);
  make_group_matrix(&(group_matrix.data())[group_size * (group_size + 1)], group_id);
  encode_partial_blocks_for_failures_(group_size + 1, group_matrix.data(),
                                      data_ptrs, coding_ptrs, block_size,
                                      data_idxs, parity_idxs, failure_idxs);
}

int Opt_Cau_LRC::num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs)
{
  int cnt = 0;
  int parity_num = (int)parity_idxs.size();
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_idxs[i] >= k);
    bool flag = false;
    if (parity_idxs[i] < k + g) {
      for (auto idx : data_idxs) {
        if (idx < k || idx >= k + g || idx == parity_idxs[i]) {
          flag = true;
          break;
        }
      }
    } else {
      int gid = bid2gid(parity_idxs[i]);
      for (auto idx : data_idxs) {
        int t_gid = bid2gid(idx);
        if (t_gid == gid || (idx >= k && idx < k + g) || idx == parity_idxs[i]) {
          flag = true;
          break;
        }
      }
    }
    if (flag) {
      cnt ++;
    }
  }
  return cnt;
}

bool Opt_Cau_LRC::check_if_decodable(std::vector<int> failure_idxs)
{
  int failed_num = (int)failure_idxs.size();
  std::unordered_map<int, int> b2g;
  std::vector<int> group_fd_cnt;
  std::vector<int> group_slp_cnt;
  int fd_cnt = 0;
  int sgp_cnt = g;
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      b2g.insert(std::make_pair(idx, i));
      idx++;
    }
    b2g.insert(std::make_pair(k + g + i, i));
    group_fd_cnt.push_back(0);
    group_slp_cnt.push_back(1);
  }

  for (int i = 0; i < failed_num; i++) {
    int block_id = failure_idxs[i];
    if (block_id < k) {
      group_fd_cnt[b2g[block_id]] += 1;
      fd_cnt += 1;
    } else if (block_id < k + g && block_id >= k) {
      sgp_cnt -= 1;
    } else {
      group_slp_cnt[block_id - k - g] -= 1;
    }
  }

  if (sgp_cnt < g) {
    int fg_cnt = g - sgp_cnt;
    int healthy_group_cnt = 0;
    for (int i = 0; i < l; i++) {
      if (group_slp_cnt[i] && !group_fd_cnt[i]) {
        healthy_group_cnt++;
      }
    }
    if (healthy_group_cnt >= fg_cnt) {  // repair failed global parity blocks by enough healthy groups
      sgp_cnt = g;
    }
  }

  if (sgp_cnt < g) {
    if (sgp_cnt >= fd_cnt) {
      return true;
    } else {
      return false;
    }
  } else {
    for (int i = 0; i < l; i++) {
      if (group_slp_cnt[i] && group_slp_cnt[i] <= group_fd_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      }
    }
    for (int i = 0; i < l; i++) {
      if (sgp_cnt >= group_fd_cnt[i]) {
        sgp_cnt -= group_fd_cnt[i];
        group_fd_cnt[i] = 0;
      } else {
        return false;
      }
    }
  }
  return true;
}

void Opt_Cau_LRC::make_full_matrix(int *matrix, int kk)
{
  std::vector<int> encoding_matrix(m * k, 0);
	make_encoding_matrix(encoding_matrix.data());
	for (int i = 0; i < g; i++) {
		memcpy(&matrix[i * kk], &encoding_matrix[i * k], k * sizeof(int));
	}
  for (int i = 0; i < l; i++) {
    int min_idx = 0;
    int group_size = get_group_size(i, min_idx);
    std::vector<int> group_matrix(1 * group_size, 0);
    make_group_matrix(group_matrix.data(), i);
    memcpy(&matrix[(g + i) * kk + min_idx], group_matrix.data(),
        (group_size - g) * sizeof(int));
    memcpy(&matrix[(g + i) * kk + k], &group_matrix[group_size - g],
        g * sizeof(int));
  }
}

void Opt_Cau_LRC::make_encoding_matrix(int *final_matrix)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[i * k + j];
    }
  }

  int d_idx = 0;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      if (i * r <= j && j < (i + 1) * r) {
        final_matrix[(i + g) * k + j] = matrix[g * k + d_idx];
        d_idx++;
      }
    }
  }

  // print_matrix(final_matrix, g + l, k, "final_matrix_before_xor");

  for (int i = 0; i < l; i++) {
    for (int j = 0; j < g; j++) {
      galois_region_xor((char *)&matrix[j * k], (char *)&final_matrix[(i + g) * k], 4 * k);
    }
  }

  // print_matrix(final_matrix, g + l, k, "final_matrix_after_xor");

  free(matrix);
}

void Opt_Cau_LRC::make_encoding_matrix_v2(int *final_matrix)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[i * k + j];
    }
  }

  std::vector<int> l_matrix(l * (k + g), 0);
  std::vector<int> d_g_matrix((k + g) * k, 0);
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      l_matrix[i * (k + g) + idx] = matrix[g * k + idx];
      idx++;
    }
    for (int j = k; j < k + g; j++) {
      l_matrix[i * (k + g) + j] = 1;
    }
  }
  for (int i = 0; i < k; i++) {
    d_g_matrix[i * k + i] = 1;
  }
  idx = k * k;
  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      d_g_matrix[idx + i * k + j] = matrix[i * k + j];
    }
  }

  // print_matrix(l_matrix.data(), l, k + g, "l_matrix");
  // print_matrix(d_g_matrix.data(), k + g, k, "d_g_matrix");

  int *mix_matrix = jerasure_matrix_multiply(l_matrix.data(), d_g_matrix.data(),
                                             l, k + g, k + g, k, w);

  idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = mix_matrix[i * k + j];
    }
  }

  // print_matrix(final_matrix, g + l, k, "final_matrix");

  free(matrix);
  free(mix_matrix);
}

void Opt_Cau_LRC::make_group_matrix(int *group_matrix, int group_id)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, 8);
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      if (i == group_id) {
        group_matrix[j] = matrix[g * k + idx];
      }
      idx++;
    }
    for (int j = group_size; j < group_size + g; j++) {
      if (i == group_id)
        group_matrix[j] = 1;
    }
  }
}

bool Opt_Cau_LRC::check_parameters()
{
  if (r * (l - 1) < k || !k || !l || !g)
    return false;
  return true;
}

int Opt_Cau_LRC::bid2gid(int block_id)
{
  int group_id = -1;
  if (block_id < k) {
    group_id = block_id / r;
  } else if (block_id >= k && block_id < k + g) {
    group_id = l; 
  } else {
    group_id = block_id - k - g;
  }
  return group_id;
}

int Opt_Cau_LRC::idxingroup(int block_id)
{
  if (block_id < k) {
    return block_id % r;
  } else if (block_id >= k && block_id < k + g) {
    return block_id - k + r;
  } else {
    if (block_id - k - g < l - 1) {
      return r + g;
    } else {
      return k % r == 0 ? r + g : k % r + g;
    }
  }
}

int Opt_Cau_LRC::get_group_size(int group_id, int& min_idx)
{
  min_idx = group_id * r;
  if (group_id < l - 1) {
    return r + g;
  } else if (group_id == l - 1) {
    return (k % r == 0 ? r : k % r) + g;
  } else {
    min_idx = k;
    return g;
  }
}

void Opt_Cau_LRC::grouping_information(std::vector<std::vector<int>>& groups)
{
  int idx = 0;
  for (int i = 0; i < l; i++) {
    std::vector<int> local_group;
    int group_size = std::min(r, k - i * r);
    for (int j = 0; j < group_size; j++) {
      local_group.push_back(idx++);
    }
    local_group.push_back(k + g + i);
    groups.push_back(local_group);
  }
  std::vector<int> local_group;
  for (int i = 0; i < g; i++) {
    local_group.push_back(idx++);
  }
  groups.push_back(local_group);
}

std::string Opt_Cau_LRC::self_information()
{
	return "Optimal_Cauchy_LRC(" + std::to_string(k) + "," + std::to_string(l) + \
         "," + std::to_string(g) + ")";
}

std::string Opt_Cau_LRC::type()
{
  return "Optimal_Cauchy_LRC";
}

void Opt_Cau_LRC::help_blocks_for_single_block_repair(
          int failure_idx,
          std::vector<int>& parity_idxs,
          std::vector<std::vector<int>>& help_blocks,
					bool partial_scheme)
{
	int parition_num = (int)partition_plan.size();
  if (!parition_num) {
    return;
  }

  if (local_or_column) {
    if (failure_idx >= k && failure_idx < k + g) {
      for (int i = 0; i < parition_num; i++) {
        std::vector<int> help_block;
        for (auto bid : partition_plan[i]) {
          if ((bid >= k && bid < k + g && bid != failure_idx) || 
              bid2gid(bid) == surviving_group_ids[0]) {
            help_block.push_back(bid);
            if (bid >= k + g) {
              parity_idxs.push_back(bid);
            }
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
    } else {
      if (failure_idx >= k + g) {
        parity_idxs.push_back(failure_idx);
      }
      int gid = bid2gid(failure_idx);
      for (int i = 0; i < parition_num; i++) {
        std::vector<int> help_block;
        for (auto bid : partition_plan[i]) {
          if ((bid2gid(bid) == gid && bid != failure_idx) || 
              (bid >= k && bid < k + g)) {
            help_block.push_back(bid);
            if (bid >= k + g) {
              parity_idxs.push_back(bid);
            }
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
    }
  } else {
    if (failure_idx >= k) {
      parity_idxs.push_back(failure_idx);
    }
    if (!partial_scheme && failure_idx >= k && failure_idx < k + g) {
      // repair global parity with data blocks by encoding
      for (auto partition : partition_plan) {
        std::vector<int> help_block;
        for (auto bid : partition) {
          if (bid < k) {
            help_block.push_back(bid);
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
    } else {
      int main_par_idx = 0;
      std::vector<std::pair<int, int>> partition_idx_to_num;
      for (int i = 0; i < parition_num; i++) {
        int cnt = 0;
        for (auto bid : partition_plan[i]) {
          if (bid < k + g && bid != failure_idx) {
            cnt++;
          }
          if (bid == failure_idx) {
            main_par_idx = i;
            cnt = 0;
            break;
          }
        }
        if (cnt > 0) {
          partition_idx_to_num.push_back(std::make_pair(i, cnt));
        }
      }
      std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
                cmp_descending);
      
      int cnt = 0;
      std::vector<int> main_help_block;
      for (auto idx : partition_plan[main_par_idx]) {
        if (idx != failure_idx && idx < k + g) {
          if (cnt < k) {
            main_help_block.push_back(idx);
            cnt++;
            if (idx >= k) {
              parity_idxs.push_back(idx);
            }
          } else {
            break;
          }
        }
      }
      if (cnt > 0) {
        help_blocks.push_back(main_help_block);
      }
      if (cnt == k) {
        return;
      }
      for (auto& pair : partition_idx_to_num) {
        std::vector<int> help_block;
        for (auto idx : partition_plan[pair.first]) {
          if (idx < k + g) {
            if (cnt < k) {
              help_block.push_back(idx);
              cnt++;
              if (idx >= k) {
                parity_idxs.push_back(idx);
              }
            } else {
              break;
            }
          }
        }
        if (cnt > 0 && cnt <= k) {
          help_blocks.push_back(help_block);
        }
        if (cnt == k) {
          return;
        }
      }
    }
  }
}

void Opt_Cau_LRC::help_blocks_for_multi_global_blocks_repair(
				std::vector<int> failure_idxs,
        std::vector<int>& parity_idxs,
        std::vector<std::vector<int>>& help_blocks,
        bool partial_scheme)
{
  int parition_num = (int)partition_plan.size();
  my_assert(parition_num > 0);
  int failed_num = (int)failure_idxs.size();
  std::vector<bool> global_failure_map(g, false);
  for (auto idx : failure_idxs) {
    my_assert(idx >= k && idx < k + g);
    global_failure_map[idx - k] = true;
  }
  if (local_or_column) {
    for (int i = 0; i < parition_num; i++) {
        std::vector<int> help_block;
        for (auto bid : partition_plan[i]) {
          if ((bid >= k && bid < k + g && !global_failure_map[bid - k]) ||
              std::find(surviving_group_ids.begin(), surviving_group_ids.end(),
              bid2gid(bid)) != surviving_group_ids.end()) {
            help_block.push_back(bid);
            if (bid >= k + g) {
              parity_idxs.push_back(bid);
            }
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
  } else {
    for (auto idx : failure_idxs) {
      parity_idxs.push_back(idx);
    }
    if (partial_scheme) {
      std::vector<std::vector<int>> copy_partition;
      for (int i = 0; i < parition_num; i++) {
        std::vector<int> partition;
        for (auto idx : partition_plan[i]) {
          partition.push_back(idx);
        }
        copy_partition.push_back(partition);
      }

      int failures_cnt[parition_num] = {0};
      for (auto failure_idx : failure_idxs) {
        for (int i = 0; i < parition_num; i++) {
          auto it = std::find(copy_partition[i].begin(), copy_partition[i].end(),
                              failure_idx);
          if (it != copy_partition[i].end()) {
            failures_cnt[i]++;
            copy_partition[i].erase(it);	// remove the failures
            break;
          }
        }
      }
      for (int l_ = k + g; l_ < k + g + l; l_++) {
        for (int i = 0; i < parition_num; i++) {
          auto it = std::find(copy_partition[i].begin(), copy_partition[i].end(), l_);
          if (it != copy_partition[i].end()) {
            copy_partition[i].erase(it);	// remove local parity blocks
            break;
          }
        }
      }
      std::vector<std::pair<int, int>> main_partition_idx_to_num;
      std::vector<std::pair<int, int>> partition_idx_to_num;
      for (int i = 0; i < parition_num; i++) {
        int partition_size = (int)copy_partition[i].size();
        if (failures_cnt[i]) {
          main_partition_idx_to_num.push_back(std::make_pair(i, partition_size));
        } else {
          partition_idx_to_num.push_back(std::make_pair(i, partition_size));
        }
      }
      std::sort(main_partition_idx_to_num.begin(), main_partition_idx_to_num.end(),
                cmp_descending);
      std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
                cmp_descending);

      int cnt = 0;
      for (auto& pair : main_partition_idx_to_num) {
        std::vector<int> main_help_block;
        for (auto idx : copy_partition[pair.first]) {
          if (cnt < k) {
            main_help_block.push_back(idx);
            cnt++;
            if (idx >= k) {
              parity_idxs.push_back(idx);
            }
          } else {
            break;
          }
        }
        if (cnt > 0 && cnt <= k && main_help_block.size() > 0) {
          help_blocks.push_back(main_help_block);
        }
        if (cnt == k) {
          return;
        }
      }
      for (auto& pair : partition_idx_to_num) {
        std::vector<int> help_block;
        for (auto idx : copy_partition[pair.first]) {
          if (cnt < k) {
            help_block.push_back(idx);
            cnt++;
            if (idx >= k) {
              parity_idxs.push_back(idx);
            }
          } else {
            break;
          }
        }
        if (cnt > 0 && cnt <= k && help_block.size() > 0) {
            help_blocks.push_back(help_block);
        } 
        if (cnt == k) {
          return;
        }
      }
    } else {
      // repair global parity with data blocks by encoding
      for (auto partition : partition_plan) {
        std::vector<int> help_block;
        for (auto bid : partition) {
          if (bid < k) {
            help_block.push_back(bid);
          }
        }
        if (help_block.size() > 0) {
          help_blocks.push_back(help_block);
        }
      }
    }
  }
}

void Opt_Cau_LRC::repair_multi_data_blocks_with_global_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      use_global_in_group = true;
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }


  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_Cau_LRC::repair_multi_data_blocks_with_local_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy) {
          help_blocks_map[k + g + i] = 1;
          use_global_in_group = true;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_Cau_LRC::repair_multi_blocks_with_global_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity block
  int l_idx = k + g;
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        use_global_flag = true;
        parity_idxs.push_back(i);
      } else {
        use_global_in_group = true;
        parity_idxs.push_back(i);
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          use_global_flag = true;
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      use_global_in_group = true;
    }
  }

  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Opt_Cau_LRC::repair_multi_blocks_with_local_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy) {
          use_global_in_group = true;
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity block
  int cnt = 0;
  for (int i = k; i < k + g; i++) {
    if (failures_map[i]) {
      cnt++;
    }
  }
  int l_idx = k + g;
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        bool flag = false;
        if (r + g < k) {
          while (l_idx < k + g + l) {
            if (!failures_map[l_idx] && !help_blocks_map[l_idx]) {
              help_blocks_map[l_idx] = 1;
              parity_idxs.push_back(l_idx);
              use_global_in_group = true;
              flag = true;
              for (auto idx : groups[l_idx - k - g]) {
                if (!failures_map[idx] && !help_blocks_map[idx]) {
                  help_blocks_map[idx] = 1;
                }
              }
              break;
            }
            l_idx++;
          }
        }
        if (!flag) {
          use_global_flag = true;
          parity_idxs.push_back(i);
        }
      } else {
        parity_idxs.push_back(i);
        use_global_in_group = true;
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

bool Opt_Cau_LRC::generate_repair_plan(
        std::vector<int> failure_idxs, std::vector<RepairPlan>& plans,
				bool partial_scheme, bool repair_priority, bool repair_method)
{
  bool decodable = check_if_decodable(failure_idxs);
  if (!decodable) {
    return false;
  }
	int failed_num = (int)failure_idxs.size();
	if (failed_num == 1) {
    RepairPlan plan;
    for (auto idx : failure_idxs) {
      plan.failure_idxs.push_back(idx);
    }
    local_or_column = true;
    plan.local_or_column = true;
    if (failure_idxs[0] < k + g && r + g >= k) {
      local_or_column = false;
    }
    if (local_or_column && bid2gid(failure_idxs[0]) == l) {
      surviving_group_ids.clear();
      surviving_group_ids.push_back(0);
    }
		help_blocks_for_single_block_repair(failure_idxs[0], plan.parity_idxs,
        plan.help_blocks, partial_scheme);
    plans.push_back(plan);
	} else {
    if (repair_method) {
      return repair_multi_blocks_method2(failure_idxs, plans, partial_scheme, repair_priority);
    } else {
      return repair_multi_blocks_method1(failure_idxs, plans, partial_scheme, repair_priority);
    }
  }
  return true;
}

bool Opt_Cau_LRC::generate_flow_repair_plan(std::vector<int> failure_idxs,
															std::vector<RepairPlan>& plans,
															bool partial_scheme,
															bool repair_priority,
															bool repair_method,
                                      						std::vector<int> main_help_clusterID,
                	                  						std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
															int failure_stripe_ID,
															bool main_help_cluster_flag)
{
  bool decodable = check_if_decodable(failure_idxs);
  if (!decodable) {
    return false;
  }
	int failed_num = (int)failure_idxs.size();
	if (failed_num == 1) {
    RepairPlan plan;
    for (auto idx : failure_idxs) {
      plan.failure_idxs.push_back(idx);
    }
    local_or_column = true;
    plan.local_or_column = true;
    if (failure_idxs[0] < k + g && r + g >= k) {
      local_or_column = false;
    }
    if (local_or_column && bid2gid(failure_idxs[0]) == l) {
      surviving_group_ids.clear();
      surviving_group_ids.push_back(0);
    }
		help_blocks_for_single_block_repair(failure_idxs[0], plan.parity_idxs,
        plan.help_blocks, partial_scheme);
    plans.push_back(plan);
	} else {
    if (repair_method) {
      return repair_multi_blocks_method2(failure_idxs, plans, partial_scheme, repair_priority);
    } else {
      return repair_multi_blocks_method1(failure_idxs, plans, partial_scheme, repair_priority);
    }
  }
  return true;
}

bool Opt_Cau_LRC::repair_multi_blocks_method1(std::vector<int> failure_idxs,
																              std::vector<RepairPlan>& plans,
                                              bool partial_scheme,
                                              bool repair_priority)
{
  std::vector<int> failed_map(k + g + l, 0);
  std::vector<int> fb_group_cnt(l + 1, 0);
  int num_of_failed_blocks = int(failure_idxs.size());
  for (int i = 0; i < num_of_failed_blocks; i++) {
    int failed_idx = failure_idxs[i];
    failed_map[failed_idx] = 1;
    fb_group_cnt[bid2gid(failed_idx)] += 1;
    if (failed_idx >= k && failed_idx < k + g) {
      for (int j = 0; j < l; j++) {
        fb_group_cnt[j] += 1;
      }
    }
  }

  int iter_cnt = 0;
  while (num_of_failed_blocks > 0) {
    for (int i = k; i < k + g; i++) {
      // repair single global parity block in a local group if possible
      if (failed_map[i]) {
        for (int j = 0; j < l; j++) {
          if (fb_group_cnt[j] == 1) {
            surviving_group_ids.clear();
            RepairPlan plan;
            plan.local_or_column = true;
            plan.failure_idxs.push_back(i);
            local_or_column = true;
            surviving_group_ids.push_back(j);
            if (iter_cnt > 0 && r + g >= k) {
              local_or_column = false;
            }
            help_blocks_for_single_block_repair(i, plan.parity_idxs,
                plan.help_blocks, partial_scheme);
            plans.push_back(plan);

            // update
            failed_map[i] = 0;
            for (int jj = 0; jj <= l; jj++) {
              fb_group_cnt[jj] -= 1;
            }
            num_of_failed_blocks -= 1;
            failure_idxs.erase(std::find(failure_idxs.begin(),
                failure_idxs.end(), i));
            break;
          }
        }
      }
    }

    for (int group_id = 0; group_id < l; group_id++) {
      // local repair
      if (fb_group_cnt[group_id] == 1) {
        int failed_idx = -1;
        for (int i = 0; i < k + g + l; i++) {
          if (failed_map[i] && bid2gid(i) == group_id) {
            failed_idx = i;
            break;
          }
        }
        RepairPlan plan;
        plan.local_or_column = true;
        plan.failure_idxs.push_back(failed_idx);
        local_or_column = true;
        help_blocks_for_single_block_repair(failed_idx, plan.parity_idxs,
            plan.help_blocks, partial_scheme);
        plans.push_back(plan);
        // update
        failed_map[failed_idx] = 0;
        fb_group_cnt[group_id] = 0;
        num_of_failed_blocks -= 1;
        failure_idxs.erase(std::find(failure_idxs.begin(),
            failure_idxs.end(), failed_idx));
      }
    }
      
    if (num_of_failed_blocks > 0) {
      RepairPlan plan;
      plan.local_or_column = false;
      int fg_cnt = 0;
      for (int i = 0; i < k + g + l; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
          if (i >= k && i < k + g) {
            fg_cnt++;
          }
        }
      }

      if (num_of_failed_blocks == fg_cnt) {
        surviving_group_ids.clear();
        int cnt = 0;
        for (int i = 0; i < l; i++) {
          if (fb_group_cnt[i] == fg_cnt) {
            surviving_group_ids.push_back(i);
            cnt++;
            if (cnt == fg_cnt)
              break;
          }
        }
        if (cnt == fg_cnt && !repair_priority) {
          local_or_column = true;
        } else {
          local_or_column = false;
        }
        help_blocks_for_multi_global_blocks_repair(plan.failure_idxs,
            plan.parity_idxs, plan.help_blocks, partial_scheme);
      } else {
        if (repair_priority) {
          repair_multi_blocks_with_global_priority(failure_idxs,
              plan.parity_idxs, plan.help_blocks);
        } else {
          repair_multi_blocks_with_local_priority(failure_idxs,
              plan.parity_idxs, plan.help_blocks);
        }
      }
      if (plan.help_blocks.size() == 0) {
        std::cout << "Undecodable!!!" << std::endl;
        return false;
      }
      plans.push_back(plan);

      // update 
      for (int i = 0; i < k + g + l; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
    }
  }
  return true;
}


bool Opt_Cau_LRC::repair_multi_blocks_method2(std::vector<int> failure_idxs,
																              std::vector<RepairPlan>& plans,
                                              bool partial_scheme,
                                              bool repair_priority)
{
  std::vector<int> failed_map(k + g + l, 0);
  std::vector<int> fb_group_cnt(l + 1, 0);
  int num_of_failed_data = 0;
  int num_of_failed_blocks = int(failure_idxs.size());
  for (int i = 0; i < num_of_failed_blocks; i++) {
    int failed_idx = failure_idxs[i];
    failed_map[failed_idx] = 1;
    fb_group_cnt[bid2gid(failed_idx)] += 1;
    if (failed_idx < k) {
      num_of_failed_data += 1;
    } else if (failed_idx < k + g) {
      for (int j = 0; j < l; j++) {
        fb_group_cnt[j] += 1;
      }
    }
  }

  int iter_cnt = 0;
  while (num_of_failed_blocks > 0) {
    for (int i = k; i < k + g; i++) {
      // repair single global parity block in a local group if possible
      if (failed_map[i]) {
        for (int j = 0; j < l; j++) {
          if (fb_group_cnt[j] == 1) {
            surviving_group_ids.clear();
            RepairPlan plan;
            plan.local_or_column = true;
            plan.failure_idxs.push_back(i);
            local_or_column = true;
            surviving_group_ids.push_back(j);
            if (iter_cnt > 0 && r + g >= k) {
              local_or_column = false;
            }
            help_blocks_for_single_block_repair(i, plan.parity_idxs,
                plan.help_blocks, partial_scheme);
            plans.push_back(plan);
            // update
            failed_map[i] = 0;
            for (int jj = 0; jj <= l; jj++) {
              fb_group_cnt[jj] -= 1;
            }
            num_of_failed_blocks -= 1;
            failure_idxs.erase(std::find(failure_idxs.begin(),
                failure_idxs.end(), i));
            break;
          }
        }
      }
    }

    for (int group_id = 0; group_id < l; group_id++) {
      // local repair
      if (fb_group_cnt[group_id] == 1) {
        int failed_idx = -1;
        for (int i = 0; i < k + g + l; i++) {
          if (failed_map[i] && bid2gid(i) == group_id) {
            failed_idx = i;
            break;
          }
        }
        RepairPlan plan;
        plan.local_or_column = true;
        plan.failure_idxs.push_back(failed_idx);
        local_or_column = true;
        help_blocks_for_single_block_repair(failed_idx, plan.parity_idxs,
            plan.help_blocks, partial_scheme);
        plans.push_back(plan);
        // update
        failed_map[failed_idx] = 0;
        fb_group_cnt[group_id] = 0;
        num_of_failed_blocks -= 1;
        if (failed_idx < k) {
          num_of_failed_data -= 1;
        }
        failure_idxs.erase(std::find(failure_idxs.begin(),
            failure_idxs.end(), failed_idx));
      }
    }
      
    if (num_of_failed_data > 0) {
      RepairPlan plan;
      plan.local_or_column = false;

      for (int i = 0; i < k; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
        }
      }
        
      if (repair_priority) {
        repair_multi_data_blocks_with_global_priority(failure_idxs,
            plan.parity_idxs, plan.help_blocks);
      } else {
        repair_multi_data_blocks_with_local_priority(failure_idxs,
            plan.parity_idxs, plan.help_blocks);
      }
      if (plan.help_blocks.size() == 0) {
        std::cout << "Undecodable!!!" << std::endl;
        return false;
      }
      plans.push_back(plan);
        
      // update 
      for (int i = 0; i < k; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
      num_of_failed_data = 0;
    }
      
    if (num_of_failed_blocks == 0) {
      break;
    }

    if (iter_cnt > 0 && num_of_failed_blocks > 0) {
      // global parity block repair 
      RepairPlan plan;
      plan.local_or_column = false;
      for (int i = k; i < k + g; i++) {
        if (failed_map[i]) {
          plan.failure_idxs.push_back(i);
        }
      }
      int failed_parity_num = (int)plan.failure_idxs.size();
      if (failed_parity_num == 1) {
        local_or_column = false;
        help_blocks_for_single_block_repair(plan.failure_idxs[0],
            plan.parity_idxs, plan.help_blocks, partial_scheme);
      } else if(failed_parity_num > 1) {
        surviving_group_ids.clear();
        int cnt = 0;
        for (int i = 0; i < l; i++) {
          if (fb_group_cnt[i] == failed_parity_num) {
            surviving_group_ids.push_back(i);
            cnt++;
            if (cnt == failed_parity_num)
              break;
          }
        }
        if (cnt == failed_parity_num && failed_parity_num < l) {
          local_or_column = true;
        } else {
          local_or_column = false;
        }
        help_blocks_for_multi_global_blocks_repair(plan.failure_idxs,
            plan.parity_idxs, plan.help_blocks, partial_scheme);
      }
      plans.push_back(plan); 
        
      // update
      for (int i = k; i < k + g; i++) {
        if (failed_map[i]) {
          failed_map[i] = 0;
          num_of_failed_blocks -= 1;
          fb_group_cnt[bid2gid(i)] -= 1;
          failure_idxs.erase(std::find(failure_idxs.begin(),
              failure_idxs.end(), i));
        }
      }
    }
    iter_cnt++;
    if (iter_cnt > 2) {
      std::cout << "generate repair plan error!" << std::endl;
      return false;
    }
  }
  return true;
}

bool Uni_Cau_LRC::check_if_decodable(std::vector<int> failure_idxs)
{
  int failed_num = (int)failure_idxs.size();
  std::unordered_map<int, int> b2g;
  std::vector<int> group_fd_cnt;
  std::vector<int> group_fgp_cnt;
  std::vector<int> group_slp_cnt;
  std::vector<bool> group_pure_flag;
  int sgp_cnt = g;
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      b2g.insert(std::make_pair(idx, i));
      idx++;
    }
    if (idx <= k || idx - group_size >= k) {
      group_pure_flag.push_back(true);
    } else {
      group_pure_flag.push_back(false);
    }
    b2g.insert(std::make_pair(k + g + i, i));
    group_fd_cnt.push_back(0);
    group_fgp_cnt.push_back(0);
    group_slp_cnt.push_back(1);
  }

  for (int i = 0; i < failed_num; i++) {
    int block_id = failure_idxs[i];
    if (block_id < k) {
      group_fd_cnt[b2g[block_id]] += 1;
    } else if (block_id < k + g && block_id >= k) {
      group_fgp_cnt[b2g[block_id]] += 1;
      sgp_cnt -= 1;
    } else {
      group_slp_cnt[block_id - k - g] -= 1;
    }
  }

  for (int i = 0; i < l; i++) {
    if (group_slp_cnt[i] && group_pure_flag[i]) {
      if (group_slp_cnt[i] <= group_fd_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      }
      if (group_slp_cnt[i] && group_slp_cnt[i] == group_fgp_cnt[i]) {
        group_fgp_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
        sgp_cnt += 1;
      }
    } else if (group_slp_cnt[i] && !group_pure_flag[i]) {
      if (group_fd_cnt[i] == 1 && !group_fgp_cnt[i]) {
        group_fd_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
      } else if (group_fgp_cnt[i] == 1 && !group_fd_cnt[i]) {
        group_fgp_cnt[i] -= group_slp_cnt[i];
        group_slp_cnt[i] = 0;
        sgp_cnt += 1;
      }
    }
  }
  for (int i = 0; i < l; i++) {
    if (sgp_cnt >= group_fd_cnt[i]) {
      sgp_cnt -= group_fd_cnt[i];
      group_fd_cnt[i] = 0;
    } else {
      return false;
    }
  }
  return true;
}

void Uni_Cau_LRC::make_encoding_matrix(int *final_matrix)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[i * k + j];
    }
  }

  std::vector<int> l_matrix(l * k, 0);
  int d_idx = 0;
  int l_idx = 0;
  for (int i = 0; i < l && d_idx < k; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size && d_idx < k; j++) {
      l_matrix[i * k + d_idx] = matrix[g * k + d_idx];
      d_idx++;
    }
    l_idx = i;
  }

  // print_matrix(l_matrix.data(), l, k, "l_matrix_before_xor");

  int g_idx = 0;
  for (int i = l_idx; i < l; i++) {
    int sub_g_num = -1;
    int group_size = std::min(r, k + g - i * r);
    if (group_size < r) { // must be the last group
      sub_g_num = g - g_idx;
    } else {
      sub_g_num = (i + 1) * r - (k + g_idx);
    }
    std::vector<int> sub_g_matrix(sub_g_num * k, 0);
    for (int j = 0; j < sub_g_num; j++) {
      for (int jj = 0; jj < k; jj++) {
        sub_g_matrix[j * k + jj] = matrix[g_idx * k + jj];
      }
      g_idx++;
    }
    for (int j = 0; j < sub_g_num; j++) {
      galois_region_xor((char *)&sub_g_matrix[j * k], (char *)&l_matrix[i * k], 4 * k);
    }
  }

  // print_matrix(l_matrix.data(), l, k, "l_matrix_after_xor");

  int idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = l_matrix[i * k + j];
    }
  }

  // print_matrix(final_matrix, g + l, k, "final_matrix");

  free(matrix);
}

void Uni_Cau_LRC::make_encoding_matrix_v2(int *final_matrix)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, w);

  bzero(final_matrix, sizeof(int) * k * (g + l));

  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[i * k + j] = matrix[i * k + j];
    }
  }

  std::vector<int> l_matrix(l * (k + g), 0);
  std::vector<int> d_g_matrix((k + g) * k, 0);
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      if (idx < k) {
        l_matrix[i * (k + g) + idx] = matrix[g * k + idx];
      } else {
        l_matrix[i * (k + g) + idx] = 1;
      }
      idx++;
    }
  }
  for (int i = 0; i < k; i++) {
    d_g_matrix[i * k + i] = 1;
  }
  idx = k * k;
  for (int i = 0; i < g; i++) {
    for (int j = 0; j < k; j++) {
      d_g_matrix[idx + i * k + j] = matrix[i * k + j];
    }
  }

  // print_matrix(l_matrix.data(), l, k + g, "l_matrix");
  // print_matrix(d_g_matrix.data(), k + g, k, "d_g_matrix");

  int *mix_matrix = jerasure_matrix_multiply(l_matrix.data(), d_g_matrix.data(),
                                             l, k + g, k + g, k, w);

  idx = g * k;
  for (int i = 0; i < l; i++) {
    for (int j = 0; j < k; j++) {
      final_matrix[idx + i * k + j] = mix_matrix[i * k + j];
    }
  }

  // print_matrix(final_matrix, g + l, k, "final_matrix");

  free(matrix);
  free(mix_matrix);
}

void Uni_Cau_LRC::make_group_matrix(int *group_matrix, int group_id)
{
  int *matrix = cauchy_good_general_coding_matrix(k, g + 1, 8);
  int idx = 0;
  for (int i = 0; i < l; i++) {
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      if (i == group_id) {
        if (idx < k) {
          group_matrix[j] = matrix[g * k + idx];
        } else {
          group_matrix[j] = 1;
        }
      }
      idx++;
    }
  }
}

bool Uni_Cau_LRC::check_parameters()
{
  if (r * (l - 1) < k + g || !k || !l || !g)
    return false;
  return true;
}

int Uni_Cau_LRC::bid2gid(int block_id)
{
  int group_id = -1;
  if (block_id < k + g) {
    group_id = block_id / r;
  } else {
    group_id = block_id - k - g;
  }
  return group_id;
}

int Uni_Cau_LRC::idxingroup(int block_id)
{
  if (block_id < k + g) {
    return block_id % r;
  } else {
    if (block_id - k - g < l - 1) {
      return r;
    } else {
      return (k + g) % r == 0 ? r : (k + g) % r;
    }
  }
}

int Uni_Cau_LRC::get_group_size(int group_id, int& min_idx)
{
  min_idx = group_id * r;
  if (group_id < l - 1) {
    return r;
  } else {
    return (k + g) % r == 0 ? r : (k + g) % r;
  }
}

void Uni_Cau_LRC::grouping_information(std::vector<std::vector<int>>& groups)
{
  int idx = 0;
  for (int i = 0; i < l; i++) {
    std::vector<int> local_group;
    int group_size = std::min(r, k + g - i * r);
    for (int j = 0; j < group_size; j++) {
      local_group.push_back(idx++);
    }
    local_group.push_back(k + g + i);
    groups.push_back(local_group);
  }
}

void Uni_Cau_LRC::partition_optimal()
{
  std::vector<std::vector<int>> stripe_groups;
  grouping_information(stripe_groups);

  for (int i = 0; i < l; i++) {
    std::vector<int> local_group = stripe_groups[i];
    int group_size = int(local_group.size());
    // every g + 1 blocks a partition
    for (int j = 0; j < group_size; j += g + 1) {
      std::vector<int> partition;
      for (int ii = j; ii < j + g + 1 && ii < group_size; ii++) {
        partition.push_back(local_group[ii]);
      }
      partition_plan.push_back(partition);
    }
  }
}

void Uni_Cau_LRC::repair_multi_data_blocks_with_global_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      if (i == l - 1) {
        use_global_in_group = true;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Uni_Cau_LRC::repair_multi_data_blocks_with_local_priority(
					std::vector<int> failure_idxs,
          std::vector<int>& parity_idxs,
					std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          if (i == l - 1) {
            use_global_in_group = true;
          }
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }
  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Uni_Cau_LRC::repair_multi_blocks_with_global_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  int cnt = 0;
  for (int i = k; i < k + g; i++) {
    if (failures_map[i]) {
      cnt++;
    }
  }
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        int gid = bid2gid(i);
        if (cnt == 1 && !failures_map[k + g + gid]) {
          use_global_in_group = true;
          for (auto idx : groups[gid]) {
            if (!failures_map[idx] && !help_blocks_map[idx]) {
              help_blocks_map[idx] = 1;
            }
          }
          cnt--;
        } else {
          parity_idxs.push_back(i);
          use_global_flag = true;
          cnt--;
        }
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  // replace local with global
  global_idx = k;
  for (int i = 0; i < l; i++) {
    if (help_blocks_map[k + g + i]) {
      while (global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          help_blocks_map[global_idx] = 1;
          help_blocks_map[k + g + i] = 0;
          parity_idxs.push_back(global_idx);
          break;
        }
        global_idx++;
      }
    }
    if (help_blocks_map[k + g + i]) {
      for (auto idx : groups[i]) {
        if (!failures_map[idx] && !help_blocks_map[idx]) {
          help_blocks_map[idx] = 1;
        }
      }
      parity_idxs.push_back(k + g + i);
      if (i == l - 1) {
        use_global_in_group = true;
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

void Uni_Cau_LRC::repair_multi_blocks_with_local_priority(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks)
{
  int failed_num = (int)failure_idxs.size();
  std::vector<int> failures_map(k + g + l, 0);
  std::vector<int> help_blocks_map(k + g + l, 0);
  std::vector<int> group_fd_cnt(l, 0);
  bool global_healthy = true;
  for (auto idx : failure_idxs) {
    failures_map[idx] = 1;
    if (idx < k) {
      group_fd_cnt[bid2gid(idx)] += 1;
    }
    if (idx >= k && idx < k + g) {
      global_healthy = false;
    }
  }

  std::vector<std::vector<int>> groups;
  grouping_information(groups);
  int global_idx = k;
  bool use_global_flag = false;
  bool use_global_in_group = false;
  for (int i = 0; i < l; i++) {
    if (group_fd_cnt[i]) {
      // first local
      if (!failures_map[k + g + i] && !help_blocks_map[k + g + i]) {
        if (global_healthy || i < l - 1) {
          help_blocks_map[k + g + i] = 1;
          group_fd_cnt[i] -= 1;
          if (i == l - 1) {
            use_global_in_group = true;
          }
          parity_idxs.push_back(k + g + i);
        }
      }
      // then global
      while (group_fd_cnt[i] && global_idx < k + g) {
        if (!failures_map[global_idx] && !help_blocks_map[global_idx]) {
          use_global_flag = true;
          help_blocks_map[global_idx] = 1;
          group_fd_cnt[i] -= 1;
          parity_idxs.push_back(global_idx);
        }
        global_idx++;
      }
      if (help_blocks_map[k + g + i]) {
        for (auto idx : groups[i]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
      if (group_fd_cnt[i]) {
        std::cout << "undecodable!\n";
        return;
      }
    }
  }

  // focus on parity blocks
  int cnt = 0;
  for (int i = k; i < k + g; i++) {
    if (failures_map[i]) {
      cnt++;
    }
  }
  for (int i = k; i < k + g + l; i++) {
    if (failures_map[i]) {
      if (i < k + g) {
        int gid = bid2gid(i);
        if (cnt == 1 && !failures_map[k + g + gid]) {
          use_global_in_group = true;
          for (auto idx : groups[gid]) {
            if (!failures_map[idx] && !help_blocks_map[idx]) {
              help_blocks_map[idx] = 1;
              if (idx >= k + g) {
                parity_idxs.push_back(idx);
              }
            }
          }
          cnt--;
        } else {
          parity_idxs.push_back(i);
          use_global_flag = true;
          cnt--;
        }
      } else {
        parity_idxs.push_back(i);
        if (i - k - g == l - 1) {
          use_global_in_group = true;
        }
        for (auto idx : groups[i - k - g]) {
          if (!failures_map[idx] && !help_blocks_map[idx]) {
            help_blocks_map[idx] = 1;
          }
        }
      }
    }
  }

  if (use_global_in_group) {
    for (int i = k; i < k + g; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }
  
  if (use_global_flag) {
    for (int i = 0; i < k; i++) {
      if (!failures_map[i] && !help_blocks_map[i]) {
        help_blocks_map[i] = 1;
      }
    }
  }

  for (auto& partition : partition_plan) {
    std::vector<int> help_block;
    for (auto idx : partition) {
      if (help_blocks_map[idx]) {
        help_block.push_back(idx);
      }
    }
    if (help_block.size() > 0) {
      help_blocks.push_back(help_block);
    }
  }
}

std::string Uni_Cau_LRC::self_information()
{
	return "Uniform_Cauchy_LRC(" + std::to_string(k) + "," + std::to_string(l) + \
         "," + std::to_string(g) + ")";
}

std::string Uni_Cau_LRC::type()
{
	return "Uniform_Cauchy_LRC";
}