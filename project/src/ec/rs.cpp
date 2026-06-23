#include "rs.h"

using namespace ECProject;

void RSCode::make_encoding_matrix(int *final_matrix)
{
	int *matrix = reed_sol_vandermonde_coding_matrix(k, m, w);

	bzero(final_matrix, sizeof(int) * k * m);

	for (int i = 0; i < m; i++) {
		for (int j = 0; j < k; j++) {
			final_matrix[i * k + j] = matrix[i * k + j];
		}
	}

	free(matrix);
}

void RSCode::encode(char **data_ptrs, char **coding_ptrs, int block_size)
{
	std::vector<int> rs_matrix(k * m, 0);
	make_encoding_matrix(rs_matrix.data());
	jerasure_matrix_encode(k, m, w, rs_matrix.data(), data_ptrs, coding_ptrs, block_size);
}

void RSCode::decode(char **data_ptrs, char **coding_ptrs, int block_size,
										int *erasures, int failed_num)
{
	if (failed_num > m) {
		std::cout << "[Decode] Undecodable!" << std::endl;
		return;
	}
	int *rs_matrix = reed_sol_vandermonde_coding_matrix(k, m, w);
	int ret = 0;
	ret = jerasure_matrix_decode(k, m, w, rs_matrix, failed_num, erasures,
															 data_ptrs, coding_ptrs, block_size);
	if (ret == -1) {
		std::cout << "[Decode] Failed!" << std::endl;
		return;
	}
}

void RSCode::encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme)
{
	if (partial_scheme) {
		if (parity_idxs.size() == failure_idxs.size()) {
			std::vector<int> rs_matrix((k + m) * (k + m), 0);
			get_identity_matrix(rs_matrix.data(), k + m, k + m);
			make_full_matrix(&(rs_matrix.data())[k * (k + m)], k + m);
			// print_matrix(rs_matrix.data(), k + m, k + m, "full_matrix");
			encode_partial_blocks_for_failures_(k + m, rs_matrix.data(), data_ptrs,
					coding_ptrs, block_size, data_idxs, parity_idxs, failure_idxs);
		} else {
			std::vector<int> rs_matrix((k + m) * k, 0);
			get_identity_matrix(rs_matrix.data(), k, k);
			make_encoding_matrix(&(rs_matrix.data())[k * k]);
			encode_partial_blocks_for_failures_v2_(k, rs_matrix.data(), data_ptrs,
																						 coding_ptrs, block_size,
																						 data_idxs, failure_idxs,
																						 live_idxs); 
		}
		
	} else {
		int parity_num = (int)parity_idxs.size();
		for (int i = 0; i < parity_num; i++) {
			my_assert(parity_idxs[i] >= k);
			partial_flags[i] = true;
		}
		std::vector<int> rs_matrix((k + m) * (k + m), 0);
		get_identity_matrix(rs_matrix.data(), k + m, k + m);
		make_full_matrix(&(rs_matrix.data())[k * (k + m)], k + m);
		// print_matrix(rs_matrix.data(), k + m, k + m, "full_matrix");
		encode_partial_blocks_for_parities_(k + m, rs_matrix.data(), data_ptrs,
				coding_ptrs, block_size, data_idxs, parity_idxs);
	}
	
}

void RSCode::decode_with_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> failure_idxs, std::vector<int> parity_idxs)
{
	for (auto idx : parity_idxs) {
		my_assert(idx >= k);
	}
	std::vector<int> rs_matrix((k + m) * (k + m), 0);
	get_identity_matrix(rs_matrix.data(), k + m, k + m);
	make_full_matrix(&(rs_matrix.data())[k * (k + m)], k + m);
	// print_matrix(rs_matrix.data(), k + m, k + m, "full_matrix");
	decode_with_partial_blocks_(k + m, rs_matrix.data(), data_ptrs, coding_ptrs,
															block_size, failure_idxs, parity_idxs);
}

int RSCode::num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs)
{
	return (int)parity_idxs.size();
}

bool RSCode::check_if_decodable(std::vector<int> failure_idxs)
{
	int failed_num = (int)failure_idxs.size();
	if (m >= failed_num) {
		return true;
	} else {
		return false;
	}
}

