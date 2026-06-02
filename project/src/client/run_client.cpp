#include "client.h"
#include <unistd.h>
#include <fstream>
#include <sstream>

using namespace ECProject;

void test_single_block_repair(Client &client, int block_num)
{
  auto stripe_ids = client.list_stripes();
  int stripe_num = stripe_ids.size();
  std::vector<double> repair_times;
  std::vector<double> decoding_times;
  std::vector<double> cross_cluster_times;
  std::vector<double> meta_times;
  std::vector<int> cross_cluster_transfers;
  std::vector<int> io_cnts;
  std::cout << "Single-Block Repair:" << std::endl;
  for (int i = 0; i < stripe_num; i++) {
    std::cout << "[Stripe " << i << "]" << std::endl;
    double temp_repair = 0;
    double temp_decoding = 0;
    double temp_cross_cluster = 0;
    double temp_meta = 0;
    int temp_cc_transfers = 0;
    int temp_io_cnt = 0;
    for (int j = 0; j < block_num; j++) {
      std::vector<unsigned int> failures;
      failures.push_back((unsigned int)j);
      auto resp = client.blocks_repair(failures, stripe_ids[i]);
      temp_repair += resp.repair_time;
      temp_decoding += resp.decoding_time;
      temp_cross_cluster += resp.cross_cluster_time;
      temp_meta += resp.meta_time;
      temp_cc_transfers += resp.cross_cluster_transfers;
      temp_io_cnt += resp.io_cnt;
    }
    repair_times.push_back(temp_repair);
    decoding_times.push_back(temp_decoding);
    cross_cluster_times.push_back(temp_cross_cluster);
    meta_times.push_back(temp_meta);
    cross_cluster_transfers.push_back(temp_cc_transfers);
    io_cnts.push_back(temp_io_cnt);
    std::cout << "repair = " << temp_repair / block_num
              << "s, decoding = " << temp_decoding / block_num
              << "s, cross-cluster = " << temp_cross_cluster / block_num
              << "s, meta = " << temp_meta / block_num
              << "s, cross-cluster-count = " << (double)temp_cc_transfers / block_num
              << ", I/Os = " << temp_io_cnt / block_num
              << std::endl;
  }
  auto avg_repair = std::accumulate(repair_times.begin(),
      repair_times.end(), 0.0) / (stripe_num * block_num);
  auto avg_decoding = std::accumulate(decoding_times.begin(),
      decoding_times.end(), 0.0) / (stripe_num * block_num);
  auto avg_cross_cluster = std::accumulate(cross_cluster_times.begin(),
      cross_cluster_times.end(), 0.0) / (stripe_num * block_num);
  auto avg_meta = std::accumulate(meta_times.begin(),
      meta_times.end(), 0.0) / (stripe_num * block_num);
  auto avg_cc_transfers = (double)std::accumulate(cross_cluster_transfers.begin(),
      cross_cluster_transfers.end(), 0) / (stripe_num * block_num);
  auto avg_io_cnt = (double)std::accumulate(io_cnts.begin(),
      io_cnts.end(), 0) / (stripe_num * block_num);
  std::cout << "^-^[Average]^-^" << std::endl;
  std::cout << "repair = " << avg_repair << "s, decoding = " << avg_decoding
            << "s, cross-cluster = " << avg_cross_cluster
            << "s, meta = " << avg_meta
            << "s, cross-cluster-count = " << avg_cc_transfers
            << ", I/Os = " << avg_io_cnt
            << std::endl;
}

