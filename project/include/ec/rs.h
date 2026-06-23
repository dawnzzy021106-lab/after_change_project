#pragma once

#include "jerasure.h"
#include "reed_sol.h"
#include "cauchy.h"
#include "erasure_code.h"

namespace ECProject
{
	class RSCode : public ErasureCode
	{
	public:
		RSCode() {}
		RSCode(int k, int m) : ErasureCode(k, m) {}
		~RSCode() override {}

		void encode(char **data_ptrs, char **coding_ptrs, int block_size) override;
		void decode(char **data_ptrs, char **coding_ptrs, int block_size,
								int *erasures, int failed_num) override;
		bool check_if_decodable(std::vector<int> failure_idxs) override;

		void make_encoding_matrix(int *final_matrix) override;
		void encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme) override;
		void decode_with_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> failure_idxs, std::vector<int> parity_idxs) override;
		int num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs) override;

		void partition_random() override;
		void partition_optimal() override;

		std::string self_information() override;
		std::string type() override;

		void help_blocks_for_single_block_repair(
						int failure_idx,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme);

		void help_blocks_for_multi_blocks_repair(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme);
		
		void help_blocks_for_single_block_flow_repair(
						int failure_idx,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme,
						std::vector<int> main_help_clusterID,
            			std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
						int failure_stripe_ID,
						bool main_help_cluster_flag);
		
		bool generate_repair_plan(std::vector<int> failure_idxs,
															std::vector<RepairPlan>& plans,
															bool partial_scheme,
															bool repair_priority,
															bool repair_method) override;
		bool generate_flow_repair_plan(std::vector<int> failure_idxs,
															std::vector<RepairPlan>& plans,
															bool partial_scheme,
															bool repair_priority,
															bool repair_method,
                                      						std::vector<int> main_help_clusterID,
                	                  						std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs,
															int failure_stripe_ID,
															bool main_help_cluster_flag) override;
	};
}
