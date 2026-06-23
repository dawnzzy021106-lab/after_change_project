#include "pc.h"

using namespace ECProject;

void ProductCode::init_coding_parameters(CodingParameters cp)
{
	k1 = cp.k1;
	m1 = cp.m1;
	k2 = cp.k2;
	m2 = cp.m2;
	k = k1 * k2;
	m = (k1 + m1) * (k2 + m2) - k;
	row_code.k = k1;
	row_code.m = m1;
	col_code.k = k2;
	col_code.m = m2;
	local_or_column = cp.local_or_column;
}

void ProductCode::get_coding_parameters(CodingParameters& cp)
{
	cp.k1 = k1;
	cp.m1 = m1;
	cp.k2 = k2;
	cp.m2 = m2;
	cp.k = k;
	cp.m = m;
	cp.local_or_column = local_or_column;
}

/*
	PC: (k2 + m2) rows * (k1 + m1) cols
	encode_PC:
	data_ptrs: ordered by row
	D(0) ... D(k1*k2-1)
	coding_ptrs: ordered by row
	R(0) ... R(k2*m1-1) C(0) ...  C(m2*k1-1) G(0) ... G(m2*m1-1)
*/
void ProductCode::encode(char **data_ptrs, char **coding_ptrs, int block_size)
{
	// encode row parities
	for (int i = 0; i < k2; i++) {
		std::vector<char *> t_coding(m1);
		char **coding = (char **)t_coding.data();
		for (int j = 0; j < m1; j++) {
			coding[j] = coding_ptrs[i * m1 + j];
		}
		row_code.encode(&data_ptrs[i * k1], coding, block_size);
	}
	// encode column parities
	for (int i = 0; i < k1 + m1; i++) {
		std::vector<char *> t_data(k2);
		char **data = (char **)t_data.data();
		if (i < k1) {
			for (int j = 0; j < k2; j++) {
				data[j] = data_ptrs[j * k1 + i];
			}
		} else {
			for (int j = 0; j < k2; j++) {
				data[j] = coding_ptrs[j * m1 + i - k1];
			}
		}
		std::vector<char *> t_coding(m2);
		char **coding = (char **)t_coding.data();
		if (i < k1) {
			for (int j = 0; j < m2; j++) {
				coding[j] = coding_ptrs[k2 * m1 + j * k1 + i];
			}
		} else {
			for (int j = 0; j < m2; j++) {
				coding[j] = coding_ptrs[k2 * m1 + k1 * m2 + j * m1 + i - k1];
			}
		}
		col_code.encode(data, coding, block_size);
	}
}

// the index order is the same as shown above
void ProductCode::decode(char **data_ptrs, char **coding_ptrs, int block_size,
												 int *erasures, int failed_num)
{
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<std::vector<char *>> blocks_map(k2 + m2, std::vector<char *>(k1 + m1, nullptr));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	int data_idx = 0;
	int parity_idx = 0;
	for (int i = 0; i < k2; i++) {
		for (int j = 0; j < k1 + m1; j++) {
			if (j < k1) {
				blocks_map[i][j] = data_ptrs[data_idx++];
			} else {
				blocks_map[i][j] = coding_ptrs[parity_idx++];
			}
		}
	}
	int g_parity_idx = k2 * m1 + m2 * k1;
	for (int i = k2; i < k2 + m2; i++) {
		for (int j = 0; j < k1 + m1; j++) {
			if (j < k1) {
				blocks_map[i][j] = coding_ptrs[parity_idx++];
			} else {
				blocks_map[i][j] = coding_ptrs[g_parity_idx++];
			}
		}
	}

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = erasures[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1 + m1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				std::vector<char *> data(k2, nullptr);
				std::vector<char *> coding(m2, nullptr);
				int *erasure = new int[fb_col_cnt[i] + 1];
				erasure[fb_col_cnt[i]] = -1;
				int cnt = 0;
				for (int jj = 0; jj < k2; jj++) {
					if (failed_map[jj][i]) {
						erasure[cnt++] = jj;
					}
					data[jj] = blocks_map[jj][i];
				}
				for (int jj = 0; jj < m2; jj++) {
					if (failed_map[jj + k2][i]) {
						erasure[cnt++] = jj + k2;
					}
					coding[jj] = blocks_map[jj + k2][i];
				}
				col_code.decode(data.data(), coding.data(), block_size, erasure, fb_col_cnt[i]);
				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				delete erasure;
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2 + m2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				std::vector<char *> data(k1, nullptr);
				std::vector<char *> coding(m1, nullptr);
				int *erasure = new int[fb_row_cnt[i] + 1];
				erasure[fb_row_cnt[i]] = -1;
				int cnt = 0;
				for (int jj = 0; jj < k1; jj++) {
					if (failed_map[i][jj]) {
						erasure[cnt++] = jj;
					}
					data[jj] = blocks_map[i][jj];
				}
				for (int jj = 0; jj < m1; jj++) {
					if (failed_map[i][jj + k1]) {
						erasure[cnt++] = jj + k1;
					}
					coding[jj] = blocks_map[i][jj + k1];
				}
				row_code.decode(data.data(), coding.data(), block_size, erasure, fb_row_cnt[i]);
				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				delete erasure;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return;
		}
	}
}

