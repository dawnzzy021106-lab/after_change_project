#include "coordinator.h"

namespace ECProject
{
  void Coordinator::do_stripe_merge(MergeResp& response, int step_size)
  {
    ECFAMILY ec_family = check_ec_family(ec_schema_.ec_type);
    if(ec_family == RSCodes) {
      rs_merge(response, step_size);
    } else if (ec_schema_.ec_type == AZURE_LRC) {
      azu_lrc_merge(response, step_size);
    } else if(ec_family == PCs){
      pc_merge(response, step_size);
    }
  }

  void Coordinator::rs_merge(MergeResp& response, int step_size)
  {
    struct timeval start_time, end_time;
    struct timeval m_start_time, m_end_time;
    int x = ec_schema_.x;
    int tot_stripe_num = int(stripe_table_.size());
    int stripe_cnt = 0;
    std::vector<std::vector<unsigned int>> new_merge_groups;
    // for simulation
    int t_cross_cluster = 0;
    int io_cnt = 0;
    double merging_time = 0;
    double computing_time = 0;
    double meta_time = 0;
    double cross_cluster_time = 0;

    ErasureCode *temp_ec = new_ec_for_merge(step_size);

    // for each merge group, every x stripes a merge group
    for (auto& merge_group : merge_groups_) {
      int group_len = (int)merge_group.size();
      my_assert(group_len % step_size == 0);
      std::vector<unsigned int> s_merge_group;
      for (int mi = 0; mi < group_len; mi += step_size) {
        gettimeofday(&start_time, NULL);
        gettimeofday(&m_start_time, NULL);
        std::vector<unsigned int> parity_node_ids;
        std::unordered_set<int> old_stripe_id_set;
        std::unordered_map<unsigned int, LocationInfo> block_location;
        DeletePlan old_parities;
        std::unordered_set<int> old_parities_cluster_set;
        MainRecalPlan main_plan;
        // merge and generate new stripe information
        size_t block_size = ec_schema_.block_size;
        Stripe larger_stripe;
        larger_stripe.stripe_id = cur_stripe_id_++;
        larger_stripe.block_size = block_size;
        int seri_num = 0;
        for (int ni = mi; ni < mi + step_size; ni++) {
          unsigned int t_stripe_id = merge_group[ni];
          old_stripe_id_set.insert(t_stripe_id);
          Stripe &t_stripe = stripe_table_[t_stripe_id];
          int k = t_stripe.ec->k;
          int m = t_stripe.ec->m;
          larger_stripe.objects.insert(larger_stripe.objects.end(),
                                      t_stripe.objects.begin(),
                                      t_stripe.objects.end());
          for (int i = 0; i < k + m; i++) {
            unsigned int t_node_id = t_stripe.blocks2nodes[i];
            unsigned int t_cluster_id = node_table_[t_node_id].map2cluster;
            if (i < k) {  // data blocks
              larger_stripe.blocks2nodes.push_back(t_node_id);
              larger_stripe.block_ids.push_back(t_stripe.block_ids[i]);
              cluster_table_[t_cluster_id].holding_stripe_ids.insert(
                  larger_stripe.stripe_id);
              if (block_location.find(t_cluster_id) == block_location.end()) {
                LocationInfo new_location;
                new_location.cluster_id = t_cluster_id;
                new_location.proxy_port = cluster_table_[t_cluster_id].proxy_port;
                new_location.proxy_ip = cluster_table_[t_cluster_id].proxy_ip;
                block_location[t_cluster_id] = new_location;
              }
              LocationInfo& t_location = block_location[t_cluster_id];
              Node &t_node = node_table_[t_node_id];
              int new_idx = seri_num * k + i;
              t_location.blocks_info.push_back(
                  std::make_pair(new_idx, std::make_pair(t_node.node_ip, t_node.node_port)));
              t_location.block_ids.push_back(t_stripe.block_ids[i]);
            } else {  // parity blocks
              Node& t_node = node_table_[t_node_id];
              // for new locations
              parity_node_ids.push_back(t_node_id);
              // for delete
              old_parities.blocks_info.push_back(
                  std::make_pair(t_node.node_ip, t_node.node_port));
              old_parities.block_ids.push_back(t_stripe.block_ids[i]);
              old_parities_cluster_set.insert(t_cluster_id);
            }
          }
          seri_num++;
        }
        unsigned int parity_cluster_id = 0;
        // generate new parity blocks
        for (int i = 0; i < temp_ec->m; i++) {
          unsigned int t_node_id = parity_node_ids[i];
          auto& t_node = node_table_[t_node_id];
          larger_stripe.blocks2nodes.push_back(t_node_id);
          larger_stripe.block_ids.push_back(cur_block_id_++);
          parity_cluster_id = t_node.map2cluster;
          cluster_table_[parity_cluster_id].holding_stripe_ids.insert(
              larger_stripe.stripe_id);
          main_plan.new_locations.push_back(
            std::make_pair(temp_ec->k + i,
            std::make_pair(t_node.node_ip, t_node.node_port)));
          main_plan.new_parity_block_ids.push_back(cur_block_id_ - 1);
        }

        if (IF_DEBUG) {
          std::cout << "Finish generate new stripe information for stripe"
                    << larger_stripe.stripe_id << std::endl;
        }

        // find out necessary relocation
        std::vector<unsigned int> blocks_to_move;
        std::vector<unsigned int> src_nodes;
        std::vector<unsigned int> des_nodes;
        std::unordered_map<int, int> cluster_block_cnt;
        std::unordered_map<unsigned int, std::vector<int>> cluster_blocks;
        std::vector<unsigned int> free_clusters;
        for (unsigned int i = 0; i < num_of_clusters_; i++) {
          free_clusters.push_back(i);
        }

        int block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int cluster_id = node_table_[node_id].map2cluster;
          if (cluster_block_cnt.find(cluster_id) == cluster_block_cnt.end()) {
            cluster_block_cnt[cluster_id] = 1;
            cluster_blocks[cluster_id] = std::vector({block_idx});
          } else {
            cluster_block_cnt[cluster_id] += 1;
            cluster_blocks[cluster_id].push_back(block_idx);
          }
          block_idx++;
          auto it = std::find(free_clusters.begin(), free_clusters.end(),
                              (unsigned)cluster_id);
          if (it != free_clusters.end()) {
            free_clusters.erase(it);
          }
        }
        std::vector<std::pair<int, int>> 
            sorted_clusters(cluster_block_cnt.begin(), cluster_block_cnt.end());
        std::sort(sorted_clusters.begin(), sorted_clusters.end(), cmp_descending);
        int m = temp_ec->m;
        int cluster_num = (int)sorted_clusters.size();
        size_t free_cluster_num = free_clusters.size();

        for (int i = 0; i < cluster_num; i++) {
          auto& kv1 = sorted_clusters[i];
          if (kv1.second <= m) {
            break;
          }
          
          unsigned int src_cid = kv1.first;
          bool flag = false;
          for (int ii = cluster_num - 1; ii > i; ii--) {
            auto& kv2 = sorted_clusters[ii];
            if(kv2.second + kv1.second <= 2 * m) {
              
              unsigned int des_cid = kv2.first;
              std::vector<unsigned int> free_nodes;
              for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
                free_nodes.push_back(des_cid * num_of_nodes_per_cluster_ + j);
              }
              auto& des_blocks = cluster_blocks[des_cid];
              for (auto block_idx : des_blocks) {
                unsigned int des_nid = larger_stripe.blocks2nodes[block_idx];
                auto it = std::find(free_nodes.begin(), free_nodes.end(), des_nid);
                if (it != free_nodes.end()) {
                  free_nodes.erase(it);
                }
              }
              size_t free_node_num = free_nodes.size();
              auto& src_blocks = cluster_blocks[src_cid];
              int block_num = (int)src_blocks.size();
              for (int j = m; j < block_num; j++) {
                int block_idx = src_blocks[j];
                blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
                unsigned int src_nid = larger_stripe.blocks2nodes[block_idx];
                src_nodes.push_back(src_nid);
                // select a destination node
                int rand_idx = random_index(free_node_num);
                unsigned int des_nid = free_nodes[rand_idx];
                larger_stripe.blocks2nodes[block_idx] = des_nid;
                des_nodes.push_back(des_nid);
                auto it = std::find(free_nodes.begin(), free_nodes.end(), des_nid);
                if (it != free_nodes.end()) {
                  free_nodes.erase(it);
                  free_node_num--;
                }
              }
              kv2.second += block_num - m;
              flag = true;
              break;
            }
          }
          if (!flag) {
            int rand_idx = random_index(free_cluster_num);
            unsigned int des_cid = free_clusters[rand_idx];
            std::vector<unsigned int> free_nodes;
            for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
              free_nodes.push_back(des_cid * num_of_nodes_per_cluster_ + j);
            }
            size_t free_node_num = free_nodes.size();
            auto& src_blocks = cluster_blocks[src_cid];
            int block_num = (int)src_blocks.size();
            for (int j = m; j < block_num; j++) {
              int block_idx = src_blocks[j];
              blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
              unsigned int src_nid = larger_stripe.blocks2nodes[block_idx];
              src_nodes.push_back(src_nid);
              // select a destination node
              int rand_idx = random_index(free_node_num);
              unsigned int des_nid = free_nodes[rand_idx];
              larger_stripe.blocks2nodes[block_idx] = des_nid;
              des_nodes.push_back(des_nid);
              auto it = std::find(free_nodes.begin(), free_nodes.end(), des_nid);
              if (it != free_nodes.end()) {
                free_nodes.erase(it);
                free_node_num--;
              }
            }
            auto it = std::find(free_clusters.begin(), free_clusters.end(), des_cid);
            if (it != free_clusters.end()) {
              free_clusters.erase(it);
              free_cluster_num--;
            }
          }
        }

