#include "coordinator.h"

namespace ECProject
{
  Coordinator::Coordinator(std::string ip, int port, std::string xml_path)
      : ip_(ip), port_(port), xml_path_(xml_path)
  {
    easylog::set_min_severity(easylog::Severity::ERROR); // 日志级别：ERROR
    rpc_server_ = std::make_unique<coro_rpc::coro_rpc_server>(4, port_); // 创建RPC服务器，4个工作线程
    rpc_server_->register_handler<&Coordinator::checkalive>(this);
    rpc_server_->register_handler<&Coordinator::set_erasure_coding_parameters>(this);
    rpc_server_->register_handler<&Coordinator::set_flow_repair_max_parallel>(this);
    rpc_server_->register_handler<&Coordinator::request_set>(this);
    rpc_server_->register_handler<&Coordinator::commit_object>(this);
    rpc_server_->register_handler<&Coordinator::request_get>(this);
    rpc_server_->register_handler<&Coordinator::request_delete_by_stripe>(this);
    rpc_server_->register_handler<&Coordinator::request_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_flow_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_flow_unordered_concurrency_repair>(
            this);
    rpc_server_->register_handler<&Coordinator::request_flow_ordered_concurrency_repair>(
            this);
    rpc_server_->register_handler<&Coordinator::request_join_flow_ordered_concurrency_repair>(
            this);
    rpc_server_->register_handler<&Coordinator::request_merge>(this);
    rpc_server_->register_handler<&Coordinator::list_stripes>(this);
    // 在已有的 register_handler 列表后加上：
    rpc_server_->register_handler<&Coordinator::request_snapshot_metadata>(this);
    rpc_server_->register_handler<&Coordinator::request_revert_metadata>(this);

    cur_stripe_id_ = 0;
    cur_block_id_ = 0;
    time_ = 0;
    
    init_cluster_info(); // init_cluster_info：从XML配置文件初始化集群拓扑信息
    init_proxy_info(); // init_proxy_info：初始化与所有proxy的连接
    if (IF_DEBUG) {
      std::cout << "Start the coordinator! " << ip << ":" << port << std::endl;
    }
  }
  // 析构函数：停止RPC服务器，清理资源
  Coordinator::~Coordinator() { rpc_server_->stop(); }

  // run：启动RPC服务器，开始监听客户端请求
  void Coordinator::run() { auto err = rpc_server_->start(); }

  std::string Coordinator::checkalive(std::string msg) 
  { 
    return msg + " Hello, it's me. The coordinator!"; 
  }

  std::string Coordinator::proxy_endpoint_key(const std::string& proxy_ip, int proxy_port)
  {
    return proxy_ip + ":" + std::to_string(proxy_port);
  }

  void Coordinator::set_erasure_coding_parameters(ParametersInfo paras)
  {
    ec_schema_.partial_decoding = paras.partial_decoding;
    ec_schema_.partial_scheme = paras.partial_scheme;
    ec_schema_.repair_priority = paras.repair_priority;
    ec_schema_.repair_method = paras.repair_method;
    ec_schema_.ec_type = paras.ec_type;
    ec_schema_.placement_rule = paras.placement_rule;
    ec_schema_.multistripe_placement_rule = paras.multistripe_placement_rule;
    ec_schema_.x = paras.cp.x;
    ec_schema_.block_size = paras.block_size;
    ec_schema_.repair_stripe_num = paras.repair_stripe_num;
    ec_schema_.flow_repair_parallel_unordered = paras.flow_repair_parallel_unordered;
    ec_schema_.flow_repair_parallel_ordered = paras.flow_repair_parallel_ordered;
    ec_schema_.flow_repair_parallel_join_ordered = paras.flow_repair_parallel_join_ordered;
    ec_schema_.ec = ec_factory(paras.ec_type, paras.cp); // 创建EC实例
    reset_metadata(); // 重置元数据
  }

  void Coordinator::set_flow_repair_max_parallel(int n)
  {
    // 三种模式并行度已在 set_erasure_coding_parameters 中从 config.ini 初始化，
    // do_flow_repair_common 直接读取 ec_schema_ 的模式字段，不再依赖此 RPC。
    // 保留空操作以兼容旧客户端调用。
    (void)n;
  }

