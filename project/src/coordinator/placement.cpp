#include "coordinator.h"

namespace ECProject
{
  // 为指定条带生成 placement
  void Coordinator::generate_placement(unsigned int stripe_id)
  {
    Stripe &stripe = stripe_table_[stripe_id];
    int n = stripe.ec->k + stripe.ec->m;
    // 初始化条带块ID
    for (int i = 0; i < n; i++) {
      stripe.block_ids.push_back(cur_block_id_++);
    }
    // 根据EC类型生成分区方案
    stripe.ec->placement_rule = ec_schema_.placement_rule;
    stripe.ec->generate_partition();
    if (IF_DEBUG) {
      stripe.ec->print_info(stripe.ec->partition_plan, "partition");
    }

    // 处理合并组
    // merge_groups_：记录哪些条带可以合并
    // x：合并参数，表示每个合并组的条带数
    int idx = merge_groups_.size() - 1;
    bool new_group = false;
    if (idx < 0 || merge_groups_[idx].size() == ec_schema_.x) {
      new_group = true;
    }

    // 5种放置规则：
    // DISPERSED：分散放置，每个分区的块分散在不同集群，最大化数据可靠性，避免单集群故障
    // AGGREGATED, VERTICAL：聚合放置、垂直放置，条带的所有分区按顺序分布在集群中
    // HORIZONTAL：水平放置，一个分区在幸运集群，其余分区在其他集群，平衡数据局部性和可靠性
    // RAND：随机放置，
    if (ec_schema_.placement_rule == OPTIMAL &&
        ec_schema_.multistripe_placement_rule != RAND) {
      if (ec_schema_.multistripe_placement_rule == DISPERSED) {
        if (new_group) {
          free_clusters_.clear();
          for (unsigned int i = 0; i < num_of_clusters_; i++) {
            free_clusters_.push_back(i);
          }
        }
        int required_cluster_num = (int)stripe.ec->partition_plan.size();
        my_assert((int)free_clusters_.size() >= required_cluster_num);
        select_nodes_by_random(free_clusters_, stripe_id, required_cluster_num);
      } else if (ec_schema_.multistripe_placement_rule == AGGREGATED ||
                 ec_schema_.multistripe_placement_rule == VERTICAL) {
        if (new_group) {
          lucky_cid_ = random_index(num_of_clusters_);
        }
        select_nodes_in_order(stripe_id);
      } else if (ec_schema_.multistripe_placement_rule == HORIZONTAL) {
        if (new_group) {
          lucky_cid_ = random_index(num_of_clusters_); // 幸运集群的分区ID
          free_clusters_.clear();
          for (unsigned int i = 0; i < num_of_clusters_; i++) {
            if (i != lucky_cid_) { // 除了幸运集群外的集群
              free_clusters_.push_back(i);
            }
          }
        }
        int required_cluster_num = (int)stripe.ec->partition_plan.size();
        my_assert((int)free_clusters_.size() >= required_cluster_num - 1);
        select_nodes_by_random(free_clusters_, stripe_id, required_cluster_num - 1);
      }
    } else {
      std::vector<unsigned int> free_clusters;
      for (unsigned int i = 0; i < num_of_clusters_; i++) {
        free_clusters.push_back(i);
      }
      int required_cluster_num = (int)stripe.ec->partition_plan.size();
      select_nodes_by_random(free_clusters, stripe_id, required_cluster_num);
    }
    if (new_group) {
      std::vector<unsigned int> temp;
      temp.push_back(stripe_id);
      merge_groups_.push_back(temp); // 创建新合并组
    } else {
      merge_groups_[idx].push_back(stripe_id); // 加入现有合并组
    }

    if (IF_DEBUG) {
      // print_placement_result("Generate placement:");
    }
  }