// the index order is the same as shown above
bool ProductCode::check_if_decodable(std::vector<int> failure_idxs)
{
	int failed_num = (int)failure_idxs.size();
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++)
	{
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0)
	{
		// part one
		for (int i = 0; i < k1 + m1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) { // repair on column
				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2 + m2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) {// repair on row
				max_row = i;
				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
			}
		}
		if (max_row == -1) {
			return false;
		}
	}
	return true;
}

void ProductCode::encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme)
{
	int row = -1, col = -1;
	if (local_or_column) {
		for (auto& idx : data_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		for (auto& idx : failure_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		col_code.encode_partial_blocks(data_ptrs, coding_ptrs,
				block_size, data_idxs, parity_idxs, failure_idxs,
				live_idxs, partial_flags, partial_scheme);
	} else {
		for (auto& idx : data_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		for (auto& idx : failure_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		row_code.encode_partial_blocks(data_ptrs, coding_ptrs,
				block_size, data_idxs, parity_idxs, failure_idxs,
				live_idxs, partial_flags, partial_scheme);
	}
}

void ProductCode::decode_with_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> failure_idxs, std::vector<int> parity_idxs)
{
	int row = -1, col = -1;
	if (local_or_column) {
		for (auto& idx : failure_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		col_code.decode_with_partial_blocks(data_ptrs, coding_ptrs,
				block_size, failure_idxs, parity_idxs);
	} else {
		for (auto& idx : failure_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		row_code.decode_with_partial_blocks(data_ptrs, coding_ptrs,
				block_size, failure_idxs, parity_idxs);
	}
}

int ProductCode::num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs)
{
	int row = -1, col = -1;
	if (local_or_column) {
		for (auto& idx : data_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = row;
		}
		return col_code.num_of_partial_blocks_to_transfer(data_idxs, parity_idxs);
	} else {
		for (auto& idx : data_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		for (auto& idx : parity_idxs) {
			bid2rowcol(idx, row, col);
			idx = col;
		}
		return row_code.num_of_partial_blocks_to_transfer(data_idxs, parity_idxs);
	}
}

int ProductCode::rowcol2bid(int row, int col)
{
	int bid = -1;
	if (row < k2 && col < k1) {	 // data block
		bid = row * k1 + col;
	} else if (row < k2 && col >= k1) {		// row parity block
		bid = k1 * k2 + row * m1 + (col - k1);
	} else if (row >= k2 && col < k1) {		// column parity block
		bid = (k1 + m1) * k2 + (row - k2) * k1 + col;
	} else {	// global parity block
		bid = (k1 + m1) * k2 + k1 * m2 + (row - k2) * m1 + (col - k1);
	}
	return bid;
}

void ProductCode::bid2rowcol(int bid, int &row, int &col)
{
	if (bid < k1 * k2) {	// data block
		row = bid / k1;
		col = bid % k1;
	} else if (bid >= k1 * k2 && bid < (k1 + m1) * k2) {	// row parity block
		int tmp_id = bid - k1 * k2;
		row = tmp_id / m1;
		col = tmp_id % m1 + k1;
	} else if (bid >= (k1 + m1) * k2 && bid < (k1 + m1) * k2 + k1 * m2) {	// column parity block
		int tmp_id = bid - (k1 + m1) * k2;
		row = tmp_id / k1 + k2;
		col = tmp_id % k1;
	} else {	// global parity block
		int tmp_id = bid - (k1 + m1) * k2 - k1 * m2;
		row = tmp_id / m1 + k2;
		col = tmp_id % m1 + k1;
	}
}

int ProductCode::oldbid2newbid_for_merge(int old_block_id, int x, int seri_num, bool isvertical)
{
	int new_block_id = -1;
	int row = -1, col = -1;
	bid2rowcol(old_block_id, row, col);
	if (isvertical) {	// for data blocks and row parity blocks
		row += seri_num * k2;
		ProductCode pc(k1, m1, x * k2, m2);
		new_block_id = pc.rowcol2bid(row, col);
	} else {	// for data blocks and column parity blocks
		col += seri_num * k1;
		ProductCode pc(x * k1, m1, k2, m2);
		new_block_id = pc.rowcol2bid(row, col);
	}
	return new_block_id;
}

void ProductCode::partition_flat()
{
	row_code.partition_plan.clear();
	int n = k + m;
  for (int i = 0; i < n; i++) {
    partition_plan.push_back({i});
  }
	for (int i = 0; i < k1 + m1; i++) {
		row_code.partition_plan.push_back({i});
	}
}

void ProductCode::partition_random()
{
	row_code.partition_plan.clear();
	int n = k1 + m1;
	std::vector<int> columns;
  for (int i = 0; i < n; i++) {
    columns.push_back(i);
  }

	int cnt = 0;
  int cnt_left = n;
  while (cnt < n) {
		// at most every m1 columns of blocks in a partition
    int random_columns_num = random_range(1, m1);  
    int columns_num = std::min(random_columns_num, n - cnt);
    std::vector<int> partition;
		std::vector<int> row_partition;
    for (int i = 0; i < columns_num; i++, cnt++) {
      int ran_idx = random_index(n - cnt);
      int col = columns[ran_idx];
      for (int row = 0; row < k2 + m2; row++) {
				int block_idx = rowcol2bid(row, col);
				partition.push_back(block_idx);
			}
			row_partition.push_back(col);
      auto it = std::find(columns.begin(), columns.end(), col);
      columns.erase(it);
    }
    partition_plan.push_back(partition);
		row_code.partition_plan.push_back(row_partition);
  }
}

void ProductCode::partition_optimal()
{
	row_code.partition_plan.clear();
	int n = k1 + m1;
	int cnt = 0;
	while (cnt < n) {
		// every m columns of blocks in a partition
		int columns_num = std::min(m1, n - cnt);
		std::vector<int> partition;
		std::vector<int> row_partition;
    for(int i = 0; i < columns_num; i++, cnt++) {
			for (int row = 0; row < k2 + m2; row++) {
				int block_idx = rowcol2bid(row, cnt);
				partition.push_back(block_idx);
			}
			row_partition.push_back(cnt);
		}
		partition_plan.push_back(partition);
		row_code.partition_plan.push_back(row_partition);
	}
}

std::string ProductCode::self_information()
{
	return "PC(" + std::to_string(k1) + "," + std::to_string(m1) + "," + \
				 std::to_string(k2) + "," + std::to_string(m2) + ")";
}

std::string ProductCode::type()
{
	return "PC";
}

bool ProductCode::generate_repair_plan(std::vector<int> failure_idxs,
																			 std::vector<RepairPlan>& plans,
																			 bool partial_scheme,
																			 bool repair_priority,
																			 bool repair_method)
{
	int failed_num = (int)failure_idxs.size();
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1 + m1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				RepairPlan plan;
				plan.local_or_column = true;
				int cnt = 0;
				std::vector<int> help_block;
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (cnt == k2) {
						break;
					}
					if (!failed_map[jj][i]) {
						help_block.push_back(rowcol2bid(jj, i));
						cnt++;
					}
				}
				if (placement_rule == FLAT) {
					for (auto block : help_block) {
						plan.help_blocks.push_back({block});
					}
				} else {
					plan.help_blocks.push_back(help_block);
				}

				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						plan.failure_idxs.push_back(rowcol2bid(jj, i));
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				plans.push_back(plan);
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2 + m2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				RepairPlan plan;
				plan.local_or_column = false;
				std::vector<int> tmp_failure_idxs;
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						tmp_failure_idxs.push_back(jj);
					}
				}
				row_code.help_blocks_for_multi_blocks_repair(tmp_failure_idxs,
						plan.parity_idxs, plan.help_blocks, partial_scheme);
				int par_num = (int)plan.help_blocks.size();
				for (int ii = 0; ii < par_num; ii++) {
					for (auto it = plan.help_blocks[ii].begin(); it != plan.help_blocks[ii].end(); it++) {
						*it = rowcol2bid(i, *it);
					}
				}

				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						plan.failure_idxs.push_back(rowcol2bid(i, jj));
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				plans.push_back(plan);
				break;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return false;
		}
	}
	return true;
}