void RSCode::partition_random()
{
	int n = k + m;
	int cluster_num = 5;
	std::vector<int> blocks;
	for (int i = 0; i < n; i++) {
		blocks.push_back(i);
	}

	int cnt = 0;
	while (cnt < n) {
		// at least subject to single-region fault tolerance
		int random_partition_size = random_range(1, m);
		int partition_size = std::min(random_partition_size, n - cnt);
		std::vector<int> partition;
		for (int i = 0; i < partition_size; i++, cnt++) {
			int ran_idx = random_index(n - cnt);
			int block_idx = blocks[ran_idx];
			partition.push_back(block_idx);
			auto it = std::find(blocks.begin(), blocks.end(), block_idx);
			blocks.erase(it);
		}
		partition_plan.push_back(partition);
	}

	while (partition_plan.size() > cluster_num) {
		std::vector<int> extra_partition = partition_plan.back();
		partition_plan.pop_back();

		for (int block_to_move : extra_partition) {
			bool moved = false;
			std::vector<int> target_indices(partition_plan.size());
			std::iota(target_indices.begin(), target_indices.end(), 0);
			std::random_shuffle(target_indices.begin(), target_indices.end());

			for (int idx : target_indices) {
				if (partition_plan[idx].size() < m) {
					partition_plan[idx].push_back(block_to_move);
					moved = true;
					break;
				}
			}

			// 如果没有分区有空间，我们必须将块重新添加为新分区，虽然这种情况不应该发生
			if (!moved) {
				partition_plan.push_back({block_to_move});
				break;
			}
		}
	}
}

void RSCode::partition_optimal()
{
	int n = k + m;
	int cnt = 0;
	while (cnt < n) {
		// every m blocks in a partition
		int partition_size = std::min(m, n - cnt);
		std::vector<int> partition;
		for (int i = 0; i < partition_size; i++, cnt++) {
			partition.push_back(cnt);
		}
		partition_plan.push_back(partition);
	}
}

std::string RSCode::self_information()
{
	return "RS(" + std::to_string(k) + "," + std::to_string(m) + ")";
}

std::string RSCode::type()
{
	return "RS";
}