void test_node_repair(Client &client, int failed_node_id, const ParametersInfo& paras)
{
  std::vector<unsigned int> failed_node_ids;
  failed_node_ids.push_back((unsigned int)failed_node_id);
  int failed_node_num = 1;

  std::cout << "Node-Level Repair (Failing " << failed_node_num << " nodes):" << std::endl;
  std::cout << "Failed Node IDs: ";
  for (auto id : failed_node_ids) std::cout << id << " ";
  std::cout << std::endl;

  // 1. 在修复发生前，拍下当前集群布局的元数据快照
  client.snapshot_metadata();

  // ========================================================
  // 测试算法 1：最大流修复 (min cost max flow)
  // ========================================================
  std::cout << "\n================= min cost max flow =================" << std::endl;
  auto resp2 = client.nodes_flow_repair(failed_node_ids);

  if (resp2.repair_time > 0) {
    std::cout << "Repair Result:        " << std::endl;
    std::cout << "  Total Time:         " << resp2.repair_time << "s" << std::endl;
    std::cout << "  |- Decoding (Pure): " << resp2.decoding_time << "s" << std::endl;
    std::cout << "  |- Network (Pure):  " << resp2.cross_cluster_time << "s" << std::endl;
    std::cout << "  |- Network (Inner): " << resp2.inner_network_time << "s" << std::endl;
    std::cout << "  |- Disk I/O (Agg):  " << resp2.io_time << "s" << std::endl;
    std::cout << "  Meta (Coord):       " << resp2.meta_time << "s" << std::endl;
    std::cout << "  Cross-Cluster-Count:" << resp2.cross_cluster_transfers << std::endl;
    std::cout << "  I/Os:               " << resp2.io_cnt << std::endl;
  } else if (!resp2.success) {
    std::cout << "Flow repair failed or aborted (repair_time=0). See coordinator stderr "
                 "(e.g. matrix file, min_cost_max_flow, cluster_data.bin, plan generation)."
              << std::endl;
  } else {
    std::cout << "No blocks were found on the failed nodes to repair." << std::endl;
  }
  

  // 2. 将元数据回滚，假装上一步的修复从未发生过
  client.revert_metadata();

  // ========================================================
  // 测试算法 2：无序并发最大流（可设置批次并行度，例如 25）
  // ========================================================
  const int flow_repair_parallel_unordered = paras.flow_repair_parallel_unordered;
  client.set_flow_repair_max_parallel(flow_repair_parallel_unordered);

  std::cout << "\n================= unordered concurrency min cost max flow ================="
            << std::endl;
  auto resp3 = client.nodes_flow_unordered_concurrency_repair(failed_node_ids);

  if (resp3.repair_time > 0) {
    std::cout << "Repair Result:        " << std::endl;
    std::cout << "  Total Time:         " << resp3.repair_time << "s" << std::endl;
    std::cout << "  |- Decoding (Pure): " << resp3.decoding_time << "s" << std::endl;
    std::cout << "  |- Network (Pure):  " << resp3.cross_cluster_time << "s" << std::endl;
    std::cout << "  |- Network (Inner): " << resp3.inner_network_time << "s" << std::endl;
    std::cout << "  |- Disk I/O (Agg):  " << resp3.io_time << "s" << std::endl;
    std::cout << "  Meta (Coord):       " << resp3.meta_time << "s" << std::endl;
    std::cout << "  Cross-Cluster-Count:" << resp3.cross_cluster_transfers << std::endl;
    std::cout << "  I/Os:               " << resp3.io_cnt << std::endl;
  } else if (!resp3.success) {
    std::cout << "Flow repair failed or aborted (repair_time=0). See coordinator stderr "
                 "(e.g. matrix file, min_cost_max_flow, cluster_data.bin, plan generation)."
              << std::endl;
  } else {
    std::cout << "No blocks were found on the failed nodes to repair." << std::endl;
  }

  client.revert_metadata();

  // ========================================================
  // 测试算法 3：跨机架链路调度后的无序并发最大流（按 scheduleRepairLink 的匹配轮次发送 help_repair）
  // ========================================================
  const int flow_repair_parallel_ordered = paras.flow_repair_parallel_ordered;
  client.set_flow_repair_max_parallel(flow_repair_parallel_ordered);
  std::cout << "\n================= ordered concurrency min cost max flow ================="
            << std::endl;
  auto resp4 = client.nodes_flow_ordered_concurrency_repair(failed_node_ids);

  if (resp4.repair_time > 0) {
    std::cout << "Repair Result:        " << std::endl;
    std::cout << "  Total Time:         " << resp4.repair_time << "s" << std::endl;
    std::cout << "  |- Decoding (Pure): " << resp4.decoding_time << "s" << std::endl;
    std::cout << "  |- Network (Pure):  " << resp4.cross_cluster_time << "s" << std::endl;
    std::cout << "  |- Network (Inner): " << resp4.inner_network_time << "s" << std::endl;
    std::cout << "  |- Disk I/O (Agg):  " << resp4.io_time << "s" << std::endl;
    std::cout << "  Meta (Coord):       " << resp4.meta_time << "s" << std::endl;
    std::cout << "  Cross-Cluster-Count:" << resp4.cross_cluster_transfers << std::endl;
    std::cout << "  I/Os:               " << resp4.io_cnt << std::endl;
  } else if (!resp4.success) {
    std::cout << "Flow repair failed or aborted (repair_time=0). See coordinator stderr "
                 "(e.g. matrix file, min_cost_max_flow, cluster_data.bin, plan generation)."
              << std::endl;
  } else {
    std::cout << "No blocks were found on the failed nodes to repair." << std::endl;
  }

  client.revert_metadata();

  // ========================================================
  // 测试算法 4：跨机架链路调度 + 每轮强制 join 同步（Round-based barrier）
  // ========================================================
  const int flow_repair_parallel_join_ordered = paras.flow_repair_parallel_join_ordered;
  client.set_flow_repair_max_parallel(flow_repair_parallel_join_ordered);
  std::cout << "\n================= join ordered concurrency min cost max flow ================="
            << std::endl;
  auto resp5 = client.nodes_join_flow_ordered_concurrency_repair(failed_node_ids);

  if (resp5.repair_time > 0) {
    std::cout << "Repair Result:        " << std::endl;
    std::cout << "  Total Time:         " << resp5.repair_time << "s" << std::endl;
    std::cout << "  |- Decoding (Pure): " << resp5.decoding_time << "s" << std::endl;
    std::cout << "  |- Network (Pure):  " << resp5.cross_cluster_time << "s" << std::endl;
    std::cout << "  |- Network (Inner): " << resp5.inner_network_time << "s" << std::endl;
    std::cout << "  |- Disk I/O (Agg):  " << resp5.io_time << "s" << std::endl;
    std::cout << "  Meta (Coord):       " << resp5.meta_time << "s" << std::endl;
    std::cout << "  Cross-Cluster-Count:" << resp5.cross_cluster_transfers << std::endl;
    std::cout << "  I/Os:               " << resp5.io_cnt << std::endl;
  } else if (!resp5.success) {
    std::cout << "Flow repair failed or aborted (repair_time=0). See coordinator stderr "
                 "(e.g. matrix file, min_cost_max_flow, cluster_data.bin, plan generation)."
              << std::endl;
  } else {
    std::cout << "No blocks were found on the failed nodes to repair." << std::endl;
  }

  client.revert_metadata();

 
  // ========================================================
  // 测试算法 5：基线修复 (ec_prototype)
  // ========================================================
  
  std::cout << "\n================== ec_prototype =====================" << std::endl;
  auto resp = client.nodes_repair(failed_node_ids);

  if (resp.repair_time > 0) {
    std::cout << "Repair Result:        " << std::endl;
    std::cout << "  Total Time:         " << resp.repair_time << "s" << std::endl;
    std::cout << "  |- Decoding (Pure): " << resp.decoding_time << "s" << std::endl;
    std::cout << "  |- Network (Pure):  " << resp.cross_cluster_time << "s" << std::endl;
    std::cout << "  |- Network (Inner): " << resp.inner_network_time << "s" << std::endl;
    std::cout << "  |- Disk I/O (Agg):  " << resp.io_time << "s" << std::endl;
    std::cout << "  Meta (Coord):       " << resp.meta_time << "s" << std::endl;
    std::cout << "  Cross-Cluster-Count:" << resp.cross_cluster_transfers << std::endl;
    std::cout << "  I/Os:               " << resp.io_cnt << std::endl;
  } else if (!resp.success) {
    std::cout << "ec_prototype repair failed or aborted (repair_time=0). "
                 "This is often caused by prior RPC timeout/connection reset, "
                 "or snapshot/revert RPC failure. Check coordinator/client RPC ERROR logs."
              << std::endl;
  } else {
    std::cout << "No blocks were found on the failed nodes to repair." << std::endl;
  }

}

