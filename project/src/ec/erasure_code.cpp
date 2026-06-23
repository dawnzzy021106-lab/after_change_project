#include "erasure_code.h"
#include <mutex>

using namespace ECProject;

namespace
{
  std::mutex g_erasure_print_info_mutex;
}

void ErasureCode::init_coding_parameters(CodingParameters cp)
{
	k = cp.k;
	m = cp.m;
	local_or_column = cp.local_or_column;
}

void ErasureCode::get_coding_parameters(CodingParameters& cp)
{
	cp.k = k;
	cp.m = m;
	cp.local_or_column = local_or_column;
}

void ErasureCode::print_matrix(int *matrix, int rows, int cols, std::string msg)
{
	std::cout << msg << ":" << std::endl;
	for (int i = 0; i < rows; i++) {
		for (int j = 0; j < cols; j++) {
			std::cout << matrix[i * cols + j] << " ";
		}
		std::cout << std::endl;
	}
}

void ErasureCode::get_identity_matrix(int *matrix, int rows, int kk)
{
	for (int i = 0; i < rows; i++) {
		matrix[i * kk + i] = 1;
	}
}

void ErasureCode::make_full_matrix(int *matrix, int kk)
{
	std::vector<int> encoding_matrix(m * k, 0);
	make_encoding_matrix(encoding_matrix.data());
	for (int i = 0; i < m; i++) {
		memcpy(&matrix[i * kk], &encoding_matrix[i * k], k * sizeof(int));
	}
}

void ErasureCode::make_submatrix_by_rows(int cols, int *matrix, int *new_matrix,
										 										 std::vector<int> block_idxs)
{
	int i = 0;
	for (auto it = block_idxs.begin(); it != block_idxs.end(); it++) {
		int j = *it;
		memcpy(&new_matrix[i * cols], &matrix[j * cols], cols * sizeof(int));
		i++;
	}
}

void ErasureCode::make_submatrix_by_cols(int cols, int rows,
																				 int *matrix, int *new_matrix,
																				 std::vector<int> block_idxs)
{
	int block_num = int(block_idxs.size());
	int i = 0;
	for (auto it = block_idxs.begin(); it != block_idxs.end(); it++) {
		int j = *it;
		for (int u = 0; u < rows; u++) {
			new_matrix[u * block_num + i] = matrix[u * cols + j];
		}
		i++;
	}
}

/*
	take an example:
	data_ptrs = [A, B, C, D, E, F, G, H, I, J]
	data_idxs = [8, 5, 5, 7, 6, 7, 8, 6, 5, 7]
	parity_idxs = [5, 6, 7, 8]
	coding_ptrs = [P1, P2, P3, P4]
	then P1 = B + C + I, P2 = E + H, P3 = D + F, P4 = A + G
*/
void ErasureCode::perform_addition(char **data_ptrs, char **coding_ptrs,
		int block_size, const std::vector<int>& data_idxs, const std::vector<int>& parity_idxs)
{
	int block_num = int(data_idxs.size());
  int parity_num = int(parity_idxs.size());
	std::vector<char *> t_data(block_num);
  char **data = (char **)t_data.data();
  for (int i = 0; i < parity_num; i++) {
		int cnt = 0;
		for (int j = 0; j < block_num; j++) {
			if (parity_idxs[i] == data_idxs[j]) {
				data[cnt++] = data_ptrs[j];
			}
		}
		std::vector<int> new_matrix(1 * cnt, 1);
		jerasure_matrix_encode(cnt, 1, w, new_matrix.data(), data,
													 &coding_ptrs[i], block_size);
	}
}

// any parity_idx >= k
// calculate partial blocks of selected parity blocks
void ErasureCode::encode_partial_blocks_for_parities_(int k_,
		int *full_matrix, char **data_ptrs, char **coding_ptrs, int block_size,
		const std::vector<int>& data_idxs, const std::vector<int>& parity_idxs)
{
	int block_num = int(data_idxs.size());
	int parity_num = int(parity_idxs.size());
	std::vector<int> matrix(parity_num * k_, 0);
	make_submatrix_by_rows(k_, full_matrix, matrix.data(), parity_idxs);

	std::vector<int> new_matrix(parity_num * block_num, 0);
	make_submatrix_by_cols(k_, parity_num, matrix.data(), new_matrix.data(), data_idxs);
	// print_matrix(new_matrix.data(), parity_num, block_num, "new_matrix");

  jerasure_matrix_encode(block_num, parity_num, w, new_matrix.data(), data_ptrs,
												 coding_ptrs, block_size);
}

// get inverse matrix of f rows (corresponding to failures) of encoding matrix
// then decode the failures with partial blocks of selected parity blocks
void ErasureCode::decode_with_partial_blocks_(int k_,
		int *full_matrix, char **data_ptrs, char **coding_ptrs, int block_size,
		const std::vector<int>& failure_idxs, const std::vector<int>& parity_idxs)
{
	int failures_num = int(failure_idxs.size());
	int parity_num = int(parity_idxs.size());
	my_assert(failures_num <= parity_num);
	std::vector<int> matrix(parity_num * k_, 0);
	make_submatrix_by_rows(k_, full_matrix, matrix.data(), parity_idxs);

	std::vector<int> new_matrix(parity_num * failures_num, 0);
	make_submatrix_by_cols(k_, failures_num, matrix.data(), new_matrix.data(), failure_idxs);
	// print_matrix(new_matrix.data(), parity_num, failures_num, "decode_matrix");
	
	std::vector<int> inverse_matrix(parity_num * failures_num, 0);
	jerasure_invert_matrix(new_matrix.data(), inverse_matrix.data(), failures_num, w);
	jerasure_matrix_encode(parity_num, failures_num, w, inverse_matrix.data(),
												 coding_ptrs, data_ptrs, block_size);
}