// failure_idx: 需要修复的块的索引 (0 ~ k+m-1)
// parity_idx: 输出参数，存放需要读取的奇偶校验块的索引
// help_blocks: 输出参数，存放分组的帮助块列表，每个子列表代表一个分区的帮助块
// partial_scheme: 是否使用部分修复方案
void RSCode::help_blocks_for_single_block_repair(
				int failure_idx,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks,
				bool partial_scheme)
{
	int parition_num = (int)partition_plan.size();
	if (!parition_num) {
		return;
	}
	if (failure_idx >= k) {
		parity_idxs.push_back(failure_idx);
	}

	if (failure_idx < k || (partial_scheme && failure_idx >= k)) {
		int main_parition_idx = -1;
		std::vector<std::pair<int, int>> partition_idx_to_num;
		for (int i = 0; i < parition_num; i++) {
			auto it = std::find(partition_plan[i].begin(), partition_plan[i].end(),
													failure_idx);
			if (it != partition_plan[i].end()) {
				main_parition_idx = i; // 包含失效块的分区ID
			} else {
				int partition_size = (int)partition_plan[i].size();
				partition_idx_to_num.push_back(std::make_pair(i, partition_size));
			} // 对其他分区记录索引和大小，存入 partition_idx_to_num
		}
		std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
							cmp_descending); // 降序排序

		int cnt = 0;
		std::vector<int> main_help_block;
		for (auto idx : partition_plan[main_parition_idx]) {
			if (idx != failure_idx) {
				if (cnt < k) {
					main_help_block.push_back(idx); // 从主分区选择除失效块外的其他块作为 help_blocks
					cnt++;
					if (idx >= k) {
						parity_idxs.push_back(idx); // 记录校验块
					}
				} else {
					break;
				}
			}
		}
		if (cnt > 0) {
			help_blocks.push_back(main_help_block); // 将从主分区选择的可用块加入修复方案
		}
		if (cnt == k) {
			return; // 如果已经够k个可用块，则直接返回
		}
		for (auto& pair : partition_idx_to_num) { // 从其他分区继续选择可用块
			std::vector<int> help_block;
			for (auto idx : partition_plan[pair.first]) {
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
			if (cnt > 0 && cnt <= k) {
				help_blocks.push_back(help_block);
			} 
			if (cnt == k) { // 直到选满k个
				return;
			}
		}
	} else { // 如果当前方案是 partial_scheme=false，则从每个分区选择所有数据块加入修复方案：
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

void RSCode::help_blocks_for_multi_blocks_repair(
				std::vector<int> failure_idxs,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks,
				bool partial_scheme)
{
	int parition_num = (int)partition_plan.size();
	if (!parition_num) {
    return;
  }
	bool global_flag = true;
	for (auto failure_idx : failure_idxs) {
		if (failure_idx < k) {
			global_flag = false;
			break;
		}
		if (failure_idx >= k) {
			parity_idxs.push_back(failure_idx);
		}
	}

	if (global_flag && !partial_scheme) {
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
	}
}

// failure_idx: 需要修复的块的索引 (0 ~ k+m-1)
// parity_idx: 输出参数，存放需要读取的奇偶校验块的索引
// help_blocks: 输出参数，存放分组的帮助块列表，每个子列表代表一个分区的帮助块
// partial_scheme: 是否使用部分修复方案
void RSCode::help_blocks_for_single_block_flow_repair(
				int failure_idx,
				std::vector<int>& parity_idxs,
				std::vector<std::vector<int>>& help_blocks,
				bool partial_scheme,
                std::vector<int> main_help_clusterID,
            	std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
				int failure_stripe_ID,
				bool main_help_cluster_flag)
{
	int parition_num = (int)partition_plan.size();
	if (!parition_num) {
		return;
	}
	if (failure_idx >= k) {
		parity_idxs.push_back(failure_idx);
	}

	if (failure_idx < k || (partial_scheme && failure_idx >= k)) {
		// 新增参数：目的机架所含分区ID
		int main_parition_idx = -1;
		// 如果flag=-1，说明目的机架内没有可用块，也就没有分区ID
		// flag =  -1  -> main_parition_idx =  -1  -> 没有分区ID=-1
		// flag != -1 ->  main_parition_idx != -1  -> 可以找到一个分区ID
		if (main_help_cluster_flag != -1) {
			main_parition_idx = main_help_clusterID[failure_stripe_ID];
		}

		std::vector<std::pair<int, int>> partition_idx_to_num;
		for (int i = 0; i < parition_num; i++) {
			// 如果 main_parition_idx != -1，则只处理非主分区的其他分区
			// 如果 main_parition_idx =  -1，则处理所有分区
			// 对其他分区记录索引和大小，存入 partition_idx_to_num
			if (i != main_parition_idx) {
				int partition_size = (int)partition_plan[i].size();
				partition_idx_to_num.push_back(std::make_pair(i, partition_size));
			}
		}
		std::sort(partition_idx_to_num.begin(), partition_idx_to_num.end(),
							cmp_descending); // 降序排序

		int cnt = 0;
		std::vector<int> main_help_block;
		// 如果存在主分区ID
		if (main_parition_idx != -1) {
			for (auto idx : partition_plan[main_parition_idx]) {
				if (idx != failure_idx) {
					if (cnt < k) {
						main_help_block.push_back(idx); // 从主分区选择除失效块外的其他块作为 help_blocks
						cnt++;
						if (idx >= k) {
							parity_idxs.push_back(idx); // 记录校验块
						}
					} else {
						break;
					}
				}
			}
			if (cnt > 0) {
				help_blocks.push_back(main_help_block); // 将从主分区选择的可用块加入修复方案
			}
			if (cnt == k) {
				return; // 如果已经够k个可用块，则直接返回
			}
		}

		// 从其他分区继续选择可用块
		for (auto& pair : other_help_clusterID_chunkNum_pairs[failure_stripe_ID]) {
			std::vector<int> help_block;
			int partition_chunk_num = 0;
			for (auto idx : partition_plan[pair.first]) {
				if (cnt < k && partition_chunk_num < pair.second) {
					if (idx == failure_idx) continue;
					help_block.push_back(idx);
					cnt++;
					if (idx >= k) {
						parity_idxs.push_back(idx);
					}
				} else {
					break;
				}
			}
			if (cnt > 0 && cnt <= k) {
				help_blocks.push_back(help_block);
			} 
			if (cnt == k) { // 直到选满k个
				return;
			}
		}
	} else { // 如果当前方案是 partial_scheme=false，则从每个分区选择所有数据块加入修复方案：
		// 未做修改
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


bool RSCode::generate_repair_plan(std::vector<int> failure_idxs,
																	std::vector<RepairPlan>& plans,
																	bool partial_scheme,
																	bool repair_priority,
																	bool repair_method)
{
	int failed_num = (int)failure_idxs.size();
	RepairPlan plan;
	for (auto idx : failure_idxs) {
		plan.failure_idxs.push_back(idx);
	}
	if (failed_num == 1) {
		help_blocks_for_single_block_repair(failure_idxs[0],
																				plan.parity_idxs,
																				plan.help_blocks,
																				partial_scheme);
	} else {
		help_blocks_for_multi_blocks_repair(failure_idxs,
																				plan.parity_idxs,
																				plan.help_blocks,
																				partial_scheme);
	}
	plans.push_back(plan);
	return true;
}

bool RSCode::generate_flow_repair_plan(std::vector<int> failure_idxs,
														std::vector<RepairPlan>& plans,
														bool partial_scheme,
														bool repair_priority,
														bool repair_method,
                                      					std::vector<int> main_help_clusterID,
                	                  					std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
														int failure_stripe_ID,
														bool main_help_cluster_flag)
{
	int failed_num = (int)failure_idxs.size();
	RepairPlan plan;
	for (auto idx : failure_idxs) {
		plan.failure_idxs.push_back(idx);
	}
	if (failed_num == 1) {
		help_blocks_for_single_block_flow_repair(failure_idxs[0],
															plan.parity_idxs,
															plan.help_blocks,
															partial_scheme,
															main_help_clusterID,
															other_help_clusterID_chunkNum_pairs,
															failure_stripe_ID,
															main_help_cluster_flag);
	} else {
		std::cout << "[ERROR] : In rs.cpp/generate_flow_repair_plan(), failed_num > 1 " << std::endl;
	}
	plans.push_back(plan);
	return true;
}