  // request_set：处理客户端数据写入请求
  SetResp Coordinator::request_set(
              std::vector<std::pair<std::string, size_t>> objects)
  {
    int num_of_objects = (int)objects.size();
    my_assert(num_of_objects > 0);
    mutex_.lock();
    for (int i = 0; i < num_of_objects; i++) {
      std::string key = objects[i].first;
      if (commited_object_table_.contains(key)) { // 检查对象是否已存在
        mutex_.unlock();
        my_assert(false);
      }
    }
    mutex_.unlock();

    // 计算总数据大小
    size_t total_value_len = 0;
    std::vector<ObjectInfo> new_objects;
    for (int i = 0; i < num_of_objects; i++) {
      ObjectInfo new_object;
      new_object.value_len = objects[i].second;
      my_assert(new_object.value_len % ec_schema_.block_size == 0);
      total_value_len += new_object.value_len;
      new_objects.push_back(new_object);
    }

    my_assert(total_value_len % (ec_schema_.block_size * ec_schema_.ec->k) == 0);
    

    if (IF_DEBUG) {
      std::cout << "[SET] Ready to process " << num_of_objects
                << " objects. Each with length of "
                << total_value_len / num_of_objects << std::endl;
    }

    PlacementInfo placement;

    // init_placement_info：初始化placement
    init_placement_info(placement, objects[0].first,
                        total_value_len, ec_schema_.block_size);

    // 计算需要的条带数
    int num_of_stripes = total_value_len / (ec_schema_.ec->k * ec_schema_.block_size);
    size_t left_value_len = total_value_len;
    int cumulative_len = 0, obj_idx = 0;
    for (int i = 0; i < num_of_stripes; i++) {
      left_value_len -= ec_schema_.ec->k * ec_schema_.block_size;
      // new_stripe：创建新的数据条带
      Stripe& stripe = new_stripe(ec_schema_.block_size, ec_schema_.ec);
      while (cumulative_len < ec_schema_.ec->k * ec_schema_.block_size) {
        stripe.objects.push_back(objects[obj_idx].first);
        new_objects[obj_idx].stripes.push_back(stripe.stripe_id);
        cumulative_len += objects[obj_idx].second;
        obj_idx++;
      }

      if (IF_DEBUG) {
        std::cout << "[SET] Ready to data placement for stripe "
                  << stripe.stripe_id << std::endl;
      }

      // generate_placement：生成 placement
      generate_placement(stripe.stripe_id);

      if (IF_DEBUG) {
        std::cout << "[SET] Finish data placement for stripe "
                  << stripe.stripe_id << std::endl;
      }

      placement.stripe_ids.push_back(stripe.stripe_id);
      placement.seri_nums.push_back(stripe.stripe_id % ec_schema_.x);
      for (auto& node_id : stripe.blocks2nodes) {
        auto& node = node_table_[node_id];
        placement.datanode_ip_port.push_back(std::make_pair(node.map2cluster,
          std::make_pair(node.node_ip, node.node_port)));
      }
      for (auto& block_id : stripe.block_ids) {
        placement.block_ids.push_back(block_id);
      }
    }
    my_assert(left_value_len == 0);

    mutex_.lock();
    for (int i = 0; i < num_of_objects; i++) {
      updating_object_table_[objects[i].first] = new_objects[i];
    }
    mutex_.unlock();

    // 选择该集群的 proxy 处理编码和存储
    unsigned int node_id = stripe_table_[cur_stripe_id_ - 1].blocks2nodes[0];
    unsigned int selected_cluster_id = node_table_[node_id].map2cluster;
    std::string selected_proxy_ip = cluster_table_[selected_cluster_id].proxy_ip;
    int selected_proxy_port = cluster_table_[selected_cluster_id].proxy_port;

    if (IF_DEBUG) {
      std::cout << "[SET] Select proxy " << selected_proxy_ip << ":" 
                << selected_proxy_port << " in cluster "
                << selected_cluster_id << " to handle encode and set!" << std::endl;
    }

    if (IF_SIMULATION) {// simulation, commit object
      mutex_.lock();
      for (int i = 0; i < num_of_objects; i++) {
        my_assert(commited_object_table_.contains(objects[i].first) == false &&
                  updating_object_table_.contains(objects[i].first) == true);
        commited_object_table_[objects[i].first] = updating_object_table_[objects[i].first];
        updating_object_table_.erase(objects[i].first);
      }
      mutex_.unlock();
    } else {
      std::string proxy_key = proxy_endpoint_key(selected_proxy_ip, selected_proxy_port);

      if (main_proxies_.find(proxy_key) == main_proxies_.end()) {
        std::cerr << "ERROR: Proxy " << proxy_key << " not found in main_proxies_ map!" << std::endl;
        my_assert(false); 
      } 

      async_simple::coro::syncAwait(
        main_proxies_[proxy_key]
            ->call<&Proxy::encode_and_store_object>(placement));
    }
    // if (IF_DEBUG) {
    //   std::cout << "[SET] coordinator.cpp over! " << std::endl;
    // }
    SetResp response;
    response.proxy_ip = selected_proxy_ip;
    response.proxy_port = selected_proxy_port + SOCKET_PORT_OFFSET; // port for transfer data

    return response;
  }