void test_multiple_blocks_repair(Client &client, int block_num, const ParametersInfo& paras)
{
  auto stripe_ids = client.list_stripes();
  int stripe_num = stripe_ids.size();
  std::vector<double> repair_times;
  std::vector<double> decoding_times;
  std::vector<double> cross_cluster_times;
  std::vector<double> meta_times;
  std::vector<int> cross_cluster_transfers;
  std::vector<int> io_cnts;
  int run_time = 5;
  int tot_cnt = 0;
  LocallyRepairableCode* lrc = lrc_factory(paras.ec_type, paras.cp);
  std::cout << "Multi-Block Repair:" << std::endl;
  for (int i = 0; i < stripe_num; i++) {
    std::cout << "[Stripe " << i << "]" << std::endl;
    double temp_repair = 0;
    double temp_decoding = 0;
    double temp_cross_cluster = 0;
    double temp_meta = 0;
    int temp_cc_transfers = 0;
    int temp_io_cnt = 0;
    int cnt = 0;
    for (int j = 0; j < run_time; j++) {
      int failed_num = random_range(2, 4);
      std::vector<int> failed_blocks;
      random_n_num(0, block_num - 1, failed_num, failed_blocks);
      std::vector<unsigned int> failures;
      for (auto& block : failed_blocks) {
        failures.push_back((unsigned int)block);
      }
      if (!lrc->check_if_decodable(failed_blocks)) {
        j--;
        continue;
      }
      auto resp = client.blocks_repair(failures, stripe_ids[i]);
      if (resp.success) {
        temp_repair += resp.repair_time;
        temp_decoding += resp.decoding_time;
        temp_cross_cluster += resp.cross_cluster_time;
        temp_meta += resp.meta_time;
        temp_cc_transfers += resp.cross_cluster_transfers;
        temp_io_cnt += resp.io_cnt;
        cnt++;
      }
    }
    repair_times.push_back(temp_repair);
    decoding_times.push_back(temp_decoding);
    cross_cluster_times.push_back(temp_cross_cluster);
    meta_times.push_back(temp_meta);
    cross_cluster_transfers.push_back(temp_cc_transfers);
    io_cnts.push_back(temp_io_cnt);
    std::cout << "repair = " << temp_repair / cnt
              << "s, decoding = " << temp_decoding / cnt
              << "s, cross-cluster = " << temp_cross_cluster / cnt
              << "s, meta = " << temp_meta / cnt
              << "s, cross-cluster-count = " << (double)temp_cc_transfers / cnt
              << ", I/Os = " << temp_io_cnt / cnt
              << std::endl;
    tot_cnt += cnt;
  }
  auto avg_repair = std::accumulate(repair_times.begin(),
      repair_times.end(), 0.0) / tot_cnt;
  auto avg_decoding = std::accumulate(decoding_times.begin(),
      decoding_times.end(), 0.0) / tot_cnt;
  auto avg_cross_cluster = std::accumulate(cross_cluster_times.begin(),
      cross_cluster_times.end(), 0.0) / tot_cnt;
  auto avg_meta = std::accumulate(meta_times.begin(),
      meta_times.end(), 0.0) / tot_cnt;
  auto avg_cc_transfers = (double)std::accumulate(cross_cluster_transfers.begin(),
      cross_cluster_transfers.end(), 0) / tot_cnt;
  auto avg_io_cnt = (double)std::accumulate(io_cnts.begin(),
      io_cnts.end(), 0) / tot_cnt;
  std::cout << "^-^[Average]^-^" << std::endl;
  std::cout << "repair = " << avg_repair << "s, decoding = " << avg_decoding
            << "s, cross-cluster = " << avg_cross_cluster
            << "s, meta = " << avg_meta
            << "s, cross-cluster-count = " << avg_cc_transfers
            << ", I/Os = " << avg_io_cnt
            << std::endl;
  if (lrc != nullptr) {
    delete lrc;
    lrc = nullptr;
  }
}

