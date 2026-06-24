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
    rpc_server_->register_handler<&Coordinator::request_random_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_flow_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_flow_unordered_concurrency_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_flow_ordered_concurrency_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_join_flow_ordered_concurrency_repair>(this);
    rpc_server_->register_handler<&Coordinator::request_merge>(this);
    rpc_server_->register_handler<&Coordinator::list_stripes>(this);
    rpc_server_->register_handler<&Coordinator::request_snapshot_metadata>(this);
    rpc_server_->register_handler<&Coordinator::request_revert_metadata>(this);

    cur_stripe_id_ = 0;
    cur_block_id_ = 0;
    time_ = 0;
    
    init_cluster_info();
    init_proxy_info();
    if (IF_DEBUG) {
      std::cout << "Start the coordinator! " << ip << ":" << port << std::endl;
    }
  }

  Coordinator::~Coordinator() { rpc_server_->stop(); }

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
    ec_schema_.ec = ec_factory(paras.ec_type, paras.cp);
    reset_metadata();
  }

  void Coordinator::set_flow_repair_max_parallel(int n)
  {
    (void)n;
  }

  SetResp Coordinator::request_set(
              std::vector<std::pair<std::string, size_t>> objects)
  {
    int num_of_objects = (int)objects.size();
    my_assert(num_of_objects > 0);
    mutex_.lock();
    for (int i = 0; i < num_of_objects; i++) {
      std::string key = objects[i].first;
      if (commited_object_table_.contains(key)) {
        mutex_.unlock();
        my_assert(false);
      }
    }
    mutex_.unlock();

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
    init_placement_info(placement, objects[0].first,
                        total_value_len, ec_schema_.block_size);

    int num_of_stripes = total_value_len / (ec_schema_.ec->k * ec_schema_.block_size);
    size_t left_value_len = total_value_len;
    int cumulative_len = 0, obj_idx = 0;
    for (int i = 0; i < num_of_stripes; i++) {
      left_value_len -= ec_schema_.ec->k * ec_schema_.block_size;
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

    unsigned int node_id = stripe_table_[cur_stripe_id_ - 1].blocks2nodes[0];
    unsigned int selected_cluster_id = node_table_[node_id].map2cluster;
    std::string selected_proxy_ip = cluster_table_[selected_cluster_id].proxy_ip;
    int selected_proxy_port = cluster_table_[selected_cluster_id].proxy_port;

    if (IF_DEBUG) {
      std::cout << "[SET] Select proxy " << selected_proxy_ip << ":" 
                << selected_proxy_port << " in cluster "
                << selected_cluster_id << " to handle encode and set!" << std::endl;
    }

    if (IF_SIMULATION) {
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

    SetResp response;
    response.proxy_ip = selected_proxy_ip;
    response.proxy_port = selected_proxy_port + SOCKET_PORT_OFFSET;

    return response;
  }

  void Coordinator::commit_object(std::vector<std::string> keys, bool commit)
  {
    int num = (int)keys.size();
    if (commit) {
      mutex_.lock();
      for (int i = 0; i < num; i++) {
        my_assert(commited_object_table_.contains(keys[i]) == false &&
                  updating_object_table_.contains(keys[i]) == true);
        commited_object_table_[keys[i]] = updating_object_table_[keys[i]];
        updating_object_table_.erase(keys[i]);
      }
      mutex_.unlock();
    } else {
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

  size_t Coordinator::request_get(std::string key, std::string client_ip,
                                  int client_port)
  {
    mutex_.lock();
    if (commited_object_table_.contains(key) == false) {
      mutex_.unlock();
      my_assert(false);
    }
    ObjectInfo &object = commited_object_table_[key];
    mutex_.unlock();

    PlacementInfo placement;
    int stripe_num = (int)object.stripes.size();
    my_assert(stripe_num > 0);
  
    unsigned int stripe_id = object.stripes[0];
    Stripe &stripe = stripe_table_[stripe_id];
    init_placement_info(placement, key, object.value_len, stripe.block_size);

    for (auto stripe_id : object.stripes) {
      Stripe &stripe = stripe_table_[stripe_id];
      placement.stripe_ids.push_back(stripe_id);

      int num_of_object_in_a_stripe = (int)stripe.objects.size();
      int offset = 0;
      for (int i = 0; i < num_of_object_in_a_stripe; i++) {
        if (stripe.objects[i] != key) {
          int t_object_len = commited_object_table_[stripe.objects[i]].value_len;
          offset += t_object_len / stripe.block_size;
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

    return object.value_len;
  }

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
      for (auto key : stripe.objects) {
        objects_key.insert(key);
      }
      int n = stripe.ec->k + stripe.ec->m;
      my_assert(n == (int)stripe.blocks2nodes.size());
      for (int j = 0; j < n; j++) {
        delete_info.block_ids.push_back(stripe.block_ids[j]);
        auto &node = node_table_[stripe.blocks2nodes[j]];
        delete_info.blocks_info.push_back({node.node_ip, node.node_port});
      }
    }

    if (!IF_SIMULATION) {
      int selected_proxy_id = random_index(cluster_table_.size());
      std::string location = proxy_endpoint_key(
          cluster_table_[selected_proxy_id].proxy_ip,
          cluster_table_[selected_proxy_id].proxy_port);
      async_simple::coro::syncAwait(
          main_proxies_[location]->call<&Proxy::delete_blocks>(delete_info));
    }

    for (int i = 0; i < num_of_stripes; i++) {
      for (auto it = cluster_table_.begin(); it != cluster_table_.end(); it++) {
        Cluster& cluster = it->second;
        cluster.holding_stripe_ids.erase(stripe_ids[i]);
      }
      stripe_table_.erase(stripe_ids[i]);
    }
    mutex_.lock();
    for (auto key : objects_key) {
      commited_object_table_.erase(key);
    }
    mutex_.unlock();
    if (IF_DEBUG) {
      std::cout << "[DELETE] Finish delete!" << std::endl;
    }
  }

  std::vector<unsigned int> Coordinator::list_stripes()
  {
    std::vector<unsigned int> stripe_ids;
    for (auto it = stripe_table_.begin(); it != stripe_table_.end(); it++) {
      stripe_ids.push_back(it->first);
    }
    return stripe_ids;
  }

  RepairResp Coordinator::request_repair(std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_repair(failed_ids, stripe_id, response);
    return response;
  }

  RepairResp Coordinator::request_random_repair(std::vector<unsigned int> failed_ids, int stripe_id)
  {
    RepairResp response{};
    do_random_repair(failed_ids, stripe_id, response);
    return response;
  }

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

  MergeResp Coordinator::request_merge(int step_size)
  {
    my_assert(step_size == ec_schema_.x && !merged_flag_);
    MergeResp response;
    do_stripe_merge(response, step_size);
    return response;
  }

  void Coordinator::request_snapshot_metadata()
  {
    initial_placement_.clear();
    for (auto& pair : stripe_table_) {
      initial_placement_[pair.first] = pair.second.blocks2nodes;
    }
    std::cout << "[TEST] Initial placement saved. Stripe count: " << initial_placement_.size() << std::endl;
  }

  void Coordinator::request_revert_metadata()
  {
    for (auto& pair : initial_placement_) {
      if (stripe_table_.find(pair.first) != stripe_table_.end()) {
        stripe_table_[pair.first].blocks2nodes = pair.second;
      }
    }
    std::cout << "[TEST] Placement reverted to initial state." << std::endl;
  }
}