  // 确认/取消 object 写入
  void Coordinator::commit_object(std::vector<std::string> keys, bool commit)
  {
    int num = (int)keys.size();
    if (commit) { // 确认提交
      // 从updating_object_table_移动到commited_object_table_
      mutex_.lock();
      for (int i = 0; i < num; i++) {
        my_assert(commited_object_table_.contains(keys[i]) == false &&
                  updating_object_table_.contains(keys[i]) == true);
        commited_object_table_[keys[i]] = updating_object_table_[keys[i]];
        updating_object_table_.erase(keys[i]);
      }
      mutex_.unlock();
    } else { // 取消提交
      // 清理相关条带和集群信息
      // 从updating_object_table_中移除对象
      for (int i = 0; i < num; i++) {
        ObjectInfo& objects = updating_object_table_[keys[i]];
        for (auto stripe_id : objects.stripes) {
          for (auto it = cluster_table_.begin(); it != cluster_table_.end(); it++) {
            Cluster& cluster = it->second;
            cluster.holding_stripe_ids.erase(stripe_id);
          }
          stripe_table_.erase(stripe_id);
          cur_stripe_id_--;
        }
      }
      mutex_.lock();
      for (int i = 0; i < num; i++) {
        my_assert(commited_object_table_.contains(keys[i]) == false &&
                  updating_object_table_.contains(keys[i]) == true);
        updating_object_table_.erase(keys[i]);
      }
      mutex_.unlock();
    }
  }

  // 处理客户端数据读取请求
  size_t Coordinator::request_get(std::string key, std::string client_ip,
                                  int client_port)
  {
    mutex_.lock();
    // 验证对象存在
    if (commited_object_table_.contains(key) == false) {
      mutex_.unlock();
      my_assert(false);
    }
    // 获取对象
    ObjectInfo &object = commited_object_table_[key];
    mutex_.unlock();

    // 收集对象相关的所有条带信息
    PlacementInfo placement;
    int stripe_num = (int)object.stripes.size();
    my_assert(stripe_num > 0);
  
    unsigned int stripe_id = object.stripes[0];
    Stripe &stripe = stripe_table_[stripe_id];
    // init_placement_info：初始化placement
    init_placement_info(placement, key, object.value_len, stripe.block_size);

    // 计算数据在条带中的offset，收集相关的所有节点的位置
    for (auto stripe_id : object.stripes) {
      Stripe &stripe = stripe_table_[stripe_id];
      placement.stripe_ids.push_back(stripe_id);

      int num_of_object_in_a_stripe = (int)stripe.objects.size();
      int offset = 0;
      for (int i = 0; i < num_of_object_in_a_stripe; i++) {
        if (stripe.objects[i] != key) {
          int t_object_len = commited_object_table_[stripe.objects[i]].value_len;
          offset += t_object_len / stripe.block_size;     // must be block_size of stripe
        } else {
          if (merged_flag_) {
            placement.cp.seri_num = i;
            placement.cp.x = num_of_object_in_a_stripe;
          }
          break;
        }
      }
      placement.offsets.push_back(offset);

      for (auto& node_id : stripe.blocks2nodes) {
        auto& node = node_table_[node_id];
        placement.datanode_ip_port.push_back(std::make_pair(node.map2cluster,
            std::make_pair(node.node_ip, node.node_port)));
      }
      for (auto& block_id : stripe.block_ids) {
        placement.block_ids.push_back(block_id);
      }
    }

    if (!IF_SIMULATION) {
      placement.client_ip = client_ip;
      placement.client_port = client_port;
      // 随机选择一个proxy读取
      int selected_proxy_id = random_index(cluster_table_.size());
      std::string location = proxy_endpoint_key(
          cluster_table_[selected_proxy_id].proxy_ip,
          cluster_table_[selected_proxy_id].proxy_port);
      if (IF_DEBUG) {
        std::cout << "[GET] Select proxy "
                  << cluster_table_[selected_proxy_id].proxy_ip << ":" 
                  << cluster_table_[selected_proxy_id].proxy_port
                  << " in cluster "
                  << cluster_table_[selected_proxy_id].cluster_id
                  << " to handle get!" << std::endl;
      }
      async_simple::coro::syncAwait(
          main_proxies_[location]->call<&Proxy::decode_and_get_object>(placement));
    }

    return object.value_len; // 返回object大小
  }