void test_multiple_blocks_repair_lrc(Client &client, const ParametersInfo& paras,
    int failed_num)
{
  auto stripe_ids = client.list_stripes();
  int stripe_num = stripe_ids.size();
  std::vector<double> repair_times;
  std::vector<double> decoding_times;
  std::vector<double> cross_cluster_times;
  std::vector<double> meta_times;
  std::vector<int> cross_cluster_transfers;
  std::vector<int> io_cnts;
  int run_time = 10;
  int tot_cnt = 0;
  LocallyRepairableCode* lrc = lrc_factory(paras.ec_type, paras.cp);
  std::vector<std::vector<int>> groups;
  lrc->grouping_information(groups);
  int group_num = (int)groups.size();
  std::cout << "Multi-Block Repair:" << std::endl;
  for (int i = 0; i < stripe_num; i++) {
    std::cout << "[Stripe " << i << "]" << std::endl;
    double temp_repair = 0;
    double temp_decoding = 0;
    double temp_cross_cluster = 0;
    double temp_meta = 0;
    int temp_cc_transfers = 0;
    int temp_io_cnt = 0;
    int cnt = 0;
    for (int j = 0; j < run_time; j++) {
      // int gid = random_index((size_t)group_num);
      int ran_data_idx = random_index((size_t)(lrc->k + lrc->g));
      int gid = ran_data_idx / lrc->r;
      std::vector<int> failed_blocks;
      random_n_element(2, groups[gid], failed_blocks);
      if (failed_num > 2) {
        int t_gid = random_index((size_t)group_num);
        int t_idx = random_index(groups[t_gid].size());
        int failed_idx = groups[t_gid][t_idx];
        while (std::find(failed_blocks.begin(), failed_blocks.end(), failed_idx)
            != failed_blocks.end()) {
          t_gid = random_index((size_t)group_num);
          t_idx = random_index(groups[t_gid].size());
          failed_idx = groups[t_gid][t_idx];
        }
        failed_blocks.push_back(failed_idx);
        if (failed_num > 3) {
          int tt_gid = 0;
          if (gid == t_gid && paras.cp.g < 3) {
            tt_gid = (gid + random_index((size_t)(group_num - 1)) + 1) % group_num;
          } else {
            tt_gid = random_index((size_t)group_num);
          }
          t_idx = random_index(groups[tt_gid].size());
          failed_idx = groups[tt_gid][t_idx];
          while (std::find(failed_blocks.begin(), failed_blocks.end(), failed_idx)
            != failed_blocks.end()) {
            if (gid == t_gid && paras.cp.g < 3) {
              tt_gid = (gid + random_index((size_t)(group_num - 1)) + 1) % group_num;
            } else {
              tt_gid = random_index((size_t)group_num);
            }
            t_idx = random_index(groups[tt_gid].size());
            failed_idx = groups[tt_gid][t_idx];
          }
          failed_blocks.push_back(failed_idx);
        }
      }
      if (!lrc->check_if_decodable(failed_blocks)) {
        j--;
        continue;
      }
      std::vector<unsigned int> failures;
      for (auto& block : failed_blocks) {
        failures.push_back((unsigned int)block);
      }
      auto resp = client.blocks_repair(failures, stripe_ids[i]);
      if (resp.success) {
        temp_repair += resp.repair_time;
        temp_decoding += resp.decoding_time;
        temp_cross_cluster += resp.cross_cluster_time;
        temp_meta += resp.meta_time;
        temp_cc_transfers += resp.cross_cluster_transfers;
        temp_io_cnt += resp.io_cnt;
        cnt++;
      }
    }
    repair_times.push_back(temp_repair);
    decoding_times.push_back(temp_decoding);
    cross_cluster_times.push_back(temp_cross_cluster);
    meta_times.push_back(temp_meta);
    cross_cluster_transfers.push_back(temp_cc_transfers);
    io_cnts.push_back(temp_io_cnt);
    std::cout << "repair = " << temp_repair / cnt
              << "s, decoding = " << temp_decoding / cnt
              << "s, cross-cluster = " << temp_cross_cluster / cnt
              << "s, meta = " << temp_meta / cnt
              << "s, cross-cluster-count = " << (double)temp_cc_transfers / cnt
              << ", I/Os = " << temp_io_cnt / cnt
              << std::endl;
    tot_cnt += cnt;
  }
  auto avg_repair = std::accumulate(repair_times.begin(),
      repair_times.end(), 0.0) / tot_cnt;
  auto avg_decoding = std::accumulate(decoding_times.begin(),
      decoding_times.end(), 0.0) / tot_cnt;
  auto avg_cross_cluster = std::accumulate(cross_cluster_times.begin(),
      cross_cluster_times.end(), 0.0) / tot_cnt;
  auto avg_meta = std::accumulate(meta_times.begin(),
      meta_times.end(), 0.0) / tot_cnt;
  auto avg_cc_transfers = (double)std::accumulate(cross_cluster_transfers.begin(),
      cross_cluster_transfers.end(), 0) / tot_cnt;
  auto avg_io_cnt = (double)std::accumulate(io_cnts.begin(),
      io_cnts.end(), 0) / tot_cnt;
  std::cout << "^-^[Average]^-^" << std::endl;
  std::cout << "repair = " << avg_repair << "s, decoding = " << avg_decoding
            << "s, cross-cluster = " << avg_cross_cluster
            << "s, meta = " << avg_meta
            << "s, cross-cluster-count = " << avg_cc_transfers
            << ", I/Os = " << avg_io_cnt
            << std::endl;
  if (lrc != nullptr) {
    delete lrc;
    lrc = nullptr;
  }
}

