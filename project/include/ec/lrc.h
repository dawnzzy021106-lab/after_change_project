#pragma once

#include "jerasure.h"
#include "reed_sol.h"
#include "cauchy.h"
#include "erasure_code.h"

namespace ECProject
{
	class LocallyRepairableCode : public ErasureCode
	{
	public:
		int l;	/* number of local parity blocks */
		int g;	/* number of global parity blocks */
		int r;	/* regular group size */

		LocallyRepairableCode() = default;
		LocallyRepairableCode(int k, int l, int g)
			: ErasureCode(k, l + g), l(l), g(g)
		{
			my_assert(l > 0);
  		r = (k + l - 1) / l;
		}
		~LocallyRepairableCode() override {}

		void init_coding_parameters(CodingParameters cp) override;
		void get_coding_parameters(CodingParameters& cp) override;

		void encode(char **data_ptrs, char **coding_ptrs, int block_size) override;
		void decode(char **data_ptrs, char **coding_ptrs, int block_size,
								int *erasures, int failed_num) override;
		void decode_global(char **data_ptrs, char **coding_ptrs, int block_size,
										int *erasures, int failed_num);
		void decode_local(char **data_ptrs, char **coding_ptrs, int block_size,
										int *erasures, int failed_num, int group_id);
		void encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme) override;
		virtual void encode_partial_blocks_local(
				char **data_ptrs, char **coding_ptrs, int block_size,
				const std::vector<int> data_idxs, const std::vector<int> parity_idxs,
				const std::vector<int> failure_idxs);
		void decode_with_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> failure_idxs, std::vector<int> parity_idxs) override;
		int num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs) override;

		// check if decodable
		bool check_if_decodable(std::vector<int> failure_idxs) override { return true; }
		// full encoding matrix, with demensions as (g + l) × k
		void make_encoding_matrix(int *final_matrix) override {}
		void make_full_matrix(int *matrix, int kk) override;
		// encoding matrix for a single local parity, with demensions as 1 × group_size
		virtual void make_group_matrix(int *group_matrix, int group_id) {}
		// check the validation of coding parameters
		virtual bool check_parameters() = 0;
		// get group_id via block_id
		virtual int bid2gid(int block_id) = 0;
		// get encoding index in a group via block_id
		virtual int idxingroup(int block_id) = 0;
		// get the size of the group-id-th group
		virtual int get_group_size(int group_id, int& min_idx) { return 0; } 

		virtual void grouping_information(std::vector<std::vector<int>>& groups) {}
		void partition_random() override;
		void partition_optimal() override;

		std::string self_information() override { return ""; }
		std::string type() override { return ""; }

		virtual void help_blocks_for_single_block_repair(
						int failure_idx,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme);
		virtual void help_blocks_for_multi_global_blocks_repair(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme);
		virtual void repair_multi_data_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks);
		virtual void repair_multi_data_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks);
		virtual void repair_multi_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks);
		virtual void repair_multi_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks);
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
		virtual bool repair_multi_blocks_method1(std::vector<int> failure_idxs,
																						 std::vector<RepairPlan>& plans,
																						 bool partial_scheme,
																						 bool repair_priority);
		virtual bool repair_multi_blocks_method2(std::vector<int> failure_idxs,
																						 std::vector<RepairPlan>& plans,
																 						 bool partial_scheme,
																						 bool repair_priority);
	};
	
	/* Microsoft's Azure LRC */
	class Azu_LRC : public LocallyRepairableCode
	{
	public:
		Azu_LRC() {}
		Azu_LRC(int k, int l, int g) : LocallyRepairableCode(k, l, g)
		{
			my_assert(l > 0);
  		r = (k + l - 1) / l;
		}
		~Azu_LRC() override {}

		bool check_if_decodable(std::vector<int> failure_idxs) override;
		void make_encoding_matrix(int *final_matrix) override;
		void make_group_matrix(int *group_matrix, int group_id) override;
		bool check_parameters() override;
		int bid2gid(int block_id) override;
		int idxingroup(int block_id) override;
		int get_group_size(int group_id, int& min_idx) override;

		void grouping_information(std::vector<std::vector<int>>& groups) override;
		void partition_optimal_v2();

		std::string self_information() override;
		std::string type() override;
	};

	/* Microsoft's Azure LRC + 1 */
	class Azu_LRC_1 : public LocallyRepairableCode
	{
	public:
		Azu_LRC_1() {}
		Azu_LRC_1(int k, int l, int g) : LocallyRepairableCode(k, l, g)
		{
			my_assert(l > 1);
  		r = (k + l - 2) / (l - 1);
		}
		~Azu_LRC_1() override {}

		bool check_if_decodable(std::vector<int> failure_idxs) override;
		void make_encoding_matrix(int *final_matrix) override;
		void make_group_matrix(int *group_matrix, int group_id) override;
		bool check_parameters() override;
		int bid2gid(int block_id) override;
		int idxingroup(int block_id) override;
		int get_group_size(int group_id, int& min_idx) override;

		void grouping_information(std::vector<std::vector<int>>& groups) override;
		void partition_optimal() override;

		void repair_multi_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;

		std::string self_information() override;
		std::string type() override;
	};

	/* Optimal LRC */
	class Opt_LRC : public LocallyRepairableCode
	{
	public:
		Opt_LRC() {}
		Opt_LRC(int k, int l, int g) : LocallyRepairableCode(k, l, g)
		{
			my_assert(l > 0);
  		r = (k + g + l - 1) / l;
		}
		~Opt_LRC() override {}

		bool check_if_decodable(std::vector<int> failure_idxs) override;
		void make_encoding_matrix(int *final_matrix) override;
		void make_encoding_matrix_v2(int *final_matrix);
		void make_group_matrix(int *group_matrix, int group_id) override;
		bool check_parameters() override;
		int bid2gid(int block_id) override;
		int idxingroup(int block_id) override;
		int get_group_size(int group_id, int& min_idx) override;

		void grouping_information(std::vector<std::vector<int>>& groups) override;
		void partition_optimal() override;

		void repair_multi_data_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_data_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;

		std::string self_information() override;
		std::string type() override;
	};

	/* Optimal Cauchy LRC [FAST'23, Google] */
	class Opt_Cau_LRC : public LocallyRepairableCode
	{
	public:
		std::vector<int> surviving_group_ids;

		Opt_Cau_LRC() {}
		Opt_Cau_LRC(int k, int l, int g) : LocallyRepairableCode(k, l, g)
		{
			my_assert(l > 0);
  		r = (k + l - 1) / l;
		}
		~Opt_Cau_LRC() override {}

		void encode_partial_blocks(
				char **data_ptrs, char **coding_ptrs, int block_size,
				std::vector<int> data_idxs, std::vector<int> parity_idxs,
				std::vector<int> failure_idxs, std::vector<int> live_idxs,
				std::vector<bool>& partial_flags, bool partial_scheme) override;
		void encode_partial_blocks_local(
				char **data_ptrs, char **coding_ptrs, int block_size,
				const std::vector<int> data_idxs, const std::vector<int> parity_idxs,
				const std::vector<int> failure_idxs) override;
		int num_of_partial_blocks_to_transfer(
				std::vector<int> data_idxs, std::vector<int> parity_idxs) override;

		bool check_if_decodable(std::vector<int> failure_idxs) override;
		void make_full_matrix(int *matrix, int kk) override;
		void make_encoding_matrix(int *final_matrix) override;
		void make_encoding_matrix_v2(int *final_matrix);
		void make_group_matrix(int *group_matrix, int group_id) override;
		bool check_parameters() override;
		int bid2gid(int block_id) override;
		int idxingroup(int block_id) override;
		int get_group_size(int group_id, int& min_idx) override;

		void grouping_information(std::vector<std::vector<int>>& groups) override;

		std::string self_information() override;
		std::string type() override;

		void help_blocks_for_single_block_repair(
						int failure_idx,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme) override;
		void help_blocks_for_multi_global_blocks_repair(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks,
						bool partial_scheme) override;
		void repair_multi_data_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_data_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
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
		bool repair_multi_blocks_method1(std::vector<int> failure_idxs,
																		 std::vector<RepairPlan>& plans,
																		 bool partial_scheme,
																		 bool repair_priority) override;
		bool repair_multi_blocks_method2(std::vector<int> failure_idxs,
																		 std::vector<RepairPlan>& plans,
																		 bool partial_scheme,
																		 bool repair_priority) override;
	};

	/* Uniform Cauchy LRC [FAST'23, Google] */
	class Uni_Cau_LRC : public LocallyRepairableCode
	{
	public:
		Uni_Cau_LRC() {}
		Uni_Cau_LRC(int k, int l, int g) : LocallyRepairableCode(k, l, g)
		{
			my_assert(l > 0);
  		r = (k + g + l - 1) / l;
		}
		~Uni_Cau_LRC() override {}

		bool check_if_decodable(std::vector<int> failure_idxs) override;
		void make_encoding_matrix(int *final_matrix) override;
		void make_encoding_matrix_v2(int *final_matrix);
		void make_group_matrix(int *group_matrix, int group_id) override;
		bool check_parameters() override;
		int bid2gid(int block_id) override;
		int idxingroup(int block_id) override;
		int get_group_size(int group_id, int& min_idx) override;

		void grouping_information(std::vector<std::vector<int>>& groups) override;
		void partition_optimal() override;

		void repair_multi_data_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_data_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_global_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;
		void repair_multi_blocks_with_local_priority(
						std::vector<int> failure_idxs,
						std::vector<int>& parity_idxs,
						std::vector<std::vector<int>>& help_blocks) override;

		std::string self_information() override;
		std::string type() override;
	};
}