bool ProductCode::generate_flow_repair_plan(std::vector<int> failure_idxs,
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
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1 + m1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				RepairPlan plan;
				plan.local_or_column = true;
				int cnt = 0;
				std::vector<int> help_block;
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (cnt == k2) {
						break;
					}
					if (!failed_map[jj][i]) {
						help_block.push_back(rowcol2bid(jj, i));
						cnt++;
					}
				}
				if (placement_rule == FLAT) {
					for (auto block : help_block) {
						plan.help_blocks.push_back({block});
					}
				} else {
					plan.help_blocks.push_back(help_block);
				}

				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						plan.failure_idxs.push_back(rowcol2bid(jj, i));
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				plans.push_back(plan);
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2 + m2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				RepairPlan plan;
				plan.local_or_column = false;
				std::vector<int> tmp_failure_idxs;
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						tmp_failure_idxs.push_back(jj);
					}
				}
				row_code.help_blocks_for_multi_blocks_repair(tmp_failure_idxs,
						plan.parity_idxs, plan.help_blocks, partial_scheme);
				int par_num = (int)plan.help_blocks.size();
				for (int ii = 0; ii < par_num; ii++) {
					for (auto it = plan.help_blocks[ii].begin(); it != plan.help_blocks[ii].end(); it++) {
						*it = rowcol2bid(i, *it);
					}
				}

				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						plan.failure_idxs.push_back(rowcol2bid(i, jj));
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				plans.push_back(plan);
				break;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return false;
		}
	}
	return true;
}