void test_stripe_merging(Client &client, int step_size)
{
  my_assert(step_size > 1);
  auto stripe_ids = client.list_stripes();
  int stripe_num = stripe_ids.size();
  std::cout << "Stripe Merging:" << std::endl;
  auto resp = client.merge(step_size);
  std::cout << "[Total]" << std::endl;
  std::cout << "merging = " << resp.merging_time
            << "s, computing = " << resp.computing_time
            << "s, cross-cluster = " << resp.cross_cluster_time
            << "s, meta =" << resp.meta_time
            << "s, cross-cluster-count = " << resp.cross_cluster_transfers
            << ", I/Os = " << resp.io_cnt
            << std::endl;
  std::cout << "[Average for every " << step_size << " stripes]" << std::endl;
  std::cout << "merging = " << resp.merging_time / stripe_num
            << "s, computing = " << resp.computing_time / stripe_num
            << "s, cross-cluster = " << resp.cross_cluster_time / stripe_num
            << "s, meta =" << resp.meta_time / stripe_num
            << "s, cross-cluster-count = "
            << (double)resp.cross_cluster_transfers / stripe_num
            << ", I/Os = " << resp.io_cnt / stripe_num
            << std::endl;
}

void generate_random_multi_block_failures_lrc(std::string filename,
    int stripe_num, const ParametersInfo& paras, int failed_num)
{
  LocallyRepairableCode* lrc = lrc_factory(paras.ec_type, paras.cp);
  std::vector<std::vector<int>> groups;
  lrc->grouping_information(groups);
  int group_num = (int)groups.size();
  std::string suf = lrc->type() + "_" + std::to_string(paras.cp.k) + "_" + \
      std::to_string(paras.cp.l) + "_" + std::to_string(paras.cp.g) + "_" + \
      std::to_string(failed_num);
  filename += suf;
  std::ofstream outFile(filename);
  if (!outFile) {
    std::cerr << "Error! Unable to open " << filename << std::endl;
    return;
  }
  int cases_per_stripe = 10;
  for (int i = 0; i < cases_per_stripe * stripe_num; i++) {
    int ran_data_idx = random_index((size_t)(lrc->k + lrc->g));
    int gid = ran_data_idx / lrc->r;
    std::vector<int> failed_blocks;
    random_n_element(2, groups[gid], failed_blocks);
    if (failed_num > 2) {
      int t_gid = random_index((size_t)group_num);
      int t_idx = random_index(groups[t_gid].size());
      int failed_idx = groups[t_gid][t_idx];
      while (std::find(failed_blocks.begin(), failed_blocks.end(), failed_idx)
          != failed_blocks.end()) {
        t_gid = random_index((size_t)group_num);
        t_idx = random_index(groups[t_gid].size());
        failed_idx = groups[t_gid][t_idx];
      }
      failed_blocks.push_back(failed_idx);
      if (failed_num > 3) {
        int tt_gid = 0;
        if (gid == t_gid && paras.cp.g < 3) {
          tt_gid = (gid + random_index((size_t)(group_num - 1)) + 1) % group_num;
        } else {
          tt_gid = random_index((size_t)group_num);
        }
        t_idx = random_index(groups[tt_gid].size());
        failed_idx = groups[tt_gid][t_idx];
        while (std::find(failed_blocks.begin(), failed_blocks.end(), failed_idx)
          != failed_blocks.end()) {
          if (gid == t_gid && paras.cp.g < 3) {
            tt_gid = (gid + random_index((size_t)(group_num - 1)) + 1) % group_num;
          } else {
            tt_gid = random_index((size_t)group_num);
          }
          t_idx = random_index(groups[tt_gid].size());
          failed_idx = groups[tt_gid][t_idx];
        }
        failed_blocks.push_back(failed_idx);
      }
    }
    if (!lrc->check_if_decodable(failed_blocks)) {
      i--;
      continue;
    } else {
      for (const auto& num : failed_blocks) {
        outFile << num << " ";
      }
      outFile << "\n";
    }
  }
  outFile.close();
}