  // 使用随机算法为条带选择节点
  // free_clusters：可用集群列表
  // split_idx：指定前几个分区需要从free_clusters中选择
  void Coordinator::select_nodes_by_random(
            std::vector<unsigned int>& free_clusters,
            unsigned int stripe_id, int split_idx)
  {
    Stripe &stripe = stripe_table_[stripe_id];
    int n = stripe.ec->k + stripe.ec->m;
    for (unsigned int i = 0; i < n; i++) {
      stripe.blocks2nodes.push_back(i);
    }

    // place each partition into a seperate cluster
    size_t free_clusters_num = free_clusters.size();
    int num_of_partitions = int(stripe.ec->partition_plan.size());
    for (int i = 0; i < split_idx; i++) {
      my_assert(free_clusters_num);
      // 随机选择一个集群
      int cluster_idx = random_index(free_clusters_num);
      unsigned int cluster_id = free_clusters[cluster_idx];
      Cluster &cluster = cluster_table_[cluster_id];
      // 从该集群中随机选择节点
      std::vector<unsigned int> free_nodes;
      for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
        free_nodes.push_back(cluster.nodes[j]);
      }
      int partition_size = int(stripe.ec->partition_plan[i].size());
      size_t free_nodes_num = int(free_nodes.size());
      // 为分区中的每个块随机选择节点
      for (int j = 0; j < partition_size; j++) {
        my_assert(free_nodes_num);
        // randomly select a node
        int node_idx = random_index(free_nodes_num);
        unsigned int node_id = free_nodes[node_idx];
        int block_idx = stripe.ec->partition_plan[i][j];
        stripe.blocks2nodes[block_idx] = node_id;
        // 从可用节点列表中移除已选节点
        auto it_n = std::find(free_nodes.begin(), free_nodes.end(), node_id);
        free_nodes.erase(it_n);
        free_nodes_num--;
      }
      // 从可用集群列表中移除已选集群
      auto it_r = std::find(free_clusters.begin(), free_clusters.end(), cluster_id);
      free_clusters.erase(it_r);
      free_clusters_num--;
    }

    // 剩余分区分配到幸运集群
    for (int i = split_idx; i < num_of_partitions; i++) {
      unsigned int cluster_id = lucky_cid_; // 使用预先确定的幸运集群
      Cluster& cluster = cluster_table_[cluster_id];
      std::vector<unsigned int> free_nodes;
      for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
        free_nodes.push_back(cluster.nodes[j]);
      }
      int partition_size = int(stripe.ec->partition_plan[i].size());
      size_t free_nodes_num = free_nodes.size();
      for (int j = 0; j < partition_size; j++) {
        my_assert(free_nodes_num);
        // randomly select a node
        int node_idx = random_index(free_nodes_num);
        unsigned int node_id = free_nodes[node_idx];
        int block_idx = stripe.ec->partition_plan[i][j];
        stripe.blocks2nodes[block_idx] = node_id;
        // remove the chosen node from the free list
        auto it_n = std::find(free_nodes.begin(), free_nodes.end(), node_id);
        free_nodes.erase(it_n);
        free_nodes_num--;
      }
    }
  }

  // 按照集群顺序为条带选择节点
  void Coordinator::select_nodes_in_order(unsigned int stripe_id)
  {
    Stripe &stripe = stripe_table_[stripe_id];
    int n = stripe.ec->k + stripe.ec->m;
    for (unsigned int i = 0; i < n; i++) {
      stripe.blocks2nodes.push_back(i);
    }

    // place each partition into a seperate cluster
    int num_of_partitions = int(stripe.ec->partition_plan.size());
    for (int i = 0; i < num_of_partitions; i++) {
      unsigned int cluster_id = (lucky_cid_ + i) % num_of_clusters_;
      Cluster &cluster = cluster_table_[cluster_id];
      std::vector<unsigned int> free_nodes;
      for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
        free_nodes.push_back(cluster.nodes[j]);
      }
      int partition_size = int(stripe.ec->partition_plan[i].size());
      size_t free_nodes_num = free_nodes.size();
      for (int j = 0; j < partition_size; j++) {
        my_assert(free_nodes_num);
        // randomly select a node
        int node_idx = random_index(free_nodes_num);
        unsigned int node_id = free_nodes[node_idx];
        int block_idx = stripe.ec->partition_plan[i][j];
        stripe.blocks2nodes[block_idx] = node_id;
        // remove the chosen node from the free list
        auto it_n = std::find(free_nodes.begin(), free_nodes.end(), node_id);
        free_nodes.erase(it_n);
        free_nodes_num--;
      }
    }
  }

  // 打印 placement 结果
  void Coordinator::print_placement_result(std::string msg)
  {
    std::cout << std::endl;
    std::cout << msg << std::endl;
    for (auto& kv : stripe_table_) {
      find_out_stripe_partitions(kv.first);
      std::cout << "Stripe " << kv.first << " block placement:\n";
      for (auto& vec : kv.second.ec->partition_plan) {
        unsigned int node_id = kv.second.blocks2nodes[vec[0]];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        std::cout << cluster_id << ": ";
        for (int ele : vec) {
          std::cout << "B" << ele << "N" << kv.second.blocks2nodes[ele] << " ";
        }
        std::cout << "\n";
      }
    }
    std::cout << "Merge Group: ";
    for (auto it1 = merge_groups_.begin(); it1 != merge_groups_.end(); it1++) {
      std::cout << "[ ";
      for (auto it2 = (*it1).begin(); it2 != (*it1).end(); it2++) {
        std::cout << (*it2) << " ";
      }
      std::cout << "] ";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
}