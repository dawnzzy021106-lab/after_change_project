#include "coordinator.h"

namespace ECProject
{
  // init_ec_schema：初始化纠删码参数配置
  void Coordinator::init_ec_schema(std::string config_file)
  {
    ParametersInfo paras;
    // parse_args：从配置文件解析参数
    parse_args(paras, config_file);
    // set_erasure_coding_parameters：设置纠删码相关参数
    set_erasure_coding_parameters(paras);
  }
  
  // init_cluster_info：从XML配置文件初始化集群拓扑信息
  void Coordinator::init_cluster_info()
  {
    tinyxml2::XMLDocument xml;
    xml.LoadFile(xml_path_.c_str());
    tinyxml2::XMLElement *root = xml.RootElement();
    unsigned int node_id = 0;
    num_of_clusters_ = 0;

    // 解析XML文件获取集群和节点信息，建立集群表（cluster_table_）和节点表（node_table_），建立节点到集群的映射关系
    // cluster_table_[cluster_id] = {cluster_id, proxy_ip, proxy_port, nodes[]}
    // node_table_[node_id] = {node_id, node_ip, node_port, map2cluster}
    for (tinyxml2::XMLElement *cluster = root->FirstChildElement(); 
          cluster != nullptr; cluster = cluster->NextSiblingElement()) 
    {
      unsigned int cluster_id(std::stoi(cluster->Attribute("id")));
      std::string proxy(cluster->Attribute("proxy"));

      cluster_table_[cluster_id].cluster_id = cluster_id;
      auto pos = proxy.find(':');
      cluster_table_[cluster_id].proxy_ip = proxy.substr(0, pos);
      cluster_table_[cluster_id].proxy_port = std::stoi(proxy.substr(pos + 1, proxy.size()));

      num_of_nodes_per_cluster_ = 0;
      for (tinyxml2::XMLElement *node = cluster->FirstChildElement()->FirstChildElement();
            node != nullptr; node = node->NextSiblingElement()) 
      {
        cluster_table_[cluster_id].nodes.push_back(node_id);

        std::string node_uri(node->Attribute("uri"));
        node_table_[node_id].node_id = node_id;
        auto pos = node_uri.find(':');
        node_table_[node_id].node_ip = node_uri.substr(0, pos);
        node_table_[node_id].node_port = std::stoi(node_uri.substr(pos + 1, node_uri.size()));
        node_table_[node_id].map2cluster = cluster_id;
        node_id++;
        num_of_nodes_per_cluster_++;
      }
      num_of_clusters_++;
    }
  }

  // init_proxy_info：初始化与所有proxy的连接
  void Coordinator::init_proxy_info()
  {
    // 遍历所有集群，为每个proxy创建RPC客户端连接，发送心跳检查proxy是否存活
    for (auto cur = cluster_table_.begin(); cur != cluster_table_.end(); cur++) {
      std::string proxy_ip = cur->second.proxy_ip;
      int proxy_port = cur->second.proxy_port;
      std::string location = proxy_endpoint_key(proxy_ip, proxy_port);
      my_assert(main_proxies_.contains(location) == false);

      main_proxies_[location] = std::make_unique<coro_rpc::coro_rpc_client>();
      help_proxies_[location] = std::make_unique<coro_rpc::coro_rpc_client>();
      help_prepare_proxies_[location] = std::make_unique<coro_rpc::coro_rpc_client>();
      help_send_proxies_[location] = std::make_unique<coro_rpc::coro_rpc_client>();
      main_proxy_mutexes_[location] = std::make_unique<std::mutex>();
      help_proxy_mutexes_[location] = std::make_unique<std::mutex>();
      help_prepare_proxy_mutexes_[location] = std::make_unique<std::mutex>();
      help_send_proxy_mutexes_[location] = std::make_unique<std::mutex>();
      if (!IF_SIMULATION) {
        async_simple::coro::syncAwait(
            main_proxies_[location]->connect(proxy_ip, std::to_string(proxy_port)));
        async_simple::coro::syncAwait(
            help_proxies_[location]->connect(proxy_ip, std::to_string(proxy_port)));
        async_simple::coro::syncAwait(
            help_prepare_proxies_[location]->connect(proxy_ip, std::to_string(proxy_port)));
        async_simple::coro::syncAwait(
            help_send_proxies_[location]->connect(proxy_ip, std::to_string(proxy_port)));
        auto msg = async_simple::coro::syncAwait(
            main_proxies_[location]->call<&Proxy::checkalive>("hello"));
        if (msg != "hello") {
          std::cout << "[Proxy Check] failed to connect " << location << std::endl;
        }
      }
    }
  }

  // reset_metadata：重置所有元数据状态
  void Coordinator::reset_metadata()
  {
    cur_stripe_id_ = 0;
    cur_block_id_ = 0;
    time_ = 0;
    merged_flag_ = false;
    commited_object_table_.clear();
    updating_object_table_.clear();
    merge_groups_.clear();
    free_clusters_.clear();
    for (auto& kv : stripe_table_) {
      if (kv.second.ec != nullptr) {
        delete kv.second.ec;
        kv.second.ec = nullptr;
      }
    }
    stripe_table_.clear();
    for (auto &kv : cluster_table_) {
      kv.second.holding_stripe_ids.clear();
    }
  }