void test_multiple_blocks_repair_lrc_with_testcases(std::string filename,
    Client &client, const ParametersInfo& paras, int failed_num)
{
  auto stripe_ids = client.list_stripes();
  int stripe_num = stripe_ids.size();
  std::vector<double> repair_times;
  std::vector<double> decoding_times;
  std::vector<double> cross_cluster_times;
  std::vector<double> meta_times;
  std::vector<int> cross_cluster_transfers;
  std::vector<int> io_cnts;
  int run_time = 10;
  int tot_cnt = 0;
  LocallyRepairableCode* lrc = lrc_factory(paras.ec_type, paras.cp);
  std::vector<std::vector<int>> groups;
  lrc->grouping_information(groups);
  int group_num = (int)groups.size();
  std::string suf = lrc->type() + "_" + std::to_string(paras.cp.k) + "_" + \
      std::to_string(paras.cp.l) + "_" + std::to_string(paras.cp.g) + "_" + \
      std::to_string(failed_num);
  filename += suf;
  std::ifstream inFile(filename);
  if (!inFile) {
    std::cerr << "Error! Unable to open " << filename << std::endl;
    return;
  }
  std::string line;
  std::cout << "Multi-Block Repair:" << std::endl;
  int ii = 0;
  int test_stripe_num = 5;
  for (int i = 0; i < test_stripe_num; i++) {
    std::cout << "[Stripe " << i << "]" << std::endl;
    double temp_repair = 0;
    double temp_decoding = 0;
    double temp_cross_cluster = 0;
    double temp_meta = 0;
    int temp_cc_transfers = 0;
    int temp_io_cnt = 0;
    int cnt = 0;
    for (int j = 0; j < run_time; j++) {
      std::vector<int> failed_blocks;
      std::getline(inFile, line);
      std::istringstream lineStream(line);
      int num;
      while (lineStream >> num) {
        failed_blocks.push_back(num);
      }
      if (i < test_stripe_num - stripe_num) {
        continue;
      }
      std::vector<unsigned int> failures;
      for (auto& block : failed_blocks) {
        failures.push_back((unsigned int)block);
      }
      auto resp = client.blocks_repair(failures, stripe_ids[ii]);
      if (resp.success) {
        temp_repair += resp.repair_time;
        temp_decoding += resp.decoding_time;
        temp_cross_cluster += resp.cross_cluster_time;
        temp_meta += resp.meta_time;
        temp_cc_transfers += resp.cross_cluster_transfers;
        temp_io_cnt += resp.io_cnt;
        cnt++;
      }
    }
    if (i < test_stripe_num - stripe_num) {
      continue;
    }
    ii++;
    repair_times.push_back(temp_repair);
    decoding_times.push_back(temp_decoding);
    cross_cluster_times.push_back(temp_cross_cluster);
    meta_times.push_back(temp_meta);
    cross_cluster_transfers.push_back(temp_cc_transfers);
    io_cnts.push_back(temp_io_cnt);
    std::cout << "repair = " << temp_repair / cnt
              << "s, decoding = " << temp_decoding / cnt
              << "s, cross-cluster = " << temp_cross_cluster / cnt
              << "s, meta = " << temp_meta / cnt
              << "s, cross-cluster-count = " << (double)temp_cc_transfers / cnt
              << ", I/Os = " << temp_io_cnt / cnt
              << std::endl;
    tot_cnt += cnt;
  }
  inFile.close();
  auto avg_repair = std::accumulate(repair_times.begin(),
      repair_times.end(), 0.0) / tot_cnt;
  auto avg_decoding = std::accumulate(decoding_times.begin(),
      decoding_times.end(), 0.0) / tot_cnt;
  auto avg_cross_cluster = std::accumulate(cross_cluster_times.begin(),
      cross_cluster_times.end(), 0.0) / tot_cnt;
  auto avg_meta = std::accumulate(meta_times.begin(),
      meta_times.end(), 0.0) / tot_cnt;
  auto avg_cc_transfers = (double)std::accumulate(cross_cluster_transfers.begin(),
      cross_cluster_transfers.end(), 0) / tot_cnt;
  auto avg_io_cnt = (double)std::accumulate(io_cnts.begin(),
      io_cnts.end(), 0) / tot_cnt;
  std::cout << "^-^[Average]^-^" << std::endl;
  std::cout << "repair = " << avg_repair << "s, decoding = " << avg_decoding
            << "s, cross-cluster = " << avg_cross_cluster
            << "s, meta = " << avg_meta
            << "s, cross-cluster-count = " << avg_cc_transfers
            << ", I/Os = " << avg_io_cnt
            << std::endl;
  if (lrc != nullptr) {
    delete lrc;
    lrc = nullptr;
  }
}