void HVPC::init_coding_parameters(CodingParameters cp)
{
	k1 = cp.k1;
	m1 = cp.m1;
	k2 = cp.k2;
	m2 = cp.m2;
	k = k1 * k2;
	m = k1 * m2 + k2 * m1;
	row_code.k = k1;
	row_code.m = m1;
	col_code.k = k2;
	col_code.m = m2;
	local_or_column = cp.local_or_column;
}

/*
	data_ptrs: ordered by row
	D(0) ... D(k1*k2-1)
	coding_ptrs: ordered by row
	R(0) ... R(k2*m1-1) C(0) ...  C(m2*k1-1)
*/
void HVPC::encode(char **data_ptrs, char **coding_ptrs, int block_size)
{
	// encode row parities
	for (int i = 0; i < k2; i++) {
		std::vector<char *> t_coding(m1);
		char **coding = (char **)t_coding.data();
		for (int j = 0; j < m1; j++) {
			coding[j] = coding_ptrs[i * m1 + j];
		}
		row_code.encode(&data_ptrs[i * k1], coding, block_size);
	}
	// encode column parities
	for (int i = 0; i < k1; i++) {
		std::vector<char *> t_data(k2);
		char **data = (char **)t_data.data();
		for (int j = 0; j < k2; j++) {
			data[j] = data_ptrs[j * k1 + i];
		}
		std::vector<char *> t_coding(m2);
		char **coding = (char **)t_coding.data();
		for (int j = 0; j < m2; j++) {
			coding[j] = coding_ptrs[k2 * m1 + j * k1 + i];
		}
		col_code.encode(data, coding, block_size);
	}
}

