#pragma once

#include "jerasure.h"
#include "reed_sol.h"
#include "cauchy.h"
#include "erasure_code.h"
#include "rs.h"

namespace ECProject
{
	class ProductCode : public ErasureCode
	{
	public:
		RSCode row_code;
		RSCode col_code;
		int k1;
		int m1;
		int k2;
		int m2;

		ProductCode() {}
		ProductCode(int k1, int m1, int k2, int m2)
			: ErasureCode(k1 * k2, (k1 + m1) * (k2 + m2) - k1 * k2),
				row_code(k1, m1), col_code(k2, m2), k1(k1), m1(m1), k2(k2), m2(m2) {}
		~ProductCode() override {}

		void init_coding_parameters(CodingParameters cp) override;
		void get_coding_parameters(CodingParameters& cp) override;

		void encode(char **data_ptrs, char **coding_ptrs, int block_size) override;
		void decode(char **data_ptrs, char **coding_ptrs, int block_size,
								int *erasures, int failed_num) override;
		bool check_if_decodable(std::vector<int> failure_idxs) override;

		void make_encoding_matrix(int *final_matrix) override {}
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
		
		int rowcol2bid(int row, int col);
		void bid2rowcol(int block_id, int &row, int &col);
		virtual int oldbid2newbid_for_merge(
				int old_block_id, int x, int seri_num, bool isvertical);

		void partition_flat() override;
		void partition_random() override;
		void partition_optimal() override;

		std::string self_information() override;
		std::string type() override;

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

	/* ProductCode without global parity blocks */
	class HVPC : public ProductCode
	{
	public:
		HVPC() {}
		HVPC(int k1, int m1, int k2, int m2)
			: ProductCode(k1, m1, k2, m2)
		{
			m = k1 * m2 + k2 * m1;
		}
		~HVPC() override {}

		void init_coding_parameters(CodingParameters cp) override;
		void encode(char **data_ptrs, char **coding_ptrs, int block_size) override;
		void decode(char **data_ptrs, char **coding_ptrs, int block_size,
								int *erasures, int failed_num) override;
		bool check_if_decodable(std::vector<int> failure_idxs) override;

		void partition_random() override;
		void partition_optimal() override;

		std::string self_information() override;
		std::string type() override;
		
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