void test_repair_performance(std::string path_prefix, int stripe_num,
    const ParametersInfo& paras, int failed_num)
{
  int block_num = paras.cp.k + paras.cp.m;

  Client client("0.0.0.0", CLIENT_PORT, "0.0.0.0", COORDINATOR_PORT);

  // set erasure coding parameters
  client.set_ec_parameters(paras);

  struct timeval start_time, end_time;
  // generate key-value pair
  int value_length = (int)paras.block_size * paras.cp.k;
  #ifdef IN_MEMORY
    std::unordered_map<std::string, std::string> key_value;
    generate_unique_random_strings(5, value_length, stripe_num, key_value);
  #endif

  // set
  double set_time = 0;
  #ifdef IN_MEMORY
    for (auto& kv : key_value) {
      gettimeofday(&start_time, NULL);
      double encoding_time = client.set(kv.first, kv.second);
      gettimeofday(&end_time, NULL);
      double temp_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
      set_time += temp_time;
      std::cout << "[SET] set time: " << temp_time << ", encoding time: "
                << encoding_time << std::endl;
    }
    std::cout << "Total set time: " << set_time << ", average set time:"
              << set_time / stripe_num << std::endl;
  #else
    for (int i = 0; i < stripe_num; i++) {
      std::string key;
      if (i < 10) {
        key = "Obj0" + std::to_string(i);
      } else {
        key = "Obj" + std::to_string(i);
      }
      std::string readpath = path_prefix + "/../../data/Object";
      double encoding_time = 0;
      gettimeofday(&start_time, NULL);
      if (access(readpath.c_str(), 0) == -1) {
        std::cout << "[Client] file does not exist!" << std::endl;
        exit(-1);
      } else {
        char *buf = new char[value_length];
        std::ifstream ifs(readpath);
        ifs.read(buf, value_length);
        // std::cout << "[Client] begin read" << std::endl;
        encoding_time = client.set(key, std::string(buf, value_length));
        // std::cout << "[Client] end read!" << std::endl;
        ifs.close();
        delete buf;
      }
      gettimeofday(&end_time, NULL);
      double temp_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
      set_time += temp_time;
      std::cout << "[SET] set time: " << temp_time << ", encoding time: "
                << encoding_time << std::endl;
    }
    std::cout << "Total set time: " << set_time << ", average set time:"
              << set_time / stripe_num << std::endl;
  #endif

  if (failed_num == 1) {
    // single-block repair
    test_single_block_repair(client, block_num);
  } else if (failed_num == 2) {
    // single node repair
    test_node_repair(client, 0, paras); // 0: failure node id
  } else if (failed_num > 2) {
    // multiple-block repair
    test_multiple_blocks_repair(client, block_num, paras);
    // test_multiple_blocks_repair_lrc(client, paras, failed_num);
    // test_multiple_blocks_repair_lrc_with_testcases(path_prefix + "/../../testcase/",
        // client, paras, failed_num);
  }

  #ifdef IN_MEMORY
    // get
    double get_time = 0;
    gettimeofday(&start_time, NULL);
    for (auto &kv : key_value) {
      auto stored_value = client.get(kv.first);
      my_assert(stored_value == kv.second);
    }
    gettimeofday(&end_time, NULL);
    get_time += end_time.tv_sec - start_time.tv_sec +
        (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
    std::cout << "Total get time: " << get_time << ", average get time:"
              << get_time / stripe_num << std::endl;
  #endif 
  // delete
  client.delete_all_stripes();
}

int main(int argc, char **argv)
{
  if (argc != 4) {
    std::cout << "./run_client stripe_num config_file failed_num" << std::endl;
    exit(0);
  }

  char buff[256];
  getcwd(buff, 256);
  std::string cwf = std::string(argv[0]);
  std::string path_prefix = std::string(buff) + cwf.substr(1, cwf.rfind('/') - 1);

  ParametersInfo paras;
  parse_args(paras, path_prefix + "/../" + std::string(argv[2]));
  int stripe_num = std::stoi(argv[1]);
  int failed_num = std::stoi(argv[3]);
  my_assert(0 <= failed_num && failed_num <= 4);
  
  /* generate test cases first */
  std::string file_path  = path_prefix + "/../../testcase/";
  if (access(file_path.c_str(), 0) == -1) {
    mkdir(file_path.c_str(), S_IRWXU);
  }
  // generate_random_multi_block_failures_lrc(file_path, stripe_num, paras, failed_num);
  
  test_repair_performance(path_prefix, stripe_num, paras, failed_num);

  return 0;
}