// the index order is the same as shown above
void HVPC::decode(char **data_ptrs, char **coding_ptrs, int block_size,
								 int *erasures, int failed_num)
{
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<std::vector<char *>> blocks_map(k2 + m2, std::vector<char *>(k1 + m1, nullptr));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	int data_idx = 0;
	int parity_idx = 0;
	for (int i = 0; i < k2; i++) {
		for (int j = 0; j < k1 + m1; j++) {
			if (j < k1) {
				blocks_map[i][j] = data_ptrs[data_idx++];
			} else {
				blocks_map[i][j] = coding_ptrs[parity_idx++];
			}
		}
	}
	for (int i = k2; i < k2 + m2; i++) {
		for (int j = 0; j < k1; j++) {
			blocks_map[i][j] = coding_ptrs[parity_idx++];
		}
	}

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = erasures[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				std::vector<char *> data(k2, nullptr);
				std::vector<char *> coding(m2, nullptr);
				int *erasure = new int[fb_col_cnt[i] + 1];
				erasure[fb_col_cnt[i]] = -1;
				int cnt = 0;
				for (int jj = 0; jj < k2; jj++) {
					if (failed_map[jj][i]) {
						erasure[cnt++] = jj;
					}
					data[jj] = blocks_map[jj][i];
				}
				for (int jj = 0; jj < m2; jj++) {
					if (failed_map[jj + k2][i]) {
						erasure[cnt++] = jj + k2;
					}
					coding[jj] = blocks_map[jj + k2][i];
				}
				col_code.decode(data.data(), coding.data(), block_size, erasure, fb_col_cnt[i]);
				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				delete erasure;
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				std::vector<char *> data(k1, nullptr);
				std::vector<char *> coding(m1, nullptr);
				int *erasure = new int[fb_row_cnt[i] + 1];
				erasure[fb_row_cnt[i]] = -1;
				int cnt = 0;
				for (int jj = 0; jj < k1; jj++) {
					if (failed_map[i][jj]) {
						erasure[cnt++] = jj;
					}
					data[jj] = blocks_map[i][jj];
				}
				for (int jj = 0; jj < m1; jj++) {
					if (failed_map[i][jj + k1]) {
						erasure[cnt++] = jj + k1;
					}
					coding[jj] = blocks_map[i][jj + k1];
				}
				row_code.decode(data.data(), coding.data(), block_size, erasure, fb_row_cnt[i]);
				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				delete erasure;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return;
		}
	}
}

// the index order is the same as shown above
bool HVPC::check_if_decodable(std::vector<int> failure_idxs)
{
	int failed_num = (int)failure_idxs.size();
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++)
	{
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0)
	{
		// part one
		for (int i = 0; i < k1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) { // repair on column
				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) {// repair on row
				max_row = i;
				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
			}
		}
		if (max_row == -1) {
			return false;
		}
	}
	return true;
}

void HVPC::partition_random()
{
	row_code.partition_plan.clear();
	int n = k1 + m1;
	std::vector<int> columns;
  for (int i = 0; i < n; i++) {
    columns.push_back(i);
  }

	int cnt = 0;
  int cnt_left = n;
  while (cnt < n) {
		// at most every m1 columns of blocks in a partition
    int random_columns_num = random_range(1, m1);  
    int columns_num = std::min(random_columns_num, n - cnt);
    std::vector<int> partition;
		std::vector<int> row_partition;
    for (int i = 0; i < columns_num; i++, cnt++) {
      int ran_idx = random_index(n - cnt);
      int col = columns[ran_idx];
			if (col < k1) {
				for (int row = 0; row < k2 + m2; row++) {
					int block_idx = rowcol2bid(row, col);
					partition.push_back(block_idx);
				}
			} else {
				for (int row = 0; row < k2; row++) {
					int block_idx = rowcol2bid(row, col);
					partition.push_back(block_idx);
				}
			}
			row_partition.push_back(col);
      auto it = std::find(columns.begin(), columns.end(), col);
      columns.erase(it);
    }
    partition_plan.push_back(partition);
		row_code.partition_plan.push_back(row_partition);
  }
}

void HVPC::partition_optimal()
{
	row_code.partition_plan.clear();
	int n = k1 + m1;
	int cnt = 0;
	while (cnt < n) {
		// every m columns of blocks in a partition
		int columns_num = std::min(m1, n - cnt);
		std::vector<int> partition;
		std::vector<int> row_partition;
    for(int i = 0; i < columns_num; i++, cnt++) {
			if (cnt < k1) {
				for (int row = 0; row < k2 + m2; row++) {
					int block_idx = rowcol2bid(row, cnt);
					partition.push_back(block_idx);
				}
			} else {
				for (int row = 0; row < k2; row++) {
					int block_idx = rowcol2bid(row, cnt);
					partition.push_back(block_idx);
				}
			}
			row_partition.push_back(cnt);
		}
		partition_plan.push_back(partition);
		row_code.partition_plan.push_back(row_partition);
	}
}

std::string HVPC::self_information()
{
	return "HVPC(" + std::to_string(k1) + "," + std::to_string(m1) + "," + \
				 std::to_string(k2) + "," + std::to_string(m2) + ")";
}

std::string HVPC::type()
{
	return "HVPC";
}