        std::vector<int> node_map(num_of_clusters_ * num_of_nodes_per_cluster_, 0);
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          node_map[idx]++;
        }
        block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          if (node_map[idx] > 1) {
            int cid = idx / num_of_nodes_per_cluster_;
            int rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            while (node_map[rand_idx] != 0) {
              rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            } 
            blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
            src_nodes.push_back(node_id);
            des_nodes.push_back((unsigned int)rand_idx);
            larger_stripe.blocks2nodes[block_idx] = (unsigned int)rand_idx;
            node_map[rand_idx]++;
            node_map[idx]--;
          }
          block_idx++;
        }

        RelocatePlan reloc_plan;
        reloc_plan.block_size = block_size;
        int move_num = (int)blocks_to_move.size();
        if (move_num > 0) {
          for (int i = 0; i < move_num; i++) {
            reloc_plan.blocks_to_move.push_back(blocks_to_move[i]);
            auto& src_node = node_table_[src_nodes[i]];
            reloc_plan.src_nodes.push_back(std::make_pair(src_node.map2cluster,
                std::make_pair(src_node.node_ip, src_node.node_port)));
            auto& des_node = node_table_[des_nodes[i]];
            reloc_plan.des_nodes.push_back(std::make_pair(des_node.map2cluster,
                std::make_pair(des_node.node_ip, des_node.node_port)));
            if (src_node.map2cluster != des_node.map2cluster) {
              t_cross_cluster++;
            }
          }
          if (IF_DEBUG) {
            std::cout << "[MERGE] all blocks to relocate:";
            for (int ii = 0; ii < move_num; ii++) {
                std::cout << blocks_to_move[ii] << "[" << src_nodes[ii] 
                          << "->" << des_nodes[ii] << "] ";
            }
            std::cout << std::endl;
          }
        }

        if (IF_DEBUG) {
          std::cout << "Find out block to relocate!" << std::endl;
        }

        // parity block recalculation
        // generate main plan
        main_plan.ec_type = ec_schema_.ec_type;
        temp_ec->get_coding_parameters(main_plan.cp);
        main_plan.cluster_id = parity_cluster_id;
        main_plan.cp.x = step_size;
        main_plan.block_size = block_size;
        main_plan.partial_decoding = ec_schema_.partial_decoding;
        for (auto& kv : block_location) {
          if (kv.first == parity_cluster_id) {
            main_plan.inner_cluster_help_blocks_info = kv.second.blocks_info;
            main_plan.inner_cluster_help_block_ids = kv.second.block_ids;
          } else {
            main_plan.help_clusters_info.push_back(kv.second);
          }
        }
        
        // simulate recalculation
        simulation_recalculation(main_plan, t_cross_cluster, io_cnt);
        io_cnt += 2 * (int)reloc_plan.blocks_to_move.size();
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec
            + (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000.0;

        auto lock_ptr = std::make_shared<std::mutex>();
        auto send_main_plan = [this, main_plan, parity_cluster_id,
                               lock_ptr, &computing_time,
                               &cross_cluster_time]() mutable
        {
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[parity_cluster_id].proxy_ip,
              cluster_table_[parity_cluster_id].proxy_port);
          auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                          ->call_for<&Proxy::main_recal>(
                              std::chrono::seconds{0}, main_plan)).value();
          lock_ptr->lock();
          computing_time += resp.computing_time;
          cross_cluster_time += resp.cross_cluster_time;
          lock_ptr->unlock();
          if (IF_DEBUG) {
            std::cout << "Selected main proxy " << chosen_proxy << std::endl;
          }
        };

        auto send_help_plan = [this, block_size, block_location, main_plan,
                              parity_cluster_id](unsigned int cluster_id) mutable
        {
          HelpRecalPlan help_plan;
          help_plan.ec_type = ec_schema_.ec_type;
          help_plan.cp = main_plan.cp;
          help_plan.block_size = block_size;
          help_plan.partial_decoding = ec_schema_.partial_decoding;
          help_plan.main_proxy_port = cluster_table_[parity_cluster_id].proxy_port + SOCKET_PORT_OFFSET;
          help_plan.main_proxy_ip = cluster_table_[parity_cluster_id].proxy_ip;
          auto& location = block_location[cluster_id];
          help_plan.inner_cluster_help_blocks_info = location.blocks_info;
          help_plan.inner_cluster_help_block_ids = location.block_ids;
          for (int i = 0; i < int(main_plan.new_locations.size()); i++) {
            help_plan.new_parity_block_idxs.push_back(main_plan.new_locations[i].first);
          }
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[cluster_id].proxy_ip,
              cluster_table_[cluster_id].proxy_port);
          async_simple::coro::syncAwait(help_proxies_[chosen_proxy]
              ->call_for<&Proxy::help_recal>(std::chrono::seconds{0}, help_plan));
          if (IF_DEBUG) {
            std::cout << "Selected help proxy " << chosen_proxy << std::endl;
          }
        };

        if (!IF_SIMULATION) {
          // recalculation
          try {
            if (IF_DEBUG) {
              std::cout << "[Parities Recalculation] Send main and help proxy plans!\n";
            }
            std::thread my_main_thread(send_main_plan);
            std::vector<std::thread> senders;
            for (auto& location : main_plan.help_clusters_info) {
              if ((IF_DIRECT_FROM_NODE && m < (int)location.block_ids.size()
                  && ec_schema_.partial_decoding) || !IF_DIRECT_FROM_NODE) {
                senders.push_back(std::thread(send_help_plan, location.cluster_id));
              }
            }
            for (int j = 0; j < int(senders.size()); j++) {
              senders[j].join();
            }
            my_main_thread.join();
          }
          catch (const std::exception &e)
          {
            std::cerr << e.what() << '\n';
          }

          // blocks relocation
          if (move_num > 0) {
            unsigned int ran_cluster_id = (unsigned int)random_index(num_of_clusters_);
            std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[ran_cluster_id].proxy_ip,
              cluster_table_[ran_cluster_id].proxy_port);
            auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                          ->call_for<&Proxy::block_relocation>(
                              std::chrono::seconds{0}, reloc_plan)).value();
            cross_cluster_time += resp.cross_cluster_time;
          }

          // delete old parities
          int idx = random_index(old_parities_cluster_set.size());
          int del_cluster_id = *(std::next(old_parities_cluster_set.begin(), idx));
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[del_cluster_id].proxy_ip,
              cluster_table_[del_cluster_id].proxy_port);
          async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                ->call_for<&Proxy::delete_blocks>(std::chrono::seconds{0}, old_parities));
        }

        gettimeofday(&m_start_time, NULL);
        // update metadata
        for (auto it = old_stripe_id_set.begin();
            it != old_stripe_id_set.end(); it++) {
          for (auto& object : stripe_table_[*it].objects) {
            auto& vec = commited_object_table_[object].stripes;
            vec.erase(std::remove(vec.begin(), vec.end(), *it), vec.end());
            vec.push_back(larger_stripe.stripe_id);
          }
          stripe_table_.erase(*it);
          for (auto& kv : cluster_table_) {
            kv.second.holding_stripe_ids.erase(*it);
          }
        }
        stripe_table_[larger_stripe.stripe_id] = larger_stripe;
        stripe_table_[larger_stripe.stripe_id].ec = clone_ec(ec_schema_.ec_type, temp_ec);
        s_merge_group.push_back(larger_stripe.stripe_id);
        stripe_cnt += step_size;
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec
            + (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000.0;
        gettimeofday(&end_time, NULL);
        double temp_time = end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
        merging_time += temp_time;
        std::cout << "[Merging] Process " << stripe_cnt << "/" << tot_stripe_num
                  << " total_time:" << merging_time << " latest:" << temp_time
                  << std::endl;
      }
      new_merge_groups.push_back(s_merge_group);
    }
    merge_groups_.clear();
    merge_groups_ = new_merge_groups;
    ec_schema_.set_ec(temp_ec);
    ec_schema_.x /= step_size;
    merged_flag_ = true;
    // response
    response.computing_time = computing_time;
    response.cross_cluster_time = cross_cluster_time;
    response.merging_time = merging_time;
    response.meta_time = meta_time;
    response.cross_cluster_transfers = t_cross_cluster;
    response.io_cnt = io_cnt;

    if (IF_DEBUG) {
      print_placement_result("After Merging:");
    }
  }

  void Coordinator::azu_lrc_merge(MergeResp& response, int step_size)
  {
    struct timeval start_time, end_time;
    struct timeval m_start_time, m_end_time;
    int x = ec_schema_.x;
    int tot_stripe_num = int(stripe_table_.size());
    int stripe_cnt = 0;
    std::vector<std::vector<unsigned int>> new_merge_groups;
    // for simulation
    int t_cross_cluster = 0;
    int io_cnt = 0;
    double merging_time = 0;
    double computing_time = 0;
    double meta_time = 0;
    double cross_cluster_time = 0;

    ErasureCode *temp_ec = new_ec_for_merge(step_size);
    CodingParameters new_cp;
    temp_ec->get_coding_parameters(new_cp);
    CodingParameters old_cp;
    ec_schema_.ec->get_coding_parameters(old_cp);

    // for each merge group, every x stripes a merge group
    for (auto& merge_group : merge_groups_) {
      int group_len = (int)merge_group.size();
      my_assert(group_len % step_size == 0);
      std::vector<unsigned int> s_merge_group;
      for (int mi = 0; mi < group_len; mi += step_size) {
        gettimeofday(&start_time, NULL);
        gettimeofday(&m_start_time, NULL);
        std::vector<unsigned int> parity_node_ids;
        std::unordered_set<int> old_stripe_id_set;
        std::unordered_map<unsigned int, LocationInfo> block_location;
        DeletePlan old_parities;
        std::unordered_set<int> old_parities_cluster_set;
        MainRecalPlan main_plan;
        // merge and generate new stripe information
        size_t block_size = ec_schema_.block_size;
        Stripe larger_stripe;
        larger_stripe.stripe_id = cur_stripe_id_++;
        larger_stripe.block_size = block_size;
        for (int i = 0; i < new_cp.k + new_cp.m; i++) {
          larger_stripe.blocks2nodes.push_back(0);
          larger_stripe.block_ids.push_back(0);
        }
        int seri_num = 0;
        for (int ni = mi; ni < mi + step_size; ni++) {
          unsigned int t_stripe_id = merge_group[ni];
          old_stripe_id_set.insert(t_stripe_id);
          Stripe &t_stripe = stripe_table_[t_stripe_id];
          int k = old_cp.k;
          int m = old_cp.m;
          int l = old_cp.l;
          int g = old_cp.g;
          larger_stripe.objects.insert(larger_stripe.objects.end(),
                                      t_stripe.objects.begin(),
                                      t_stripe.objects.end());
          for (int i = 0; i < k + m; i++) {
            unsigned int t_node_id = t_stripe.blocks2nodes[i];
            unsigned int t_cluster_id = node_table_[t_node_id].map2cluster;
            if (i < k || i >= k + g) {  // data blocks or local parity blocks
              int new_idx = seri_num * k + i;
              if (i >= k + g) {
                new_idx = x * k + g + seri_num * l + (i - k - g);
              }
              larger_stripe.blocks2nodes[new_idx] = t_node_id;
              larger_stripe.block_ids[new_idx] = t_stripe.block_ids[i];
              cluster_table_[t_cluster_id].holding_stripe_ids.insert(
                  larger_stripe.stripe_id);
              if (i < k) {
                if (block_location.find(t_cluster_id) == block_location.end()) {
                  LocationInfo new_location;
                  new_location.cluster_id = t_cluster_id;
                  new_location.proxy_port = cluster_table_[t_cluster_id].proxy_port;
                  new_location.proxy_ip = cluster_table_[t_cluster_id].proxy_ip;
                  block_location[t_cluster_id] = new_location;
                }
                LocationInfo& t_location = block_location[t_cluster_id];
                Node& t_node = node_table_[t_node_id];
                t_location.blocks_info.push_back(
                    std::make_pair(new_idx, std::make_pair(t_node.node_ip, t_node.node_port)));
                t_location.block_ids.push_back(t_stripe.block_ids[i]);
              }
            } else {  // global parity blocks
              Node& t_node = node_table_[t_node_id];
              // for new locations
              parity_node_ids.push_back(t_node_id);
              // for delete
              old_parities.blocks_info.push_back(
                  std::make_pair(t_node.node_ip, t_node.node_port));
              old_parities.block_ids.push_back(t_stripe.block_ids[i]);
              old_parities_cluster_set.insert(t_cluster_id);
            }
          }
          seri_num++;
        }

        unsigned int global_cluster_id = 0;
        // generate new parity blocks
        for (int i = 0; i < new_cp.g; i++) {
          unsigned int t_node_id = parity_node_ids[i];
          auto& t_node = node_table_[t_node_id];
          int new_idx = new_cp.k + i;
          larger_stripe.blocks2nodes[new_idx] = t_node_id;
          larger_stripe.block_ids[new_idx] = cur_block_id_++;
          global_cluster_id = t_node.map2cluster;
          cluster_table_[global_cluster_id].holding_stripe_ids.insert(
              larger_stripe.stripe_id);
          main_plan.new_locations.push_back(
            std::make_pair(new_idx,
            std::make_pair(t_node.node_ip, t_node.node_port)));
          main_plan.new_parity_block_ids.push_back(cur_block_id_ - 1);
        }

        if (IF_DEBUG) {
          std::cout << "Finish generate new stripe information for stripe"
                    << larger_stripe.stripe_id << std::endl;
        }

        // find out necessary relocation
        std::vector<unsigned int> blocks_to_move;
        std::vector<unsigned int> src_nodes;
        std::vector<unsigned int> des_nodes;
        std::unordered_map<int, int> cluster_block_cnt;
        std::unordered_map<unsigned int, std::vector<int>> cluster_blocks;
        std::vector<unsigned int> free_clusters;
        for (unsigned int i = 0; i < num_of_clusters_; i++) {
          free_clusters.push_back(i);
        }

        int block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int cluster_id = node_table_[node_id].map2cluster;
          if (cluster_block_cnt.find(cluster_id) == cluster_block_cnt.end()) {
            cluster_block_cnt[cluster_id] = 1;
            cluster_blocks[cluster_id] = std::vector({block_idx});
          } else {
            cluster_block_cnt[cluster_id] += 1;
            cluster_blocks[cluster_id].push_back(block_idx);
          }
          block_idx++;
          auto it = std::find(free_clusters.begin(), free_clusters.end(),
                              (unsigned)cluster_id);
          if (it != free_clusters.end()) {
            free_clusters.erase(it);
          }
        }

        std::vector<std::pair<int, int>> 
            sorted_clusters(cluster_block_cnt.begin(), cluster_block_cnt.end());
        std::sort(sorted_clusters.begin(), sorted_clusters.end(), cmp_descending);
        int cluster_num = (int)sorted_clusters.size();
        size_t free_cluster_num = free_clusters.size();

        for (int ii = 0; ii < cluster_num; ii++) {
          std::unordered_map<int, std::vector<int>> group_blocks;
          unsigned int cluster_id = sorted_clusters[ii].first;
          bool flag = if_subject_to_fault_tolerance_lrc(temp_ec,
              cluster_blocks[cluster_id], group_blocks);
          if (!flag) {
            int groups_num = (int)group_blocks.size();
            int cnt = 0;
            int cnt_idx = 1;
            for (auto& kv : group_blocks) {
              auto& src_blocks = kv.second;
              int block_num = (int)src_blocks.size();
              int rand_idx = 0;
              unsigned int des_cid = 0;
              if (cnt + block_num > new_cp.g + cnt_idx || !cnt) {
                rand_idx = random_index(free_cluster_num);
                des_cid = free_clusters[rand_idx];
              }
              std::vector<unsigned int> free_nodes;
              for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
                free_nodes.push_back(des_cid * num_of_nodes_per_cluster_ + j);
              }
              size_t free_node_num = free_nodes.size();
              for (int j = 0; j < block_num; j++) {
                int block_idx = src_blocks[j];
                blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
                unsigned int src_nid = larger_stripe.blocks2nodes[block_idx];
                src_nodes.push_back(src_nid);
                // select a destination node
                int rand_idx = random_index(free_node_num);
                unsigned int des_nid = free_nodes[rand_idx];
                larger_stripe.blocks2nodes[block_idx] = des_nid;
                des_nodes.push_back(des_nid);
                auto it = std::find(free_nodes.begin(), free_nodes.end(), des_nid);
                if (it != free_nodes.end()) {
                  free_nodes.erase(it);
                  free_node_num--;
                }
              }
              if (cnt + block_num > new_cp.g + cnt_idx || !cnt) {
                auto it = std::find(free_clusters.begin(), free_clusters.end(), des_cid);
                if (it != free_clusters.end()) {
                  free_clusters.erase(it);
                  free_cluster_num--;
                }
              }
              cnt += block_num;
              cnt_idx++;
              sorted_clusters[ii].second -= block_num;
              groups_num--;
              if (sorted_clusters[ii].second <= new_cp.g + groups_num) {
                break;
              }
            }
          }
        }

        std::vector<int> node_map(num_of_clusters_ * num_of_nodes_per_cluster_, 0);
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          node_map[idx]++;
        }
        block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          if (node_map[idx] > 1) {
            int cid = idx / num_of_nodes_per_cluster_;
            int rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            while (node_map[rand_idx] != 0) {
              rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            } 
            blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
            src_nodes.push_back(node_id);
            des_nodes.push_back((unsigned int)rand_idx);
            larger_stripe.blocks2nodes[block_idx] = (unsigned int)rand_idx;
            node_map[rand_idx]++;
            node_map[idx]--;
          }
          block_idx++;
        }

        if (IF_DEBUG) {
          std::cout << "Find out block to relocate!" << std::endl;
        }

        RelocatePlan reloc_plan;
        reloc_plan.block_size = block_size;
        int move_num = (int)blocks_to_move.size();
        if (move_num > 0) {
          for (int i = 0; i < move_num; i++) {
            reloc_plan.blocks_to_move.push_back(blocks_to_move[i]);
            auto& src_node = node_table_[src_nodes[i]];
            reloc_plan.src_nodes.push_back(std::make_pair(src_node.map2cluster,
                std::make_pair(src_node.node_ip, src_node.node_port)));
            auto& des_node = node_table_[des_nodes[i]];
            reloc_plan.des_nodes.push_back(std::make_pair(des_node.map2cluster,
                std::make_pair(des_node.node_ip, des_node.node_port)));
            if (src_node.map2cluster != des_node.map2cluster) {
              t_cross_cluster++;
            }
          }
          if (IF_DEBUG) {
            std::cout << "[MERGE] all blocks to relocate:";
            for (int ii = 0; ii < move_num; ii++) {
                std::cout << blocks_to_move[ii] << "[" << src_nodes[ii] 
                          << "->" << des_nodes[ii] << "] ";
            }
            std::cout << std::endl;
          }
        }

        // parity block recalculation
        // generate main plan
        main_plan.ec_type = ec_schema_.ec_type;
        temp_ec->get_coding_parameters(main_plan.cp);
        main_plan.cluster_id = global_cluster_id;
        main_plan.cp.x = step_size;
        main_plan.block_size = block_size;
        main_plan.partial_decoding = ec_schema_.partial_decoding;
        main_plan.cp.local_or_column = false;
        for (auto& kv : block_location) {
          if (kv.first == global_cluster_id) {
            main_plan.inner_cluster_help_blocks_info = kv.second.blocks_info;
            main_plan.inner_cluster_help_block_ids = kv.second.block_ids;
          } else {
            main_plan.help_clusters_info.push_back(kv.second);
          }
        }
        
        // simulate recalculation
        simulation_recalculation(main_plan, t_cross_cluster, io_cnt);
        io_cnt += 2 * (int)reloc_plan.blocks_to_move.size();
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec +
            (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000.0;

        auto lock_ptr = std::make_shared<std::mutex>();
        auto send_main_plan = [this, main_plan, global_cluster_id,
                               lock_ptr, &computing_time,
                               &cross_cluster_time]() mutable
        {
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[global_cluster_id].proxy_ip,
              cluster_table_[global_cluster_id].proxy_port);
          auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                          ->call_for<&Proxy::main_recal>(
                              std::chrono::seconds{0}, main_plan)).value();
          lock_ptr->lock();
          computing_time += resp.computing_time;
          cross_cluster_time += resp.cross_cluster_time;
          lock_ptr->unlock();
          if (IF_DEBUG) {
            std::cout << "Selected main proxy " << chosen_proxy << std::endl;
          }
        };

        auto send_help_plan = [this, block_size, block_location, main_plan,
                              global_cluster_id](unsigned int cluster_id) mutable
        {
          HelpRecalPlan help_plan;
          help_plan.ec_type = ec_schema_.ec_type;
          help_plan.cp = main_plan.cp;
          help_plan.block_size = block_size;
          help_plan.partial_decoding = ec_schema_.partial_decoding;
          help_plan.cp.local_or_column = main_plan.cp.local_or_column;
          help_plan.main_proxy_port = cluster_table_[global_cluster_id].proxy_port + SOCKET_PORT_OFFSET;
          help_plan.main_proxy_ip = cluster_table_[global_cluster_id].proxy_ip;
          auto& location = block_location[cluster_id];
          help_plan.inner_cluster_help_blocks_info = location.blocks_info;
          help_plan.inner_cluster_help_block_ids = location.block_ids;
          for (int i = 0; i < int(main_plan.new_locations.size()); i++) {
            help_plan.new_parity_block_idxs.push_back(main_plan.new_locations[i].first);
          }
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[cluster_id].proxy_ip,
              cluster_table_[cluster_id].proxy_port);
          async_simple::coro::syncAwait(help_proxies_[chosen_proxy]
              ->call_for<&Proxy::help_recal>(std::chrono::seconds{0}, help_plan));
          if (IF_DEBUG) {
            std::cout << "Selected help proxy " << chosen_proxy << std::endl;
          }
        };

        if (!IF_SIMULATION) {
          // recalculation
          try {
            if (IF_DEBUG) {
              std::cout << "[Parities Recalculation] Send main and help proxy plans!\n";
            }
            std::thread my_main_thread(send_main_plan);
            std::vector<std::thread> senders;
            int parity_num = (int)main_plan.new_parity_block_ids.size();
            for (auto& location : main_plan.help_clusters_info) {
              if ((IF_DIRECT_FROM_NODE && parity_num < location.block_ids.size()
                  && ec_schema_.partial_decoding) || !IF_DIRECT_FROM_NODE) {
                senders.push_back(std::thread(send_help_plan, location.cluster_id));
              }
            }
            for (int j = 0; j < int(senders.size()); j++) {
              senders[j].join();
            }
            my_main_thread.join();
          }
          catch (const std::exception &e)
          {
            std::cerr << e.what() << '\n';
          }

          // blocks relocation
          if (move_num > 0) {
            unsigned int ran_cluster_id = (unsigned int)random_index(num_of_clusters_);
            std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[ran_cluster_id].proxy_ip,
              cluster_table_[ran_cluster_id].proxy_port);
            auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                          ->call_for<&Proxy::block_relocation>(
                              std::chrono::seconds{0}, reloc_plan)).value();
            cross_cluster_time += resp.cross_cluster_time;
          }

          // delete old parities
          int idx = random_index(old_parities_cluster_set.size());
          int del_cluster_id = *(std::next(old_parities_cluster_set.begin(), idx));
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[del_cluster_id].proxy_ip,
              cluster_table_[del_cluster_id].proxy_port);
          async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                ->call_for<&Proxy::delete_blocks>(std::chrono::seconds{0}, old_parities));
        }

        gettimeofday(&m_start_time, NULL);
        // update metadata
        for (auto it = old_stripe_id_set.begin();
            it != old_stripe_id_set.end(); it++) {
          for (auto& object : stripe_table_[*it].objects) {
            auto& vec = commited_object_table_[object].stripes;
            vec.erase(std::remove(vec.begin(), vec.end(), *it), vec.end());
            vec.push_back(larger_stripe.stripe_id);
          }
          stripe_table_.erase(*it);
          for (auto& kv : cluster_table_) {
            kv.second.holding_stripe_ids.erase(*it);
          }
        }
        stripe_table_[larger_stripe.stripe_id] = larger_stripe;
        stripe_table_[larger_stripe.stripe_id].ec = clone_ec(ec_schema_.ec_type, temp_ec);
        s_merge_group.push_back(larger_stripe.stripe_id);
        stripe_cnt += step_size;
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec +
            (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000.0;
        gettimeofday(&end_time, NULL);
        double temp_time = end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
        merging_time += temp_time;
        std::cout << "[Merging] Process " << stripe_cnt << "/" << tot_stripe_num
                  << " total_time:" << merging_time << " latest:" << temp_time
                  << std::endl;
      }
      new_merge_groups.push_back(s_merge_group);
    }
    merge_groups_.clear();
    merge_groups_ = new_merge_groups;
    ec_schema_.set_ec(temp_ec);
    ec_schema_.x /= step_size;
    merged_flag_ = true;
    // response
    response.computing_time = computing_time;
    response.cross_cluster_time = cross_cluster_time;
    response.merging_time = merging_time;
    response.meta_time = meta_time;
    response.cross_cluster_transfers = t_cross_cluster;
    response.io_cnt = t_cross_cluster;
    response.io_cnt = io_cnt;

    if (IF_DEBUG) {
      print_placement_result("After Merging:");
    }
  }

  void Coordinator::pc_merge(MergeResp& response, int step_size)
  {
    struct timeval start_time, end_time;
    struct timeval m_start_time, m_end_time;
    int x = ec_schema_.x;
    int tot_stripe_num = int(stripe_table_.size());
    int stripe_cnt = 0;
    std::vector<std::vector<unsigned int>> new_merge_groups;
    bool isvertical = false;
    if (ec_schema_.multistripe_placement_rule == VERTICAL) {
      isvertical = true;
    }
    // for simulation
    int t_cross_cluster = 0;
    int io_cnt = 0;
    double merging_time = 0;
    double computing_time = 0;
    double meta_time = 0;
    double cross_cluster_time = 0;

    ErasureCode *temp_ec = new_ec_for_merge(step_size);
    CodingParameters new_cp;
    temp_ec->get_coding_parameters(new_cp);
    CodingParameters old_cp;
    ec_schema_.ec->get_coding_parameters(old_cp);

    // for each merge group, every x stripes a merge group
    for (auto& merge_group : merge_groups_) {
      int group_len = (int)merge_group.size();
      my_assert(group_len % step_size == 0);
      std::vector<unsigned int> s_merge_group;
      for (int mi = 0; mi < group_len; mi += step_size) {
        gettimeofday(&start_time, NULL);
        gettimeofday(&m_start_time, NULL);
        std::unordered_set<int> old_stripe_id_set;
        std::unordered_map<int, MainRecalPlan> main_plans;
        std::unordered_map<int, std::vector<unsigned int>> parity_nodes;
        if (isvertical) {
          for (int i = 0; i < old_cp.k1 + old_cp.m1; i++) {
            MainRecalPlan temp;
            main_plans[i] = temp;
            std::vector<unsigned int> vec(old_cp.m2);
            parity_nodes[i] = vec;
          }
        } else {
          for (int i = 0; i < old_cp.k2 + old_cp.m2; i++) {
            MainRecalPlan temp;
            main_plans[i] = temp;
            std::vector<unsigned int> vec(old_cp.m1);
            parity_nodes[i] = vec;
          }
        }
        
        DeletePlan old_parities;
        std::unordered_set<int> old_parities_cluster_set;
        // merge and generate new stripe information
        size_t block_size = ec_schema_.block_size;
        Stripe larger_stripe;
        larger_stripe.stripe_id = cur_stripe_id_++;
        larger_stripe.block_size = block_size;
        for (int i = 0; i < new_cp.k + new_cp.m; i++) {
          larger_stripe.blocks2nodes.push_back(0);
          larger_stripe.block_ids.push_back(0);
        }
        int seri_num = 0;
        for (int ni = mi; ni < mi + step_size; ni++) {
          unsigned int t_stripe_id = merge_group[ni];
          old_stripe_id_set.insert(t_stripe_id);
          Stripe &t_stripe = stripe_table_[t_stripe_id];
          int k1 = old_cp.k1;
          int m1 = old_cp.m1;
          int k2 = old_cp.k2;
          int m2 = old_cp.m2;
          ProductCode t_pc;
          t_pc.init_coding_parameters(old_cp);
          int row = 0;
          int col = 0;

          larger_stripe.objects.insert(larger_stripe.objects.end(),
                                      t_stripe.objects.begin(),
                                      t_stripe.objects.end());
          for (int i = 0; i < (k1 + m1) * (k2 + m2); i++) {
            unsigned int t_node_id = t_stripe.blocks2nodes[i];
            unsigned int t_cluster_id = node_table_[t_node_id].map2cluster;
            t_pc.bid2rowcol(i, row, col);
            if (i < k1 * k2) {  // data blocks
              int newbid = t_pc.oldbid2newbid_for_merge(
                  i, step_size, seri_num, isvertical);
              larger_stripe.blocks2nodes[newbid] = t_node_id;
              larger_stripe.block_ids[newbid] = t_stripe.block_ids[i];
              cluster_table_[t_cluster_id].holding_stripe_ids.insert(
                  larger_stripe.stripe_id);
              int new_idx = seri_num * k1 + col;
              int plan_idx = row;
              if (isvertical) {
                new_idx = seri_num * k2 + row;
                plan_idx = col;
              }
              bool is_in = false;
              for (auto& t_location : main_plans[plan_idx].help_clusters_info) {
                if (t_location.cluster_id == t_cluster_id) {
                  Node& t_node = node_table_[t_node_id];
                  t_location.blocks_info.push_back(
                      std::make_pair(new_idx,
                      std::make_pair(t_node.node_ip, t_node.node_port)));
                  t_location.block_ids.push_back(t_stripe.block_ids[i]);
                  is_in = true;
                  break;
                }
              }
              if (!is_in) {
                LocationInfo new_location;
                new_location.cluster_id = t_cluster_id;
                new_location.proxy_port = cluster_table_[t_cluster_id].proxy_port;
                new_location.proxy_ip = cluster_table_[t_cluster_id].proxy_ip;
                Node& t_node = node_table_[t_node_id];
                new_location.blocks_info.push_back(
                    std::make_pair(new_idx,
                    std::make_pair(t_node.node_ip, t_node.node_port)));
                new_location.block_ids.push_back(t_stripe.block_ids[i]);
                main_plans[plan_idx].help_clusters_info.push_back(new_location);
              }
            } else if (i >= k1 * k2 && i < (k1 + m1) * k2) {
              if (isvertical) {
                int newbid = t_pc.oldbid2newbid_for_merge(
                  i, step_size, seri_num, isvertical);
                larger_stripe.blocks2nodes[newbid] = t_node_id;
                larger_stripe.block_ids[newbid] = t_stripe.block_ids[i];
                cluster_table_[t_cluster_id].holding_stripe_ids.insert(
                    larger_stripe.stripe_id);
                bool is_in = false;
                for (auto& t_location : main_plans[col].help_clusters_info) {
                  if (t_location.cluster_id == t_cluster_id) {
                    Node &t_node = node_table_[t_node_id];
                    int new_idx = seri_num * k2 + row;
                    t_location.blocks_info.push_back(
                        std::make_pair(new_idx, std::make_pair(t_node.node_ip, t_node.node_port)));
                    t_location.block_ids.push_back(t_stripe.block_ids[i]);
                    is_in = true;
                    break;
                  }
                }
                if (!is_in) {
                  LocationInfo new_location;
                  new_location.cluster_id = t_cluster_id;
                  new_location.proxy_port = cluster_table_[t_cluster_id].proxy_port;
                  new_location.proxy_ip = cluster_table_[t_cluster_id].proxy_ip;
                  Node &t_node = node_table_[t_node_id];
                  int new_idx = seri_num * k2 + row;
                  new_location.blocks_info.push_back(
                      std::make_pair(new_idx,
                      std::make_pair(t_node.node_ip, t_node.node_port)));
                  new_location.block_ids.push_back(t_stripe.block_ids[i]);
                  main_plans[col].help_clusters_info.push_back(new_location);
                }
              } else {
                Node& t_node = node_table_[t_node_id];
                // for new locations
                auto& nodes = parity_nodes[row];
                nodes[col - k1] = t_node_id;
                // for delete
                old_parities.blocks_info.push_back(
                    std::make_pair(t_node.node_ip, t_node.node_port));
                old_parities.block_ids.push_back(t_stripe.block_ids[i]);
                old_parities_cluster_set.insert(t_cluster_id);
              }
            } else if (i >= (k1 + m1) * k2 && i < (k1 + m1) * k2 + k1 * m2) {
              if (isvertical) {
                Node& t_node = node_table_[t_node_id];
                // for new locations
                auto& nodes = parity_nodes[col];
                nodes[row - k2] = t_node_id;
                // for delete
                old_parities.blocks_info.push_back(
                    std::make_pair(t_node.node_ip, t_node.node_port));
                old_parities.block_ids.push_back(t_stripe.block_ids[i]);
                old_parities_cluster_set.insert(t_cluster_id);
              } else {
                int newbid = t_pc.oldbid2newbid_for_merge(
                  i, step_size, seri_num, isvertical);
                larger_stripe.blocks2nodes[newbid] = t_node_id;
                larger_stripe.block_ids[newbid] = t_stripe.block_ids[i];
                cluster_table_[t_cluster_id].holding_stripe_ids.insert(
                    larger_stripe.stripe_id);
                bool is_in = false;
                for (auto& t_location : main_plans[row].help_clusters_info) {
                  if (t_location.cluster_id == t_cluster_id) {
                    Node &t_node = node_table_[t_node_id];
                    int new_idx = seri_num * k1 + col;
                    t_location.blocks_info.push_back(
                        std::make_pair(new_idx,
                        std::make_pair(t_node.node_ip, t_node.node_port)));
                    t_location.block_ids.push_back(t_stripe.block_ids[i]);
                    is_in = true;
                    break;
                  }
                }
                if (!is_in) {
                  LocationInfo new_location;
                  new_location.cluster_id = t_cluster_id;
                  new_location.proxy_port = cluster_table_[t_cluster_id].proxy_port;
                  new_location.proxy_ip = cluster_table_[t_cluster_id].proxy_ip;
                  Node &t_node = node_table_[t_node_id];
                  int new_idx = seri_num * k1 + col;
                  new_location.blocks_info.push_back(
                      std::make_pair(new_idx, std::make_pair(t_node.node_ip, t_node.node_port)));
                  new_location.block_ids.push_back(t_stripe.block_ids[i]);
                  main_plans[row].help_clusters_info.push_back(new_location);
                }
              }
            } else {  // parity blocks
              if (ec_schema_.ec_type != HV_PC) {
                Node& t_node = node_table_[t_node_id];
                // for new locations
                if (isvertical) {
                  auto& nodes = parity_nodes[col];
                  nodes[row - k2] = t_node_id;
                } else {
                  auto& nodes = parity_nodes[row];
                  nodes[col - k1] = t_node_id;
                }
                // for delete
                old_parities.blocks_info.push_back(
                    std::make_pair(t_node.node_ip, t_node.node_port));
                old_parities.block_ids.push_back(t_stripe.block_ids[i]);
                old_parities_cluster_set.insert(t_cluster_id);
              }
            }
          }
          seri_num++;
        }

        std::unordered_map<int, unsigned int> main_cluster_ids;
        // generate new pairty blocks
        if (isvertical) {
          int base = new_cp.k2 * (new_cp.k1 + new_cp.m1);
          for (auto& kv : parity_nodes) {
            int col = kv.first;
            auto nodes = kv.second;
            for (int ii = 0; ii < new_cp.m2; ii++) {
              unsigned int t_node_id = nodes[ii];
              auto& t_node = node_table_[t_node_id];
              int newbid = base + ii * new_cp.k1 + col;
              larger_stripe.blocks2nodes[newbid] = t_node_id;
              larger_stripe.block_ids[newbid] = cur_block_id_++;
              main_cluster_ids[col] = t_node.map2cluster;
              main_plans[col].new_locations.push_back(
                std::make_pair(new_cp.k2 + ii,
                std::make_pair(t_node.node_ip, t_node.node_port)));
              main_plans[col].new_parity_block_ids.push_back(cur_block_id_ - 1);
            }
          }
        } else {
          for (auto& kv : parity_nodes) {
            int row = kv.first;
            int base = new_cp.k2 * new_cp.k1;
            if (row >= new_cp.k2) {
              base = new_cp.k2 * (new_cp.k1 + new_cp.m1) + new_cp.m2 * new_cp.k1;
            }
            auto nodes = kv.second;
            for (int ii = 0; ii < new_cp.m1; ii++) {
              unsigned int t_node_id = nodes[ii];
              auto& t_node = node_table_[t_node_id];
              int newbid = base + row * new_cp.m1 + ii;
              if (row >= new_cp.k2) {
                newbid = base + (row - new_cp.k2) * new_cp.m1 + ii;
              }
              larger_stripe.blocks2nodes[newbid] = t_node_id;
              larger_stripe.block_ids[newbid] = cur_block_id_++;
              main_cluster_ids[row] = t_node.map2cluster;
              main_plans[row].new_locations.push_back(
                std::make_pair(new_cp.k1 + ii,
                std::make_pair(t_node.node_ip, t_node.node_port)));
              main_plans[row].new_parity_block_ids.push_back(cur_block_id_ - 1);
            }
          }
        }

        for (auto& kv : main_cluster_ids) {
          unsigned int cluster_id = kv.second;
          cluster_table_[cluster_id].holding_stripe_ids.insert(
              larger_stripe.stripe_id);
        }

        if (IF_DEBUG) {
          std::cout << "Finish generate new stripe information for stripe"
                    << larger_stripe.stripe_id << std::endl;
        }

        // find out block to move
        std::vector<unsigned int> blocks_to_move;
        std::vector<unsigned int> src_nodes;
        std::vector<unsigned int> des_nodes;
        std::unordered_map<int, int> cluster_block_cnt;
        std::unordered_map<unsigned int, std::vector<int>> cluster_blocks;
        std::vector<unsigned int> free_clusters;
        for (unsigned int i = 0; i < num_of_clusters_; i++) {
          free_clusters.push_back(i);
        }

        int block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int cluster_id = node_table_[node_id].map2cluster;
          if (cluster_block_cnt.find(cluster_id) == cluster_block_cnt.end()) {
            cluster_block_cnt[cluster_id] = 1;
            cluster_blocks[cluster_id] = std::vector({block_idx});
          } else {
            cluster_block_cnt[cluster_id] += 1;
            cluster_blocks[cluster_id].push_back(block_idx);
          }
          block_idx++;
          auto it = std::find(free_clusters.begin(), free_clusters.end(),
                              (unsigned)cluster_id);
          if (it != free_clusters.end()) {
            free_clusters.erase(it);
          }
        }

        std::vector<std::pair<int, int>> 
            sorted_clusters(cluster_block_cnt.begin(), cluster_block_cnt.end());
        std::sort(sorted_clusters.begin(), sorted_clusters.end(), cmp_descending);
        int cluster_num = (int)sorted_clusters.size();
        size_t free_cluster_num = free_clusters.size();

        for (int ii = 0; ii < cluster_num; ii++) {
          std::unordered_map<int, std::vector<int>> col_blocks;
          unsigned int cluster_id = sorted_clusters[ii].first;
          bool flag = if_subject_to_fault_tolerance_pc(temp_ec,
              cluster_blocks[cluster_id], col_blocks);
          if (!flag) {
            int col_num = (int)col_blocks.size();
            std::vector<std::pair<int, int>> sorted_cols;
            for (int j = 0; j < col_num; j++) {
              sorted_cols.push_back(std::make_pair(j, (int)col_blocks[j].size()));
            }
            std::sort(sorted_cols.begin(), sorted_cols.end(), cmp_descending);
            int t_m1 = new_cp.m1;
            for (int j = col_num - 1; j >= t_m1; j -= t_m1) {
              int rand_idx = random_index(free_cluster_num);
              unsigned int des_cid = free_clusters[rand_idx];
              for (int jj = j; jj >= t_m1 && jj > j - t_m1; jj--) {
                int c_idx = sorted_cols[jj].first;
                auto& src_blocks = col_blocks[c_idx];
                int block_num = (int)src_blocks.size();
                std::vector<unsigned int> free_nodes;
                for (int j = 0; j < num_of_nodes_per_cluster_; j++) {
                  free_nodes.push_back(des_cid * num_of_nodes_per_cluster_ + j);
                }
                size_t free_node_num = free_nodes.size();
                for (int jjj = 0; jjj < block_num; jjj++) {
                  int block_idx = src_blocks[jjj];
                  blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
                  unsigned int src_nid = larger_stripe.blocks2nodes[block_idx];
                  src_nodes.push_back(src_nid);
                  // select a destination node
                  int rand_idx = random_index(free_node_num);
                  unsigned int des_nid = free_nodes[rand_idx];
                  larger_stripe.blocks2nodes[block_idx] = des_nid;
                  des_nodes.push_back(des_nid);
                  auto it = std::find(free_nodes.begin(), free_nodes.end(), des_nid);
                  if (it != free_nodes.end()) {
                    free_nodes.erase(it);
                    free_node_num--;
                  }
                }
                sorted_clusters[ii].second -= block_num;
              }
              auto it = std::find(free_clusters.begin(), free_clusters.end(), des_cid);
              if (it != free_clusters.end()) {
                free_clusters.erase(it);
                free_cluster_num--;
              }
            }
          }
        }

        std::vector<int> node_map(num_of_clusters_ * num_of_nodes_per_cluster_, 0);
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          node_map[idx]++;
        }
        block_idx = 0;
        for (auto& node_id : larger_stripe.blocks2nodes) {
          int idx = (int)node_id;
          if (node_map[idx] > 1) {
            int cid = idx / num_of_nodes_per_cluster_;
            int rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            while (node_map[rand_idx] != 0) {
              rand_idx = random_range(cid * num_of_nodes_per_cluster_, 
                (cid + 1) * num_of_nodes_per_cluster_);
            } 
            blocks_to_move.push_back(larger_stripe.block_ids[block_idx]);
            src_nodes.push_back(node_id);
            des_nodes.push_back((unsigned int)rand_idx);
            larger_stripe.blocks2nodes[block_idx] = (unsigned int)rand_idx;
            node_map[rand_idx]++;
            node_map[idx]--;
          }
          block_idx++;
        }

        if (IF_DEBUG) {
          std::cout << "Find out block to relocate!" << std::endl;
        }

        RelocatePlan reloc_plan;
        reloc_plan.block_size = block_size;
        int move_num = (int)blocks_to_move.size();
        if (move_num > 0) {
          for (int i = 0; i < move_num; i++) {
            reloc_plan.blocks_to_move.push_back(blocks_to_move[i]);
            auto& src_node = node_table_[src_nodes[i]];
            reloc_plan.src_nodes.push_back(std::make_pair(src_node.map2cluster,
                std::make_pair(src_node.node_ip, src_node.node_port)));
            auto& des_node = node_table_[des_nodes[i]];
            reloc_plan.des_nodes.push_back(std::make_pair(des_node.map2cluster,
                std::make_pair(des_node.node_ip, des_node.node_port)));
            if (src_node.map2cluster != des_node.map2cluster) {
              t_cross_cluster++;
            }
          }
          if (IF_DEBUG) {
            std::cout << "[MERGE] all blocks to relocate:";
            for (int ii = 0; ii < move_num; ii++) {
                std::cout << blocks_to_move[ii] << "[" << src_nodes[ii] 
                          << "->" << des_nodes[ii] << "] ";
            }
            std::cout << std::endl;
          }
        }
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec +
            (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000;

        // parity block recalculation
        // generate main plan
        for (auto& kv : main_plans) {
          int m_ = 0;
          int idx = kv.first;
          if (ec_schema_.ec_type == HV_PC) {
            if (isvertical) {
              if (idx >= new_cp.k1) {
                continue;
              }
            } else {
              if (idx >= new_cp.k2) {
                continue;
              }
            }
          }
          auto& main_plan = kv.second;
          unsigned int parity_cluster_id = main_cluster_ids[idx];
          main_plan.ec_type = RS;
          if (isvertical) {
            main_plan.cp.k = new_cp.k2;
            main_plan.cp.m = new_cp.m2;
            m_ = new_cp.m2;
          } else {
            main_plan.cp.k = new_cp.k1;
            main_plan.cp.m = new_cp.m1;
            m_ = new_cp.m1;
          }
          main_plan.cluster_id = parity_cluster_id;
          main_plan.cp.x = step_size;
          main_plan.block_size = block_size;
          main_plan.partial_decoding = ec_schema_.partial_decoding;
          main_plan.cp.local_or_column = false;
          int ci = 0;
          for (auto& location : main_plan.help_clusters_info) {
            if (location.cluster_id == parity_cluster_id) {
              main_plan.inner_cluster_help_blocks_info.insert(
                  main_plan.inner_cluster_help_blocks_info.end(),
                  location.blocks_info.begin(),  location.blocks_info.end());
              main_plan.inner_cluster_help_block_ids.insert(
                  main_plan.inner_cluster_help_block_ids.end(),
                  location.block_ids.begin(),  location.block_ids.end());
              main_plan.help_clusters_info.erase(main_plan.help_clusters_info.begin() + ci);
              break;
            }
            ci++;
          }
          
          // simulate recalculation
          simulation_recalculation(main_plan, t_cross_cluster, io_cnt);

          auto lock_ptr = std::make_shared<std::mutex>();
          auto send_main_plan = [this, main_plan, parity_cluster_id,
                                lock_ptr, &computing_time,
                                &cross_cluster_time]() mutable
          {
            std::string chosen_proxy = proxy_endpoint_key(
                cluster_table_[parity_cluster_id].proxy_ip,
                cluster_table_[parity_cluster_id].proxy_port);
            auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                            ->call_for<&Proxy::main_recal>(
                                std::chrono::seconds{0}, main_plan)).value();
            lock_ptr->lock();
            computing_time += resp.computing_time;
            cross_cluster_time += resp.cross_cluster_time;
            lock_ptr->unlock();
            if (IF_DEBUG) {
              std::cout << "Selected main proxy " << chosen_proxy << std::endl;
            }
          };

          auto send_help_plan = [this, block_size, main_plan,
                                parity_cluster_id](int help_idx)
          {
            HelpRecalPlan help_plan;
            help_plan.ec_type = ec_schema_.ec_type;
            help_plan.cp = main_plan.cp;
            help_plan.block_size = block_size;
            help_plan.partial_decoding = ec_schema_.partial_decoding;
            help_plan.cp.local_or_column = false;
            help_plan.main_proxy_port = cluster_table_[parity_cluster_id].proxy_port + SOCKET_PORT_OFFSET;
            help_plan.main_proxy_ip = cluster_table_[parity_cluster_id].proxy_ip;
            auto& location = main_plan.help_clusters_info[help_idx];
            unsigned int cluster_id = location.cluster_id;
            help_plan.inner_cluster_help_blocks_info = location.blocks_info;
            help_plan.inner_cluster_help_block_ids = location.block_ids;
            for (int i = 0; i < int(main_plan.new_locations.size()); i++) {
              help_plan.new_parity_block_idxs.push_back(main_plan.new_locations[i].first);
            }
            std::string chosen_proxy = proxy_endpoint_key(
                cluster_table_[cluster_id].proxy_ip,
                cluster_table_[cluster_id].proxy_port);
            async_simple::coro::syncAwait(help_proxies_[chosen_proxy]
                ->call_for<&Proxy::help_recal>(std::chrono::seconds{0}, help_plan));
            if (IF_DEBUG) {
              std::cout << "Selected help proxy " << chosen_proxy << std::endl;
            }
          };

          if (!IF_SIMULATION) {
            // recalculation
            try {
              if (IF_DEBUG) {
                std::cout << "[Parities Recalculation] Send main and help proxy plans!\n";
              }
              
              std::thread my_main_thread(send_main_plan);
              std::vector<std::thread> senders;
              int i = 0;
              for (auto& location : main_plan.help_clusters_info) {
                if ((IF_DIRECT_FROM_NODE && m_ < (int)location.block_ids.size()
                    && ec_schema_.partial_decoding) || !IF_DIRECT_FROM_NODE) {
                  senders.push_back(std::thread(send_help_plan, i));
                }
                i++;
              }
              for (int j = 0; j < int(senders.size()); j++) {
                senders[j].join();
              }
              my_main_thread.join();
            }
            catch (const std::exception &e)
            {
              std::cerr << e.what() << '\n';
            }
          }
        }

        if (!IF_SIMULATION) {
          // blocks relocation
          if (move_num > 0) {
            unsigned int ran_cluster_id = (unsigned int)random_index(num_of_clusters_);
            std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[ran_cluster_id].proxy_ip,
              cluster_table_[ran_cluster_id].proxy_port);
            auto resp = async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                          ->call_for<&Proxy::block_relocation>(
                              std::chrono::seconds{0}, reloc_plan)).value();
            cross_cluster_time += resp.cross_cluster_time;
            io_cnt += 2 * (int)reloc_plan.blocks_to_move.size();
          }

          // delete old parities
          int idx = random_index(old_parities_cluster_set.size());
          int del_cluster_id = *(std::next(old_parities_cluster_set.begin(), idx));
          std::string chosen_proxy = proxy_endpoint_key(
              cluster_table_[del_cluster_id].proxy_ip,
              cluster_table_[del_cluster_id].proxy_port);
          async_simple::coro::syncAwait(main_proxies_[chosen_proxy]
                ->call_for<&Proxy::delete_blocks>(std::chrono::seconds{0}, old_parities));
        }
        
        gettimeofday(&m_start_time, NULL);
        // update metadata
        for (auto it = old_stripe_id_set.begin();
            it != old_stripe_id_set.end(); it++) {
          for (auto& object : stripe_table_[*it].objects) {
            auto& vec = commited_object_table_[object].stripes;
            vec.erase(std::remove(vec.begin(), vec.end(), *it), vec.end());
            vec.push_back(larger_stripe.stripe_id);
          }
          stripe_table_.erase(*it);
          for (auto& kv : cluster_table_) {
            kv.second.holding_stripe_ids.erase(*it);
          }
        }
        stripe_table_[larger_stripe.stripe_id] = larger_stripe;
        stripe_table_[larger_stripe.stripe_id].ec = clone_ec(ec_schema_.ec_type, temp_ec);
        s_merge_group.push_back(larger_stripe.stripe_id);
        stripe_cnt += step_size;
        gettimeofday(&m_end_time, NULL);
        meta_time += m_end_time.tv_sec - m_start_time.tv_sec +
            (m_end_time.tv_usec - m_start_time.tv_usec) / 1000000;
        gettimeofday(&end_time, NULL);
        double temp_time = end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
        merging_time += temp_time;
        std::cout << "[Merging] Process " << stripe_cnt << "/" << tot_stripe_num
                  << " total_time:" << merging_time << " latest:" << temp_time
                  << std::endl;
      }
      new_merge_groups.push_back(s_merge_group);
    }
    merge_groups_.clear();
    merge_groups_ = new_merge_groups;
    ec_schema_.set_ec(temp_ec);
    ec_schema_.x /= step_size;
    merged_flag_ = true;
    // response
    response.computing_time = computing_time;
    response.cross_cluster_time = cross_cluster_time;
    response.merging_time = merging_time;
    response.meta_time = meta_time;
    response.cross_cluster_transfers = t_cross_cluster;
    response.io_cnt = io_cnt;

    if (IF_DEBUG) {
      print_placement_result("After Merging:");
    }
  }

  void Coordinator::simulation_recalculation(MainRecalPlan& main_plan,
            int& cross_cluster_transfers, int& io_cnt)
  {
    int parity_num = (int)main_plan.new_parity_block_ids.size();
    for (auto& location : main_plan.help_clusters_info) {
      int help_block_num = (int)location.block_ids.size();
      if (parity_num < help_block_num && ec_schema_.partial_decoding) {
        cross_cluster_transfers += parity_num;
      } else {
        cross_cluster_transfers += help_block_num;
      }
      io_cnt += help_block_num;
    }
    io_cnt += parity_num;
    io_cnt += (int)main_plan.inner_cluster_help_block_ids.size();
  }
} // namespace ECProject