  // 删除指定条带的所有数据
  void Coordinator::request_delete_by_stripe(std::vector<unsigned int> stripe_ids)
  {
    std::unordered_set<std::string> objects_key;
    int num_of_stripes = (int)stripe_ids.size();
    if (IF_DEBUG) {
        std::cout << "[DELETE] Delete " << num_of_stripes << " stripes!\n";
    }
    DeletePlan delete_info;
    for (int i = 0; i < num_of_stripes; i++) {
      auto &stripe = stripe_table_[stripe_ids[i]];
      // 收集条带涉及的所有object key
      for (auto key : stripe.objects) {
        objects_key.insert(key);
      }
      int n = stripe.ec->k + stripe.ec->m;
      my_assert(n == (int)stripe.blocks2nodes.size());
      // 收集所有块的位置信息
      for (int j = 0; j < n; j++) {
        delete_info.block_ids.push_back(stripe.block_ids[j]);
        auto &node = node_table_[stripe.blocks2nodes[j]];
        delete_info.blocks_info.push_back({node.node_ip, node.node_port});
      }
    }

    if (!IF_SIMULATION) {
      // 随机选择一个proxy处理删除
      int selected_proxy_id = random_index(cluster_table_.size());
      std::string location = proxy_endpoint_key(
          cluster_table_[selected_proxy_id].proxy_ip,
          cluster_table_[selected_proxy_id].proxy_port);
      async_simple::coro::syncAwait(
          main_proxies_[location]->call<&Proxy::delete_blocks>(delete_info));
    }

    // 更新元数据
    for (int i = 0; i < num_of_stripes; i++) {
      for (auto it = cluster_table_.begin(); it != cluster_table_.end(); it++) {
        Cluster& cluster = it->second;
        // 从集群的 holding_stripe_ids 中移除条带
        cluster.holding_stripe_ids.erase(stripe_ids[i]);
      }
      // 从 stripe_table_ 中删除条带
      stripe_table_.erase(stripe_ids[i]);
    }
    mutex_.lock();
    // 从 commited_object_table_ 中删除相关对象
    for (auto key : objects_key) {
      commited_object_table_.erase(key);
    }
    mutex_.unlock();
    if (IF_DEBUG) {
      std::cout << "[DELETE] Finish delete!" << std::endl;
    }
  }

  // 查看当前所有条带的ID列表
  std::vector<unsigned int> Coordinator::list_stripes()
  {
    std::vector<unsigned int> stripe_ids;
    for (auto it = stripe_table_.begin(); it != stripe_table_.end(); it++) {
      stripe_ids.push_back(it->first);
    }
    return stripe_ids;
  }

  // 处理修复failed_ids <节点/块>ID列表，指定条带 stripe_id (-1表示按节点修复)
  RepairResp Coordinator::request_repair(std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_repair(failed_ids, stripe_id, response);
    return response;
  }

  // 处理修复failed_ids <节点/块>ID列表，指定条带 stripe_id (-1表示按节点修复)
  RepairResp Coordinator::request_flow_repair(std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_flow_repair(failed_ids, stripe_id, response);
    return response;
  }

  RepairResp Coordinator::request_flow_unordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_flow_unordered_concurrency_repair(failed_ids, stripe_id, response);
    return response;
  }

  RepairResp Coordinator::request_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_flow_ordered_concurrency_repair(failed_ids, stripe_id, response);
    return response;
  }

  RepairResp Coordinator::request_join_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_join_flow_ordered_concurrency_repair(failed_ids, stripe_id, response);
    return response;
  }

  // 合并条带
  MergeResp Coordinator::request_merge(int step_size)
  {
    my_assert(step_size == ec_schema_.x && !merged_flag_);
    MergeResp response;
    do_stripe_merge(response, step_size);
    return response;
  }

  // 备份初始块放置 (Placement)
  void Coordinator::request_snapshot_metadata()
  {
    initial_placement_.clear();
    for (auto& pair : stripe_table_) {
      // 仅拷贝每个条带的 blocks2nodes 数组，完全避开复杂对象和指针的拷贝
      initial_placement_[pair.first] = pair.second.blocks2nodes;
    }
    std::cout << "[TEST] Initial placement saved. Stripe count: " << initial_placement_.size() << std::endl;
  }

  // 还原初始块放置 (Placement)
  void Coordinator::request_revert_metadata()
  {
    for (auto& pair : initial_placement_) {
      if (stripe_table_.find(pair.first) != stripe_table_.end()) {
        // 将 blocks2nodes 数组覆盖回去，恢复到故障发生前的完美状态
        stripe_table_[pair.first].blocks2nodes = pair.second;
      }
    }
    std::cout << "[TEST] Placement reverted to initial state." << std::endl;
  }
}