bool HVPC::generate_repair_plan(std::vector<int> failure_idxs,
																std::vector<RepairPlan>& plans,
																bool partial_scheme,
																bool repair_priority,
																bool repair_method)
{
	int failed_num = (int)failure_idxs.size();
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				RepairPlan plan;
				plan.local_or_column = true;
				int cnt = 0;
				std::vector<int> help_block;
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (cnt == k2) {
						break;
					}

					if (!failed_map[jj][i]) {
						help_block.push_back(rowcol2bid(jj, i));
						cnt++;
					}
				}
				if (placement_rule == FLAT) {
					for (auto block : help_block) {
						plan.help_blocks.push_back({block});
					}
				} else {
					plan.help_blocks.push_back(help_block);
				}

				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						plan.failure_idxs.push_back(rowcol2bid(jj, i));
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				plans.push_back(plan);
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				RepairPlan plan;
				plan.local_or_column = false;
				std::vector<int> tmp_failure_idxs;
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						tmp_failure_idxs.push_back(jj);
					}
				}
				row_code.help_blocks_for_multi_blocks_repair(tmp_failure_idxs,
						plan.parity_idxs, plan.help_blocks, partial_scheme);
				int par_num = (int)plan.help_blocks.size();
				for (int ii = 0; ii < par_num; ii++) {
					for (auto it = plan.help_blocks[ii].begin(); it != plan.help_blocks[ii].end(); it++) {
						*it = rowcol2bid(i, *it);
					}
				}

				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						plan.failure_idxs.push_back(rowcol2bid(i, jj));
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				plans.push_back(plan);
				break;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return false;
		}
	}
	return true;
}

bool HVPC::generate_flow_repair_plan(std::vector<int> failure_idxs,
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
	std::vector<std::vector<int>> failed_map(k2 + m2, std::vector<int>(k1 + m1, 0));
	std::vector<int> fb_row_cnt(k2 + m2, 0);
	std::vector<int> fb_col_cnt(k1 + m1, 0);

	for (int i = 0; i < failed_num; i++) {
		int row = -1, col = -1;
		int failed_idx = failure_idxs[i];
		bid2rowcol(failed_idx, row, col);
		failed_map[row][col] = 1;
		fb_row_cnt[row]++;
		fb_col_cnt[col]++;
	}

	while (failed_num > 0) {
		// part one
		for (int i = 0; i < k1; i++) {
			if (fb_col_cnt[i] <= m2 && fb_col_cnt[i] > 0) {	// repair on column
				RepairPlan plan;
				plan.local_or_column = true;
				int cnt = 0;
				std::vector<int> help_block;
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (cnt == k2) {
						break;
					}

					if (!failed_map[jj][i]) {
						help_block.push_back(rowcol2bid(jj, i));
						cnt++;
					}
				}
				if (placement_rule == FLAT) {
					for (auto block : help_block) {
						plan.help_blocks.push_back({block});
					}
				} else {
					plan.help_blocks.push_back(help_block);
				}

				// update failed_map
				for (int jj = 0; jj < k2 + m2; jj++) {
					if (failed_map[jj][i]) {
						plan.failure_idxs.push_back(rowcol2bid(jj, i));
						failed_map[jj][i] = 0;
						failed_num -= 1;
						fb_row_cnt[jj] -= 1;
						fb_col_cnt[i] -= 1;
					}
				}
				plans.push_back(plan);
			}
		}
		if (failed_num == 0) {
			break;
		}
		// part two
		int max_row = -1;
		for (int i = 0; i < k2; i++) {
			if (fb_row_cnt[i] <= m1 && fb_row_cnt[i] > 0) { // repair on row
				max_row = i;
				RepairPlan plan;
				plan.local_or_column = false;
				std::vector<int> tmp_failure_idxs;
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						tmp_failure_idxs.push_back(jj);
					}
				}
				row_code.help_blocks_for_multi_blocks_repair(tmp_failure_idxs,
						plan.parity_idxs, plan.help_blocks, partial_scheme);
				int par_num = (int)plan.help_blocks.size();
				for (int ii = 0; ii < par_num; ii++) {
					for (auto it = plan.help_blocks[ii].begin(); it != plan.help_blocks[ii].end(); it++) {
						*it = rowcol2bid(i, *it);
					}
				}

				// update failed_map
				for (int jj = 0; jj < k1 + m1; jj++) {
					if (failed_map[i][jj]) {
						plan.failure_idxs.push_back(rowcol2bid(i, jj));
						failed_map[i][jj] = 0;
						failed_num -= 1;
						fb_row_cnt[i] -= 1;
						fb_col_cnt[jj] -= 1;
					}
				}
				plans.push_back(plan);
				break;
			}
		}
		if (max_row == -1) {
			std::cout << "Undecodable!!" << std::endl;
			return false;
		}
	}
	return true;
}