  // new_stripe：创建新的数据条带
  Stripe& Coordinator::new_stripe(size_t block_size, ErasureCode *ec)
  {
    my_assert(ec != nullptr);
    Stripe temp;
    // 分配新的条带ID
    temp.stripe_id = cur_stripe_id_++;
    temp.block_size = block_size;
    // 在stripe_table_中创建条目
    // stripe_table_[stripe_id] = {stripe_id, block_size, ec, blocks2nodes}
    stripe_table_[temp.stripe_id] = temp;
    stripe_table_[temp.stripe_id].ec = clone_ec(ec_schema_.ec_type, ec);
    return stripe_table_[temp.stripe_id];
  }

  // new_ec_for_merge：为数据合并操作创建新的纠删码实例
  ErasureCode* Coordinator::new_ec_for_merge(int step_size)
  {
    CodingParameters cp;
    ec_schema_.ec->get_coding_parameters(cp);
    ECFAMILY ec_family = check_ec_family(ec_schema_.ec_type);
    if (ec_family == RSCodes) { // RS
      cp.k *= step_size;
    } else if (ec_family == LRCs) { // LRC
      cp.k *= step_size;
      cp.l *= step_size;
    } else { // Product Codes
      if (ec_schema_.multistripe_placement_rule == VERTICAL) {
        cp.k2 *= step_size;
      } else {
        cp.k1 *= step_size;
      }
    }
    return ec_factory(ec_schema_.ec_type, cp);
  }

  // init_placement_info：初始化placement
  void Coordinator::init_placement_info(PlacementInfo &placement, std::string key,
                                        size_t value_len, size_t block_size)
  {
    placement.ec_type = ec_schema_.ec_type;
    placement.key = key;
    placement.value_len = value_len;
    ec_schema_.ec->get_coding_parameters(placement.cp);
    placement.block_size = block_size;
    placement.cp.x = ec_schema_.x;
    placement.merged_flag = merged_flag_;
    if (ec_schema_.multistripe_placement_rule == VERTICAL) {
      placement.isvertical = true;
    }
  }

  // find_out_stripe_partitions：条带在集群间的分布情况
  void Coordinator::find_out_stripe_partitions(unsigned int stripe_id)
  {
    Stripe& stripe = stripe_table_[stripe_id];
    stripe.ec->partition_plan.clear();
    std::unordered_map<unsigned int, std::vector<int>> blocks_in_clusters;
    // 遍历条带的k+m个块，根据块所在节点找到对应集群
    for (int i = 0; i < stripe.ec->k + stripe.ec->m; i++) {
      unsigned int node_id = stripe.blocks2nodes[i];
      unsigned int cluster_id = node_table_[node_id].map2cluster;
      if (blocks_in_clusters.find(cluster_id) == blocks_in_clusters.end()) {
        blocks_in_clusters[cluster_id] = std::vector<int>({i});
      } else {
        blocks_in_clusters[cluster_id].push_back(i);
      }
    }
    // 统计每个集群包含哪些块
    for (auto& kv : blocks_in_clusters) {
      stripe.ec->partition_plan.push_back(kv.second);
    }
    if (IF_DEBUG) {
      stripe.ec->print_info(stripe.ec->partition_plan, "partition");
    }
  }

  // 检查LRC码的容错约束是否满足，确保集群故障不会导致数据不可恢复
  bool Coordinator::if_subject_to_fault_tolerance_lrc(
          ErasureCode *ec, std::vector<int> blocks_in_cluster,
          std::unordered_map<int, std::vector<int>>& group_blocks)
  {
    auto lrc = dynamic_cast<LocallyRepairableCode*>(ec);
    int blocks_num = (int)blocks_in_cluster.size();
    for (int i = 0; i < blocks_num; i++) {
      int gid = lrc->bid2gid(blocks_in_cluster[i]);
      if (group_blocks.find(gid) == group_blocks.end()) {
        group_blocks[gid] = std::vector({blocks_in_cluster[i]});
      } else {
        group_blocks[gid].push_back(blocks_in_cluster[i]);
      }
    }
    int group_num = (int)group_blocks.size();
    if (blocks_num > group_num + lrc->g) {
      return false;
    }
    return true;
  }

  // 确保集群故障不会超过Product Code的容错能力
  bool Coordinator::if_subject_to_fault_tolerance_pc(
            ErasureCode *ec, std::vector<int> blocks_in_cluster,
            std::unordered_map<int, std::vector<int>> &col_blocks)
  {
    auto pc = dynamic_cast<ProductCode*>(ec);
    int blocks_num = (int)blocks_in_cluster.size();
    for (int i = 0; i < blocks_num; i++) {
      int row = -1, col = -1;
      pc->bid2rowcol(blocks_in_cluster[i], row, col);
      if (col_blocks.find(col) == col_blocks.end()) {
        col_blocks[col] = std::vector({blocks_in_cluster[i]});
      } else {
        col_blocks[col].push_back(blocks_in_cluster[i]);
      }
    }
    int col_num = (int)col_blocks.size();
    if (col_num > pc->m1) {
      return false;
    }
    return true;
  }
}