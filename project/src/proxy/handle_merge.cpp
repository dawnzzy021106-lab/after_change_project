#include "proxy.h"

namespace ECProject
{
  MainRecalResp Proxy::main_recal(MainRecalPlan recal_plan)
  {
    struct timeval start_time, end_time;
    double computing_time = 0;
    double cross_cluster_time = 0;

    bool if_partial_decoding = recal_plan.partial_decoding;
    size_t block_size = recal_plan.block_size;
    auto ec = ec_factory(recal_plan.ec_type, recal_plan.cp);
    ec->init_coding_parameters(recal_plan.cp);

    int parity_num = recal_plan.new_parity_block_ids.size();
    auto parity_idx_ptr = std::make_shared<std::vector<int>>();
    for (int i = 0; i < parity_num; i++) {
      parity_idx_ptr->push_back(recal_plan.new_locations[i].first);
    }

    auto original_lock_ptr = std::make_shared<std::mutex>();
    auto original_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto original_blocks_idx_ptr = std::make_shared<std::vector<int>>();

    auto get_from_node = [this, original_lock_ptr, original_blocks_ptr,
                          original_blocks_idx_ptr, block_size](
                          int block_idx, unsigned int block_id,
                          std::string node_ip, int node_port) mutable
    {
      std::string block_id_str = std::to_string(block_id);
      std::vector<char> tmp_val(block_size);
      bool res = read_from_datanode(block_id_str.c_str(), block_id_str.size(), 
              tmp_val.data(), block_size, node_ip.c_str(), node_port);
      if (!res) {
        pthread_exit(NULL);
      }
      original_lock_ptr->lock();
      original_blocks_ptr->push_back(tmp_val);
      original_blocks_idx_ptr->push_back(block_idx);
      original_lock_ptr->unlock();
    };

    auto partial_lock_ptr = std::make_shared<std::mutex>();
    auto partial_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto partial_blocks_idx_ptr = std::make_shared<std::vector<int>>();
    auto computing_time_ptr = std::make_shared<std::vector<double>>();

    auto get_from_proxy = [this, original_lock_ptr, original_blocks_ptr,
                           original_blocks_idx_ptr, partial_lock_ptr,
                           partial_blocks_ptr, partial_blocks_idx_ptr,
                           block_size, computing_time_ptr, parity_num]
                           (std::shared_ptr<asio::ip::tcp::socket> socket_ptr) mutable
    {
      asio::error_code error;
      std::vector<unsigned char> int_buf(sizeof(int));
      asio::read(*socket_ptr, asio::buffer(int_buf, int_buf.size()), error);
      int t_cluster_id = ECProject::bytes_to_int(int_buf);
      std::vector<unsigned char> int_flag_buf(sizeof(int));
      asio::read(*socket_ptr, asio::buffer(int_flag_buf, int_flag_buf.size()), error);
      int t_flag = ECProject::bytes_to_int(int_flag_buf);
      std::string msg = "data";
      if(t_flag)
        msg = "partial";
      if (IF_DEBUG) {
        std::cout << "[Main Proxy " << self_cluster_id_ << "] Try to get "
                  << msg << " blocks from the proxy in cluster " << t_cluster_id
                  << ". " << std::endl;
      }
      if (t_flag) {  // receive partial blocks from helper proxies
        std::vector<unsigned char> int_buf_num_of_blocks(sizeof(int));
        asio::read(*socket_ptr, asio::buffer(int_buf_num_of_blocks,
            int_buf_num_of_blocks.size()), error);
        int partial_block_num = ECProject::bytes_to_int(int_buf_num_of_blocks);
        if (partial_block_num != parity_num) {
          std::cout << "Error! partial_blocks_num != parity_num" << std::endl;
        }
        partial_lock_ptr->lock();
        for (int i = 0; i < parity_num; i++) {
          std::vector<unsigned char> block_idx_buf(sizeof(int));
          asio::read(*socket_ptr, asio::buffer(block_idx_buf,
              block_idx_buf.size()), error);
          int block_idx = ECProject::bytes_to_int(block_idx_buf);
          partial_blocks_idx_ptr->push_back(block_idx);
          std::vector<char> tmp_val(block_size);
          asio::read(*socket_ptr, asio::buffer(tmp_val.data(), block_size), error);
          partial_blocks_ptr->push_back(tmp_val);
        }
        partial_lock_ptr->unlock();
      } else {  // receive data blocks from help proxies
        std::vector<unsigned char> int_buf_num_of_blocks(sizeof(int));
        asio::read(*socket_ptr, asio::buffer(int_buf_num_of_blocks,
            int_buf_num_of_blocks.size()), error);
        int block_num = ECProject::bytes_to_int(int_buf_num_of_blocks);
        for (int i = 0; i < block_num; i++) {
          std::vector<char> tmp_val(block_size);
          std::vector<unsigned char> block_idx_buf(sizeof(int));
          asio::read(*socket_ptr, asio::buffer(block_idx_buf,
              block_idx_buf.size()), error);
          int block_idx = ECProject::bytes_to_int(block_idx_buf);
          asio::read(*socket_ptr, asio::buffer(tmp_val.data(), block_size), error);
          original_lock_ptr->lock();
          original_blocks_ptr->push_back(tmp_val);
          original_blocks_idx_ptr->push_back(block_idx);
          original_lock_ptr->unlock();
        }
        std::vector<unsigned char> computing_time_buf(sizeof(double));
        asio::read(*socket_ptr, asio::buffer(computing_time_buf,
                                        computing_time_buf.size()));
        double temp_computing_time = bytes_to_double(computing_time_buf);
        computing_time_ptr->push_back(temp_computing_time);
      }

      if (IF_DEBUG){
        std::cout << "[Main Proxy " << self_cluster_id_
                  << "] Finish getting data from the proxy in cluster "
                  << t_cluster_id << std::endl;
      }
    };

    auto send_to_datanode = [this, block_size](
        unsigned int block_id, char *data, std::string node_ip, int node_port)
    {
      std::string block_id_str = std::to_string(block_id);
      write_to_datanode(block_id_str.c_str(), block_id_str.size(),
                        data, block_size, node_ip.c_str(), node_port);
    };

    // get original blocks inside cluster
    int num_of_original_blocks =
        (int)recal_plan.inner_cluster_help_blocks_info.size();
    if (num_of_original_blocks > 0) {
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers.push_back(std::thread(get_from_node,
            recal_plan.inner_cluster_help_blocks_info[i].first,
            recal_plan.inner_cluster_help_block_ids[i],
            recal_plan.inner_cluster_help_blocks_info[i].second.first,
            recal_plan.inner_cluster_help_blocks_info[i].second.second));
      }
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers[i].join();
      }
    }
    if (IF_DEBUG) {
      std::cout << "[Main Proxy " << self_cluster_id_
                << "] Finish getting " << num_of_original_blocks
                << " blocks inside main cluster." << std::endl;
    }

    gettimeofday(&start_time, NULL);
    // get blocks or partial blocks from other clusters
    int num_of_help_clusters = (int)recal_plan.help_clusters_info.size();
    int partial_cnt = 0;  // num of partial-block sets
    if (num_of_help_clusters > 0) {
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_help_clusters; i++) {
        int num_of_blocks_in_cluster =
            (int)recal_plan.help_clusters_info[i].blocks_info.size();
        bool t_flag = true;
        if (num_of_blocks_in_cluster <= parity_num) {
          t_flag = false;
        }
        t_flag = (if_partial_decoding && t_flag);
        if(!t_flag && IF_DIRECT_FROM_NODE) {  // transfer blocks directly
          num_of_original_blocks += num_of_blocks_in_cluster;
          for (int j = 0; j < num_of_blocks_in_cluster; j++) {
            readers.push_back(std::thread(get_from_node,
                recal_plan.help_clusters_info[i].blocks_info[j].first, 
                recal_plan.help_clusters_info[i].block_ids[j],
                recal_plan.help_clusters_info[i].blocks_info[j].second.first,
                recal_plan.help_clusters_info[i].blocks_info[j].second.second));
          }
        } else {  // transfer blocks through proxies
          if(t_flag) { // encode partial blocks and transfer
            partial_cnt++;
          } else {   // direct transfer original blocks
            num_of_original_blocks += num_of_blocks_in_cluster;
          }
          std::shared_ptr<asio::ip::tcp::socket>
              socket_ptr = std::make_shared<asio::ip::tcp::socket>(io_context_);
          acceptor_.accept(*socket_ptr);
          readers.push_back(std::thread(get_from_proxy, socket_ptr));
        }
      }
      int num_of_readers = (int)readers.size();
      for (int i = 0; i < num_of_readers; i++) {
        readers[i].join();
      }

      // simulate cross-cluster transfer
      if (IF_SIMULATE_CROSS_CLUSTER) {
        for (int i = 0; i < num_of_help_clusters; i++) {
          int num_of_blocks = (int)recal_plan.help_clusters_info[i].block_ids.size();
          bool t_flag = true;
          if (num_of_blocks <= parity_num) {
            t_flag = false;
          }
          t_flag = (if_partial_decoding && t_flag);
          if (t_flag) {
            num_of_blocks = parity_num;
          }
          size_t t_val_len = (int)block_size * num_of_blocks;
          std::string t_value = generate_random_string((int)t_val_len);
          transfer_to_networkcore(t_value.c_str(), t_val_len);
        }
      }

      if (computing_time_ptr->size() > 0) {
        auto max_decode = std::max_element(computing_time_ptr->begin(),
            computing_time_ptr->end());
        computing_time += *max_decode;
      }
    }
    gettimeofday(&end_time, NULL);
    cross_cluster_time += end_time.tv_sec - start_time.tv_sec +
        (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;

    if (IF_DEBUG) {
      std::cout << "[Main Proxy " << self_cluster_id_
                << "] Finish getting blocks from other "
                << num_of_help_clusters << " clusters." << std::endl;
    }
    
    my_assert(num_of_original_blocks == (int)original_blocks_ptr->size());
    if (num_of_original_blocks > 0 && if_partial_decoding) {  // encode-and-transfer
      std::vector<char *> v_data(num_of_original_blocks);
      std::vector<char *> v_coding(parity_num);
      char **data = (char **)v_data.data();
      char **coding = (char **)v_coding.data();
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      std::vector<std::vector<char>>
          v_coding_area(parity_num, std::vector<char>(block_size));
      for (int j = 0; j < parity_num; j++) {
        coding[j] = v_coding_area[j].data();
      }

      auto partial_flags = std::vector<bool>(parity_num, true);  
      std::vector<int> survivors;    
      auto nothing_ptr = std::make_shared<std::vector<int>>();
      gettimeofday(&start_time, NULL);
      ec->encode_partial_blocks(data, coding, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr, *nothing_ptr,
          *nothing_ptr, partial_flags, false);
      gettimeofday(&end_time, NULL);
      computing_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
            
      partial_lock_ptr->lock();
      for (int j = 0; j < parity_num; j++) {
        partial_blocks_idx_ptr->push_back(parity_idx_ptr->at(j));
        partial_blocks_ptr->push_back(v_coding_area[j]);
      }
      partial_lock_ptr->unlock();
      partial_cnt++;
    }

    if (IF_DEBUG) {
      std::cout << "[Main Proxy " << self_cluster_id_
                << "] Ready to decode! " << num_of_original_blocks << " vs "
                << partial_cnt<< std::endl;
    }
    int data_num = num_of_original_blocks;
    if (if_partial_decoding) {
      data_num = partial_cnt * parity_num;
    }
    std::vector<char *> vt_data(data_num);
    std::vector<char *> vt_coding(parity_num);
    char **t_data = (char **)vt_data.data();
    char **t_coding = (char **)vt_coding.data();
    std::vector<std::vector<char>> vt_coding_area(parity_num, std::vector<char>(block_size));
    for (int i = 0; i < parity_num; i++) {
      t_coding[i] = vt_coding_area[i].data();
    }

    if (if_partial_decoding) {
      for (int i = 0; i < data_num; i++) {
        t_data[i] = partial_blocks_ptr->at(i).data();
      }
    } else {
      for (int i = 0; i < data_num; i++) {
        t_data[i] = original_blocks_ptr->at(i).data();
      }
    }

    if (IF_DEBUG) {
      std::cout << "[Main Proxy" << self_cluster_id_
                << "] encoding!" << std::endl;
    }

    gettimeofday(&start_time, NULL);
    if (if_partial_decoding){
      ec->perform_addition(t_data, t_coding, block_size,
          *partial_blocks_idx_ptr, *parity_idx_ptr);
    } else {
      auto nothing_ptr = std::make_shared<std::vector<int>>();
      auto partial_flags = std::vector<bool>(parity_num, true);
      ec->encode_partial_blocks(t_data, t_coding, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr, *nothing_ptr,
          *nothing_ptr, partial_flags, false);
    }
    gettimeofday(&end_time, NULL);
    computing_time += end_time.tv_sec - start_time.tv_sec +
        (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      
    if (IF_DEBUG) {
      std::cout << "[Main Proxy" << self_cluster_id_
                << "] set new parity blocks!" << std::endl;
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < parity_num; i++) {
      writers.push_back(std::thread(send_to_datanode,
          recal_plan.new_parity_block_ids[i], t_coding[i],
          recal_plan.new_locations[i].second.first,
          recal_plan.new_locations[i].second.second));
    }
    for (int i = 0; i < parity_num; i++) {
      writers[i].join();
    }

    if (IF_DEBUG) {
      std::cout << "[Main Proxy" << self_cluster_id_ << "] finish recalculation! "
                << parity_num << " blocks! Computing time : "
                << computing_time << std::endl;
    }

    if (ec != nullptr) {
      delete ec;
      ec = nullptr;
    }

    MainRecalResp resp;
    resp.computing_time = computing_time;
    resp.cross_cluster_time = cross_cluster_time;
    return resp;
  }

  void Proxy::help_recal(HelpRecalPlan recal_plan)
  {
    struct timeval start_time, end_time;
    double computing_time = 0;

    bool if_partial_decoding = recal_plan.partial_decoding;
    bool t_flag = true;
    int num_of_original_blocks = (int)recal_plan.inner_cluster_help_blocks_info.size();
    int parity_num = (int)recal_plan.new_parity_block_idxs.size();
    if (num_of_original_blocks <= parity_num) {
      t_flag = false;
    }
    t_flag = (if_partial_decoding && t_flag);
    if (!t_flag && IF_DIRECT_FROM_NODE) {
      return;
    }

    size_t block_size = recal_plan.block_size;
    auto ec = ec_factory(recal_plan.ec_type, recal_plan.cp);
    ec->init_coding_parameters(recal_plan.cp);

    auto parity_idx_ptr = std::make_shared<std::vector<int>>(
        recal_plan.new_parity_block_idxs);

    auto original_lock_ptr = std::make_shared<std::mutex>();
    auto original_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto original_blocks_idx_ptr = std::make_shared<std::vector<int>>();

    auto get_from_node = [this, original_lock_ptr, original_blocks_ptr,
                          original_blocks_idx_ptr, block_size](
                          unsigned int block_id, int block_idx,
                          std::string node_ip, int node_port) mutable
    {
      std::string block_id_str = std::to_string(block_id);
      std::vector<char> tmp_val(block_size);
      bool res = read_from_datanode(block_id_str.c_str(), block_id_str.size(), 
              tmp_val.data(), block_size, node_ip.c_str(), node_port);
      if (!res) {
        pthread_exit(NULL);
      }
      original_lock_ptr->lock();
      original_blocks_ptr->push_back(tmp_val);
      original_blocks_idx_ptr->push_back(block_idx);
      original_lock_ptr->unlock();
    };
    if (IF_DEBUG) {
      std::cout << "[Helper Proxy" << self_cluster_id_
                << "] Ready to read blocks from data node!" << std::endl;
    }

    if (num_of_original_blocks > 0) {
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers.push_back(std::thread(get_from_node, 
                          recal_plan.inner_cluster_help_block_ids[i], 
                          recal_plan.inner_cluster_help_blocks_info[i].first,
                          recal_plan.inner_cluster_help_blocks_info[i].second.first,
                          recal_plan.inner_cluster_help_blocks_info[i].second.second));
      }
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers[i].join();
      }
    }

    my_assert(num_of_original_blocks == (int)original_blocks_ptr->size());

    int value_size = 0;

    if (t_flag) {
      if (IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] partial encoding with blocks " << std::endl;
        for (auto it = original_blocks_idx_ptr->begin();
             it != original_blocks_idx_ptr->end(); it++) {
          std::cout << (*it) << " ";
        }
        std::cout << std::endl;
      }
      // encode partial blocks
      std::vector<char *> v_data(num_of_original_blocks);
      std::vector<char *> v_coding(parity_num);
      char **data = (char **)v_data.data();
      char **coding = (char **)v_coding.data();
      std::vector<std::vector<char>> v_coding_area(parity_num, std::vector<char>(block_size));
      for (int j = 0; j < parity_num; j++) {
        coding[j] = v_coding_area[j].data();
      }
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      auto nothing_ptr = std::make_shared<std::vector<int>>();
      auto partial_flags = std::vector<bool>(parity_num, true);
      gettimeofday(&start_time, NULL);
      ec->encode_partial_blocks(data, coding, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr, *nothing_ptr,
          *nothing_ptr, partial_flags, false);
      gettimeofday(&end_time, NULL);
      computing_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;

      // send to main proxy
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      if (IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Try to connect main proxy port "
                  << recal_plan.main_proxy_port << std::endl;
      }
      asio::connect(socket_, resolver.resolve({recal_plan.main_proxy_ip,
          std::to_string(recal_plan.main_proxy_port)}), con_error);
      if (!con_error && IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Connect to " << recal_plan.main_proxy_ip
                  << ":" << recal_plan.main_proxy_port << " success!"
                  << std::endl;
      }
      std::vector<unsigned char>
          cid_buf = ECProject::int_to_bytes(self_cluster_id_);
      asio::write(socket_, asio::buffer(cid_buf, cid_buf.size()));
      std::vector<unsigned char> flag_buf = ECProject::int_to_bytes(1);
      asio::write(socket_, asio::buffer(flag_buf, flag_buf.size()));
      std::vector<unsigned char> num_buf = int_to_bytes(parity_num);
      asio::write(socket_, asio::buffer(num_buf, num_buf.size()));
      for (int j = 0; j < parity_num; j++) {
        if (partial_flags[j]) {
          std::vector<unsigned char>
              idx_buf = ECProject::int_to_bytes((*parity_idx_ptr)[j]);
          asio::write(socket_, asio::buffer(idx_buf, idx_buf.size()));
          asio::write(socket_, asio::buffer(coding[j], block_size));
          value_size += block_size;
        }
      }
      std::vector<unsigned char> computing_time_buf = double_to_bytes(computing_time);
      asio::write(socket_, asio::buffer(computing_time_buf, computing_time_buf.size()));
    } else if(!IF_DIRECT_FROM_NODE)  {
      // send to main proxy
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      if (IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Try to connect main proxy port "
                  << recal_plan.main_proxy_port << std::endl;
      }
      asio::connect(socket_, resolver.resolve({recal_plan.main_proxy_ip,
            std::to_string(recal_plan.main_proxy_port)}), con_error);
      if (!con_error && IF_DEBUG) {
            std::cout << "[Helper Proxy" << self_cluster_id_
            << "] Connect to " << recal_plan.main_proxy_ip<< ":"
            << recal_plan.main_proxy_port << " success!" << std::endl;
      }
      
      std::vector<unsigned char>
          int_buf_self_cluster_id = ECProject::int_to_bytes(self_cluster_id_);
      asio::write(socket_, asio::buffer(int_buf_self_cluster_id,
          int_buf_self_cluster_id.size()));
      std::vector<unsigned char> flag_buf = ECProject::int_to_bytes(1);
      asio::write(socket_, asio::buffer(flag_buf, flag_buf.size()));
      std::vector<unsigned char>
          int_buf_num_of_blocks = int_to_bytes((int)original_blocks_idx_ptr->size());
      asio::write(socket_,
          asio::buffer(int_buf_num_of_blocks, int_buf_num_of_blocks.size()));

      int j = 0;
      for(auto it = original_blocks_idx_ptr->begin();
              it != original_blocks_idx_ptr->end(); it++, j++) { 
        // send index and value
        
        int block_idx = *it;
        std::vector<unsigned char> byte_block_idx = ECProject::int_to_bytes(block_idx);
        asio::write(socket_, asio::buffer(byte_block_idx, byte_block_idx.size()));
        asio::write(socket_, asio::buffer((*original_blocks_ptr)[j], block_size));
        value_size += block_size;
      }
    }
        
    if (IF_DEBUG) {
       std::cout << "[Helper Proxy" << self_cluster_id_
       << "] Send value to proxy" <<  recal_plan.main_proxy_port 
       << "! With length of " << ". computing time : " << computing_time << std::endl;
    }
  }
}