// get inverse matrix of f rows (corresponding to failures) of encoding matrix
// then calculate partial blocks of failures
void ErasureCode::encode_partial_blocks_for_failures_(int k_,
		int *full_matrix, char **data_ptrs, char **coding_ptrs, int block_size,
		const std::vector<int>& data_idxs, const std::vector<int>& parity_idxs,
		const std::vector<int>& failure_idxs)
{
	int failures_num = int(failure_idxs.size());
	int parity_num = int(parity_idxs.size());
	my_assert(failures_num == parity_num);
	std::vector<int> matrix(parity_num * k_, 0);
	make_submatrix_by_rows(k_, full_matrix, matrix.data(), parity_idxs);

	std::vector<int> new_matrix(parity_num * failures_num, 0);
	make_submatrix_by_cols(k_, parity_num, matrix.data(), new_matrix.data(), failure_idxs);
	// print_matrix(new_matrix.data(), parity_num, failures_num, "failures_matrix");
	
	std::vector<int> inverse_matrix(parity_num * failures_num, 0);
	jerasure_invert_matrix(new_matrix.data(), inverse_matrix.data(), failures_num, w);

	int datas_num = int(data_idxs.size());
	std::vector<int> datas_matrix(parity_num * datas_num, 0);
	make_submatrix_by_cols(k_, parity_num, matrix.data(), datas_matrix.data(), data_idxs);
	// print_matrix(datas_matrix.data(), parity_num, datas_num, "datas_matrix");
	
	int *encoding_matrix = jerasure_matrix_multiply(inverse_matrix.data(),
																									datas_matrix.data(),
																									parity_num, failures_num, parity_num, datas_num, w);
	jerasure_matrix_encode(datas_num, failures_num, w, encoding_matrix,
												 data_ptrs, coding_ptrs, block_size);
	free(encoding_matrix);
}

// another version, must be repaired with k surviving blocks (unadopted)
// get inverse matrix of k rows (corresponding to  selected survivors) of the encoding matrix
// then calculate partial blocks of failures
void ErasureCode::encode_partial_blocks_for_failures_v2_(int k_,
		int *full_matrix, char **data_ptrs, char **coding_ptrs, int block_size,
		const std::vector<int>& data_idxs, const std::vector<int>& failure_idxs,
		const std::vector<int>& live_idxs)
{
	int datas_num = int(data_idxs.size());
	int failures_num = int(failure_idxs.size());
	int live_num = int(live_idxs.size());
	my_assert(k_ == live_num);
	std::vector<int> failures_matrix(failures_num * k_, 0);
	std::vector<int> survivors_matrix(k_ * k_, 0);
	make_submatrix_by_rows(k_, full_matrix, failures_matrix.data(), failure_idxs);
	make_submatrix_by_rows(k_, full_matrix, survivors_matrix.data(), live_idxs);
	// print_matrix(failures_matrix.data(), failures_num, k_, "failures_matrix");
	// print_matrix(survivors_matrix.data(), k_, k_, "survivors_matrix");

	std::vector<int> inverse_matrix(k_ * k_, 0);
	jerasure_invert_matrix(survivors_matrix.data(), inverse_matrix.data(), k_, w);
	// print_matrix(inverse_matrix.data(), k_, k_, "inverse_matrix");
	
	int *decoding_matrix = jerasure_matrix_multiply(failures_matrix.data(),
																									inverse_matrix.data(),
																									failures_num, k_, k_, k_, w);
	std::vector<int> encoding_matrix(failures_num * datas_num, 0);
	int i = 0;
	for (auto it2 = data_idxs.begin(); it2 != data_idxs.end(); it2++, i++) {
		int idx = 0;
		for (auto it3 = live_idxs.begin(); it3 != live_idxs.end(); it3++, idx++) {
			if(*it2 == *it3)
				break;
		}

		for (int u = 0; u < failures_num; u++) {
			encoding_matrix[u * datas_num + i] = decoding_matrix[u * k_ + idx];
		}
	}
	jerasure_matrix_encode(datas_num, failures_num, w, encoding_matrix.data(),
												 data_ptrs, coding_ptrs, block_size);
	free(decoding_matrix);
}

void ErasureCode::partition_flat()
{
	int n = k + m;
  for(int i = 0; i < n; i++) {
    partition_plan.push_back({i});
  }
}

void ErasureCode::generate_partition()
{
	partition_plan.clear();
	if (placement_rule == FLAT) {
		partition_flat();
	} else if (placement_rule == RANDOM) {
		partition_random();
	} else if (placement_rule == OPTIMAL) {
		partition_optimal();
	} 
}

void ErasureCode::print_info(std::vector<std::vector<int>> info, std::string info_str)
{
	std::lock_guard<std::mutex> lk(g_erasure_print_info_mutex);
	if (info_str == "partition") {
		std::string placement_type = "_flat";
		if (placement_rule == RANDOM) {
			placement_type = "_random";
		} else if (placement_rule == OPTIMAL) {
			placement_type = "_optimal";
		}
		info_str += placement_type;
	}
	
	std::cout << info_str << " result:\n";
	int cnt = 0;
	for (auto& vec : info) {
		std::cout << cnt++ << ": ";
		for (int ele : vec) {
			std::cout << ele << " ";
		}
		std::cout << "\n";
	}
}
