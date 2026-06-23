#include "proxy.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace ECProject
{
  namespace
  {
    std::mutex help_cross_trace_log_mutex;
    constexpr size_t kHelpCrossTraceChunkBytes = 4 * 1024 * 1024;

    double trace_elapsed_sec(const std::chrono::steady_clock::time_point& start)
    {
      const auto dt = std::chrono::steady_clock::now() - start;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    }

    void write_help_cross_trace(const std::ostringstream& line)
    {
      std::lock_guard<std::mutex> lk(help_cross_trace_log_mutex);
      std::cout << line.str() << std::endl;
    }

    double read_help_cross_payload_chunks(asio::ip::tcp::socket& socket,
                                          char *data,
                                          size_t total_bytes,
                                          const std::string& task_id,
                                          unsigned int main_cluster_id,
                                          int helper_cluster_id,
                                          int block_idx)
    {
      double total_sec = 0.0;
      size_t offset = 0;
      int chunk_idx = 0;
      while (offset < total_bytes) {
        const size_t chunk_bytes =
            std::min(kHelpCrossTraceChunkBytes, total_bytes - offset);
        const auto chunk_start = std::chrono::steady_clock::now();
        asio::read(socket, asio::buffer(data + offset, chunk_bytes));
        const double chunk_sec = trace_elapsed_sec(chunk_start);
        total_sec += chunk_sec;

        std::ostringstream trace;
        trace << "[HCCHUNK] direction=recv"
              << " task_id=" << (task_id.empty() ? "-" : task_id)
              << " main_cluster=" << main_cluster_id
              << " helper_cluster=" << helper_cluster_id
              << " block_idx=" << block_idx
              << " chunk_idx=" << chunk_idx
              << " offset=" << offset
              << " bytes=" << chunk_bytes
              << " sec=" << chunk_sec;
        write_help_cross_trace(trace);

        offset += chunk_bytes;
        ++chunk_idx;
      }
      return total_sec;
    }

    double write_help_cross_payload_chunks(asio::ip::tcp::socket& socket,
                                           const char *data,
                                           size_t total_bytes,
                                           const std::string& task_id,
                                           unsigned int helper_cluster_id,
                                           const std::string& main_proxy,
                                           int block_idx)
    {
      double total_sec = 0.0;
      size_t offset = 0;
      int chunk_idx = 0;
      while (offset < total_bytes) {
        const size_t chunk_bytes =
            std::min(kHelpCrossTraceChunkBytes, total_bytes - offset);
        const auto chunk_start = std::chrono::steady_clock::now();
        asio::write(socket, asio::buffer(data + offset, chunk_bytes));
        const double chunk_sec = trace_elapsed_sec(chunk_start);
        total_sec += chunk_sec;

        std::ostringstream trace;
        trace << "[HCCHUNK] direction=send"
              << " task_id=" << (task_id.empty() ? "-" : task_id)
              << " helper_cluster=" << helper_cluster_id
              << " main_proxy=" << main_proxy
              << " block_idx=" << block_idx
              << " chunk_idx=" << chunk_idx
              << " offset=" << offset
              << " bytes=" << chunk_bytes
              << " sec=" << chunk_sec;
        write_help_cross_trace(trace);

        offset += chunk_bytes;
        ++chunk_idx;
      }
      return total_sec;
    }
  }

  MainRepairResp Proxy::main_repair(MainRepairPlan repair_plan)
  {
    // 当 Coordinator 分配了独立 data_port 时，使用局部 acceptor 避免同 proxy 多 main_repair 冲突
    std::shared_ptr<asio::ip::tcp::acceptor> local_acceptor;
    if (repair_plan.data_port > 0) {
      local_acceptor = std::make_shared<asio::ip::tcp::acceptor>(
          io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), repair_plan.data_port));
    }
    // data_port==0 时仍使用共享 acceptor_，需要互斥锁防止多 worker 并发 accept
    std::unique_ptr<std::lock_guard<std::mutex>> main_repair_gate;
    if (repair_plan.data_port == 0) {
      main_repair_gate.reset(new std::lock_guard<std::mutex>(main_repair_accept_mutex_));
    }
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };
    struct timeval start_time, end_time;
    double decoding_time = 0;
    double cross_cluster_time = 0;
    auto io_time_ptr = std::make_shared<double>(0.0);
    auto inner_net_time_ptr = std::make_shared<double>(0.0);
    auto io_time_mtx = std::make_shared<std::mutex>();

    bool if_partial_decoding = repair_plan.partial_decoding;
    bool partial_scheme = repair_plan.partial_scheme;
    size_t block_size = repair_plan.block_size;
    auto ec = ec_factory(repair_plan.ec_type, repair_plan.cp);
    ec->init_coding_parameters(repair_plan.cp);
    int failed_num = (int)repair_plan.failed_blocks_index.size();
    int *erasures = new int[failed_num + 1];
    for (int i = 0; i < failed_num; i++) {
      erasures[i] = repair_plan.failed_blocks_index[i];
    }
    erasures[failed_num] = -1;

    if (IF_DEBUG) {
      std::cout << "[Main Proxy " << self_cluster_id_ << "] To repair ";
      for (int i = 0; i < failed_num; i++) {
        std::cout << erasures[i] << " ";
      }
      std::cout << std::endl;
    }

    auto live_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.live_blocks_index);
    auto fls_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.failed_blocks_index);
    auto parity_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.parity_blocks_index);
    int parity_num = (int)parity_idx_ptr->size();

    auto original_lock_ptr = std::make_shared<std::mutex>();
    auto original_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto original_blocks_idx_ptr = std::make_shared<std::vector<int>>();

    auto get_from_node = [this, original_lock_ptr, original_blocks_ptr,
                          original_blocks_idx_ptr, block_size, io_time_ptr,
                          inner_net_time_ptr, io_time_mtx](
                          int block_idx, unsigned int block_id,
                          std::string node_ip, int node_port) mutable
    {
      std::string block_id_str = std::to_string(block_id);
      std::vector<char> tmp_val(block_size);
      double disk_io = 0;
      double inner_net = 0;
      bool res = read_from_datanode(block_id_str.c_str(), block_id_str.size(), 
                         tmp_val.data(), block_size, node_ip.c_str(), node_port,
                         &disk_io, &inner_net);
      {
        std::lock_guard<std::mutex> lk(*io_time_mtx);
        *io_time_ptr += disk_io;
        *inner_net_time_ptr += inner_net;
      }
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
    auto decoding_time_ptr = std::make_shared<std::vector<double>>();
    const unsigned int main_cluster_id = repair_plan.cluster_id;
    const int main_data_port = repair_plan.data_port;

    auto get_from_proxy = [this, original_lock_ptr, original_blocks_ptr,
                           original_blocks_idx_ptr, partial_lock_ptr,
                           partial_blocks_ptr, partial_blocks_idx_ptr,
                           block_size, decoding_time_ptr, main_cluster_id, main_data_port]
                           (std::shared_ptr<asio::ip::tcp::socket> socket_ptr) mutable
    {
      const auto recv_total_start = std::chrono::steady_clock::now();
      double read_cluster_sec = 0.0;
      double read_flag_sec = 0.0;
      double read_task_sec = 0.0;
      double read_count_sec = 0.0;
      double read_idx_sec = 0.0;
      double read_payload_sec = 0.0;
      double read_decode_sec = 0.0;
      int payload_count = 0;
      std::string task_id;
      asio::error_code error;
      std::vector<unsigned char> int_buf(sizeof(int));
      auto step_start = std::chrono::steady_clock::now();
      asio::read(*socket_ptr, asio::buffer(int_buf, int_buf.size()), error);
      read_cluster_sec = trace_elapsed_sec(step_start);
      int t_cluster_id = ECProject::bytes_to_int(int_buf);
      std::vector<unsigned char> int_flag_buf(sizeof(int));
      step_start = std::chrono::steady_clock::now();
      asio::read(*socket_ptr, asio::buffer(int_flag_buf, int_flag_buf.size()), error);
      read_flag_sec = trace_elapsed_sec(step_start);
      int t_flag = ECProject::bytes_to_int(int_flag_buf);
      if (t_flag >= 2) {
        step_start = std::chrono::steady_clock::now();
        std::vector<unsigned char> task_size_buf(sizeof(int));
        asio::read(*socket_ptr, asio::buffer(task_size_buf, task_size_buf.size()), error);
        int task_size = ECProject::bytes_to_int(task_size_buf);
        if (task_size > 0) {
          std::vector<char> task_buf(static_cast<size_t>(task_size));
          asio::read(*socket_ptr, asio::buffer(task_buf.data(), task_buf.size()), error);
          task_id.assign(task_buf.begin(), task_buf.end());
        }
        read_task_sec = trace_elapsed_sec(step_start);
        t_flag = 1;
      }
      std::string msg = "data";
      if(t_flag)
        msg = "partial";
      if (IF_DEBUG) {
        std::cout << "[Main Proxy " << self_cluster_id_ << "] Try to get "
                  << msg << " blocks from the proxy in cluster " << t_cluster_id
                  << ". " << std::endl;
      }
      if(t_flag) {  // receive partial blocks from helper proxies
        std::vector<unsigned char> num_buf(sizeof(int));
        step_start = std::chrono::steady_clock::now();
        asio::read(*socket_ptr, asio::buffer(num_buf, num_buf.size()), error);
        read_count_sec = trace_elapsed_sec(step_start);
        int partial_block_num = ECProject::bytes_to_int(num_buf);
        payload_count = partial_block_num;
        partial_lock_ptr->lock();
        for (int i = 0; i < partial_block_num; i++) {
          std::vector<unsigned char> block_idx_buf(sizeof(int));
          step_start = std::chrono::steady_clock::now();
          asio::read(*socket_ptr, asio::buffer(block_idx_buf,
              block_idx_buf.size()), error);
          read_idx_sec += trace_elapsed_sec(step_start);
          int block_idx = ECProject::bytes_to_int(block_idx_buf);
          partial_blocks_idx_ptr->push_back(block_idx);
          std::vector<char> tmp_val(block_size);
          read_payload_sec += read_help_cross_payload_chunks(
              *socket_ptr, tmp_val.data(), block_size, task_id, main_cluster_id,
              t_cluster_id, block_idx);
          partial_blocks_ptr->push_back(tmp_val);
        }
        partial_lock_ptr->unlock();
        std::vector<unsigned char> decoding_time_buf(sizeof(double));
        step_start = std::chrono::steady_clock::now();
        asio::read(*socket_ptr, asio::buffer(decoding_time_buf,
                                        decoding_time_buf.size()));
        read_decode_sec = trace_elapsed_sec(step_start);
        double temp_decoding_time = bytes_to_double(decoding_time_buf);
        decoding_time_ptr->push_back(temp_decoding_time);
      } else {  // receive data blocks from help proxies
        std::vector<unsigned char> int_buf_num_of_blocks(sizeof(int));
        step_start = std::chrono::steady_clock::now();
        asio::read(*socket_ptr, asio::buffer(int_buf_num_of_blocks,
            int_buf_num_of_blocks.size()), error);
        read_count_sec = trace_elapsed_sec(step_start);
        int block_num = ECProject::bytes_to_int(int_buf_num_of_blocks);
        payload_count = block_num;
        for (int i = 0; i < block_num; i++) {
          std::vector<char> tmp_val(block_size);
          std::vector<unsigned char> block_idx_buf(sizeof(int));
          step_start = std::chrono::steady_clock::now();
          asio::read(*socket_ptr, asio::buffer(block_idx_buf,
              block_idx_buf.size()), error);
          read_idx_sec += trace_elapsed_sec(step_start);
          int block_idx = ECProject::bytes_to_int(block_idx_buf);
          read_payload_sec += read_help_cross_payload_chunks(
              *socket_ptr, tmp_val.data(), block_size, task_id, main_cluster_id,
              t_cluster_id, block_idx);
          original_lock_ptr->lock();
          original_blocks_ptr->push_back(tmp_val);
          original_blocks_idx_ptr->push_back(block_idx);
          original_lock_ptr->unlock();
        }
        std::vector<unsigned char> decoding_time_buf(sizeof(double));
        step_start = std::chrono::steady_clock::now();
        asio::read(*socket_ptr, asio::buffer(decoding_time_buf,
                                        decoding_time_buf.size()));
        read_decode_sec = trace_elapsed_sec(step_start);
        double temp_decoding_time = bytes_to_double(decoding_time_buf);
        decoding_time_ptr->push_back(temp_decoding_time);
      }

      {
        std::ostringstream trace;
        trace << "[HCRECV] main_cluster=" << main_cluster_id
              << " task_id=" << (task_id.empty() ? "-" : task_id)
              << " data_port=" << main_data_port
              << " helper_cluster=" << t_cluster_id
              << " partial=" << t_flag
              << " block_count=" << payload_count
              << " block_size=" << block_size
              << " bytes_total=" << (static_cast<size_t>(payload_count) * block_size)
              << " read_cluster=" << read_cluster_sec
              << " read_flag=" << read_flag_sec
              << " read_task=" << read_task_sec
              << " read_count=" << read_count_sec
              << " read_idx_total=" << read_idx_sec
              << " read_payload_total=" << read_payload_sec
              << " read_decode_time=" << read_decode_sec
              << " total=" << trace_elapsed_sec(recv_total_start);
        write_help_cross_trace(trace);
      }

      if (IF_DEBUG){
        std::cout << "[Main Proxy " << self_cluster_id_
                  << "] Finish getting data from the proxy in cluster "
                  << t_cluster_id << std::endl;
      }
    };

    auto send_to_datanode = [this, block_size, io_time_ptr, io_time_mtx](
        unsigned int block_id, char *data, std::string node_ip, int node_port)
    {
      std::string block_id_str = std::to_string(block_id);
      double disk_io = 0;
      write_to_datanode(block_id_str.c_str(), block_id_str.size(),
                        data, block_size, node_ip.c_str(), node_port, &disk_io);
      std::lock_guard<std::mutex> lk(*io_time_mtx);
      *io_time_ptr += disk_io;
    };

    // get blocks or partial blocks from other clusters
    int num_of_help_clusters = (int)repair_plan.help_clusters_blocks_info.size();
    int partial_cnt = 0;  // num of partial-block sets

    // 提前启动跨机架接收线程（accept 后立即 read），与机架内读取并行。
    // 将 accept+read 合并到一个线程中，防止 TCP 缓冲区满导致 helper 端
    // write() 阻塞，进而与 coordinator 的 join-per-round 调度形成死锁。
    std::vector<std::thread> cross_cluster_threads;
    if (num_of_help_clusters > 0) {
      for (int i = 0; i < num_of_help_clusters; i++) {
        bool t_flag = repair_plan.help_clusters_partial_less[i];
        t_flag = (if_partial_decoding && t_flag);
        if (!t_flag && IF_DIRECT_FROM_NODE) {
          continue;  // 直连节点：不需要 acceptor
        }
        if (t_flag) {
          partial_cnt++;
        }
        auto socket_ptr = std::make_shared<asio::ip::tcp::socket>(io_context_);
        cross_cluster_threads.push_back(std::thread([this, socket_ptr, local_acceptor,
                                                     main_cluster_id, main_data_port,
                                                     &get_from_proxy]() {
          const auto accept_start = std::chrono::steady_clock::now();
          if (local_acceptor) {
            local_acceptor->accept(*socket_ptr);
          } else {
            acceptor_.accept(*socket_ptr);
          }
          {
            std::ostringstream trace;
            trace << "[HCRECV] main_cluster=" << main_cluster_id
                  << " data_port=" << main_data_port
                  << " stage=accept"
                  << " accept_wait=" << trace_elapsed_sec(accept_start);
            write_help_cross_trace(trace);
          }
          get_from_proxy(socket_ptr);
        }));
      }
    }

    // get original blocks inside cluster（与跨机架 acceptor 并行）
    int num_of_original_blocks =
        (int)repair_plan.inner_cluster_help_blocks_info.size();
    if (num_of_original_blocks > 0) {
      const double phase_read_start = now_phase_sec();
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers.push_back(std::thread(get_from_node,
            repair_plan.inner_cluster_help_blocks_info[i].first,
            repair_plan.inner_cluster_help_block_ids[i],
            repair_plan.inner_cluster_help_blocks_info[i].second.first,
            repair_plan.inner_cluster_help_blocks_info[i].second.second));
      }
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers[i].join();
      }
      add_phase(RepairPhaseType::MAIN_READ_INTRA, phase_read_start, now_phase_sec());

      if (IF_DEBUG) {
        std::cout << "[Main Proxy " << self_cluster_id_
                  << "] Finish getting " << num_of_original_blocks
                  << " blocks inside main cluster." << std::endl;
      }
    }

    const double phase_recv_start = now_phase_sec();
    gettimeofday(&start_time, NULL);
    if (num_of_help_clusters > 0) {
      // 等待跨机架接收线程（accept + read 已合并，立即排空 TCP 缓冲区）
      for (auto& t : cross_cluster_threads) {
        if (t.joinable()) t.join();
      }
      cross_cluster_threads.clear();

      // 为直连节点（非 TCP 传输）创建读取线程
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_help_clusters; i++) {
        int num_of_blocks_in_cluster =
            (int)repair_plan.help_clusters_blocks_info[i].size();
        bool t_flag = repair_plan.help_clusters_partial_less[i];
        t_flag = (if_partial_decoding && t_flag);
        if(!t_flag && IF_DIRECT_FROM_NODE) {  // transfer blocks directly
          num_of_original_blocks += num_of_blocks_in_cluster;
          for (int j = 0; j < num_of_blocks_in_cluster; j++) {
            readers.push_back(std::thread(get_from_node,
                repair_plan.help_clusters_blocks_info[i][j].first,
                repair_plan.help_clusters_block_ids[i][j],
                repair_plan.help_clusters_blocks_info[i][j].second.first,
                repair_plan.help_clusters_blocks_info[i][j].second.second));
          }
        } else {  // TCP 传输已在 cross_cluster_threads 中完成
          if(!t_flag) {
            num_of_original_blocks += num_of_blocks_in_cluster;
          }
        }
      }
      int num_of_readers = (int)readers.size();
      for (int i = 0; i < num_of_readers; i++) {
        readers[i].join();
      }

      // simulate cross-cluster transfer
      if (IF_SIMULATE_CROSS_CLUSTER) {
        int cross_cluster_num = num_of_original_blocks - 
            (int)repair_plan.inner_cluster_help_blocks_info.size();
        cross_cluster_num += (int)partial_blocks_idx_ptr->size();
        size_t t_val_len = (int)block_size * cross_cluster_num;
        std::string t_value = generate_random_string((int)t_val_len);
        transfer_to_networkcore(t_value.c_str(), t_val_len);
      }

      if (decoding_time_ptr->size() > 0) {
        auto max_decode = std::max_element(decoding_time_ptr->begin(),
            decoding_time_ptr->end());
        decoding_time += *max_decode;
      }
    }
    gettimeofday(&end_time, NULL);
    cross_cluster_time += end_time.tv_sec - start_time.tv_sec +
        (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
    if (num_of_help_clusters > 0) {
      add_phase(RepairPhaseType::MAIN_RECV_CROSS, phase_recv_start, now_phase_sec());
    }
    
    std::cout << "[Main Proxy " << self_cluster_id_ << "] Finish getting blocks from "
              << num_of_help_clusters << " help clusters." << std::endl;
    
    my_assert(num_of_original_blocks == (int)original_blocks_ptr->size());
    // 
    const double phase_decode_start = now_phase_sec();
    if (num_of_original_blocks > 0 && if_partial_decoding) {  // encode-and-transfer
      int partial_num = parity_num;
      if (partial_scheme) {
        partial_num = failed_num;
      }
      my_assert(partial_num > 0);
      std::vector<char *> v_data(num_of_original_blocks);
      std::vector<char *> v_coding(partial_num);
      char **data = (char **)v_data.data();
      char **coding = (char **)v_coding.data();
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      std::vector<std::vector<char>>
          v_coding_area(partial_num, std::vector<char>(block_size));
      for (int j = 0; j < partial_num; j++) {
        coding[j] = v_coding_area[j].data();
      }
      
      auto partial_flags = std::vector<bool>(partial_num, true);

      gettimeofday(&start_time, NULL);
      if (partial_scheme) {
        my_assert(parity_idx_ptr->size() > 0);
      }
      ec->encode_partial_blocks(data, coding, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr,
          *fls_idx_ptr, *live_idx_ptr, partial_flags, partial_scheme);
      gettimeofday(&end_time, NULL);
      decoding_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;

      partial_lock_ptr->lock();
      for (int j = 0; j < partial_num; j++) {
        if (partial_flags[j]) {
          if (partial_scheme) {
            partial_blocks_idx_ptr->push_back((*fls_idx_ptr)[j]);
          } else {
            partial_blocks_idx_ptr->push_back((*parity_idx_ptr)[j]);
          }
          partial_blocks_ptr->push_back(v_coding_area[j]);
        }
      }
      partial_lock_ptr->unlock();
      partial_cnt++;
    }

    std::vector<char *> v_failures(failed_num);
    char **failures = (char **)v_failures.data();
    std::vector<std::vector<char>> v_failures_area(failed_num, std::vector<char>(block_size));
    for (int i = 0; i < failed_num; i++) {
      failures[i] = v_failures_area[i].data();
    }
    int num_of_partial_blocks = (int)partial_blocks_ptr->size();
    if (num_of_partial_blocks > 0) {
      if (IF_DEBUG) {
        std::cout << "[Main Proxy " << self_cluster_id_
                  << "] Ready to perform addition with "
                  << num_of_partial_blocks << " partial blocks!\n";
      }
      std::vector<char *> v_data(num_of_partial_blocks);
      char **data = (char **)v_data.data();
      for (int j = 0; j < num_of_partial_blocks; j++) {
        data[j] = (*partial_blocks_ptr)[j].data();
      }

      if (partial_scheme) {
        gettimeofday(&start_time, NULL);
        ec->perform_addition(data, failures, block_size,
            *partial_blocks_idx_ptr, *fls_idx_ptr);
        gettimeofday(&end_time, NULL);
        decoding_time += end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      } else {
        std::vector<char *> v_coding(parity_num);
        char **coding = (char **)v_coding.data();
        std::vector<std::vector<char>>
            v_coding_area(parity_num, std::vector<char>(block_size));
        for (int j = 0; j < parity_num; j++) {
          coding[j] = v_coding_area[j].data();
        }

        gettimeofday(&start_time, NULL);
        ec->perform_addition(data, coding, block_size,
            *partial_blocks_idx_ptr, *parity_idx_ptr);
        gettimeofday(&end_time, NULL);
        decoding_time += end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
        
        if (IF_DEBUG) {
          std::cout << "[Main Proxy " << self_cluster_id_
                    << "] Ready to decode with partial blocks!\n";
        }

        gettimeofday(&start_time, NULL);
        ec->decode_with_partial_blocks(coding, failures, block_size,
            *fls_idx_ptr, *parity_idx_ptr);
        gettimeofday(&end_time, NULL);
        decoding_time += end_time.tv_sec - start_time.tv_sec +
            (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      }
    } else {
      std::vector<char *> v_data(num_of_original_blocks);
      char **data = (char **)v_data.data();
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      
      auto partial_flags = std::vector<bool>(failed_num, true);

      gettimeofday(&start_time, NULL);
      ec->encode_partial_blocks(data, failures, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr,
          *fls_idx_ptr, *live_idx_ptr, partial_flags, true);
      gettimeofday(&end_time, NULL);
      decoding_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
    }
    add_phase(RepairPhaseType::MAIN_DECODE, phase_decode_start, now_phase_sec());

    const double phase_write_start = now_phase_sec();
    std::vector<std::thread> writers;
    for (int i = 0; i < failed_num; i++) {
      int index = repair_plan.failed_blocks_index[i];
      unsigned int failed_block_id = repair_plan.failed_block_ids[i];
      writers.push_back(std::thread(send_to_datanode,
          failed_block_id, failures[i],
          repair_plan.new_locations[i].second.first,
          repair_plan.new_locations[i].second.second));
    }
    for (int i = 0; i < failed_num; i++) {
      writers[i].join();
    }
    add_phase(RepairPhaseType::MAIN_WRITEBACK, phase_write_start, now_phase_sec());

    if (IF_SIMULATE_CROSS_CLUSTER) {
      const double phase_sim_cross_start = now_phase_sec();
      gettimeofday(&start_time, NULL);
      int cross_cluster_num = 0;
      for (int i = 0; i < (int)repair_plan.new_locations.size(); i++) {
        if(repair_plan.new_locations[i].first != self_cluster_id_) {
          cross_cluster_num++;   
        }
      }
      if (cross_cluster_num > 0) {
        int t_value_len = (int)block_size * cross_cluster_num;
        std::vector<char> t_value(t_value_len);
        transfer_to_networkcore(t_value.data(), t_value_len);
      }
      gettimeofday(&end_time, NULL);
      cross_cluster_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::MAIN_RECV_CROSS, phase_sim_cross_start, now_phase_sec());
    }

    if (IF_DEBUG) {
      std::cout << "[Main Proxy" << self_cluster_id_ << "] finish repair "
                << failed_num << " blocks! Decoding time : "
                << decoding_time << std::endl;
    }

    delete ec;

    MainRepairResp response;
    response.decoding_time = decoding_time;
    response.cross_cluster_time = cross_cluster_time;
    response.io_time = *io_time_ptr;
    response.inner_network_time = *inner_net_time_ptr;
    response.phase_spans = std::move(phase_spans);
    response.phase_total_time = now_phase_sec();

    return response;
  }

  HelpRepairResp Proxy::help_repair(HelpRepairPlan repair_plan)
  {
    std::cout << "[DEBUG proxy help_repair] received plan.cluster_id=" << repair_plan.cluster_id
              << " main_proxy_ip=" << repair_plan.main_proxy_ip
              << " main_proxy_port=" << repair_plan.main_proxy_port
              << " self_cluster_id=" << self_cluster_id_ << std::endl;
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };
    struct timeval start_time, end_time;
    double decoding_time = 0;
    double cross_cluster_time = 0;
    auto io_time_ptr = std::make_shared<double>(0.0);
    auto inner_net_time_ptr = std::make_shared<double>(0.0);
    auto io_time_mtx = std::make_shared<std::mutex>();

    bool if_partial_decoding = repair_plan.partial_decoding;
    bool partial_scheme = repair_plan.partial_scheme;
    bool t_flag = repair_plan.partial_less;
    int num_of_original_blocks = (int)repair_plan.inner_cluster_help_blocks_info.size();
    t_flag = (if_partial_decoding && t_flag);
    if (!t_flag && IF_DIRECT_FROM_NODE) {
      HelpRepairResp resp{};
      resp.decoding_time = 0;
      resp.cross_cluster_time = 0;
      resp.io_time = 0;
      resp.inner_network_time = 0;
      resp.phase_total_time = now_phase_sec();
      return resp;
    }

    auto ec = ec_factory(repair_plan.ec_type, repair_plan.cp);
    ec->init_coding_parameters(repair_plan.cp);
    size_t block_size = repair_plan.block_size;

    auto parity_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.parity_blocks_index);
    auto fls_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.failed_blocks_index);
    auto live_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.live_blocks_index);

    auto original_lock_ptr = std::make_shared<std::mutex>();
    auto original_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto original_blocks_idx_ptr = std::make_shared<std::vector<int>>();

    auto get_from_node = [this, original_lock_ptr, original_blocks_ptr,
                          original_blocks_idx_ptr, block_size, io_time_ptr,
                          inner_net_time_ptr, io_time_mtx](
                          unsigned int block_id, int block_idx,
                          std::string node_ip, int node_port) mutable
    {
      std::string block_id_str = std::to_string(block_id);
      std::vector<char> tmp_val(block_size);
      double disk_io = 0;
      double inner_net = 0;
      bool res = read_from_datanode(block_id_str.c_str(), block_id_str.size(), 
              tmp_val.data(), block_size, node_ip.c_str(), node_port, &disk_io,
              &inner_net);
      {
        std::lock_guard<std::mutex> lk(*io_time_mtx);
        *io_time_ptr += disk_io;
        *inner_net_time_ptr += inner_net;
      }
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
      const double phase_read_start = now_phase_sec();
      std::vector<std::thread> readers;
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers.push_back(std::thread(get_from_node, 
                          repair_plan.inner_cluster_help_block_ids[i], 
                          repair_plan.inner_cluster_help_blocks_info[i].first,
                          repair_plan.inner_cluster_help_blocks_info[i].second.first,
                          repair_plan.inner_cluster_help_blocks_info[i].second.second));
      }
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers[i].join();
      }
      add_phase(RepairPhaseType::HELP_READ_INTRA, phase_read_start, now_phase_sec());
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
      int partial_num = (int)parity_idx_ptr->size();
      if (partial_scheme) {
        partial_num = (int)fls_idx_ptr->size();
      }
      my_assert(partial_num > 0);
      std::vector<char *> v_data(num_of_original_blocks);
      std::vector<char *> v_coding(partial_num);
      char **data = (char **)v_data.data();
      char **coding = (char **)v_coding.data();
      std::vector<std::vector<char>> v_coding_area(partial_num, std::vector<char>(block_size));
      for (int j = 0; j < partial_num; j++) {
        coding[j] = v_coding_area[j].data();
      }
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      auto partial_flags = std::vector<bool>(partial_num, true);
      if (partial_scheme) {
        my_assert(parity_idx_ptr->size() > 0);
      }
      const double phase_encode_start = now_phase_sec();
      gettimeofday(&start_time, NULL);
      ec->encode_partial_blocks(data, coding, block_size,
          *original_blocks_idx_ptr, *parity_idx_ptr,
          *fls_idx_ptr, *live_idx_ptr, partial_flags, partial_scheme);
      gettimeofday(&end_time, NULL);
      decoding_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_ENCODE, phase_encode_start, now_phase_sec());
      int num_of_partial_blocks = 0;
      for (int j = 0; j < partial_num; j++) {
        if (partial_flags[j]) {
          num_of_partial_blocks++;
        }
      }
      my_assert(num_of_partial_blocks < num_of_original_blocks);

      // send to main proxy (纯网络：connect + write)
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      if (IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Try to connect main proxy port "
                  << repair_plan.main_proxy_port << std::endl;
      }
      const double phase_send_start = now_phase_sec();
      gettimeofday(&start_time, NULL);
      asio::connect(socket_, resolver.resolve({repair_plan.main_proxy_ip,
          std::to_string(repair_plan.main_proxy_port)}), con_error);
      if (!con_error && IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Connect to " << repair_plan.main_proxy_ip
                  << ":" << repair_plan.main_proxy_port << " success!"
                  << std::endl;
      }
      std::vector<unsigned char>
          cid_buf = ECProject::int_to_bytes(static_cast<int>(repair_plan.cluster_id));
      asio::write(socket_, asio::buffer(cid_buf, cid_buf.size()));
      std::vector<unsigned char> flag_buf = ECProject::int_to_bytes(1);
      asio::write(socket_, asio::buffer(flag_buf, flag_buf.size()));
      std::vector<unsigned char> num_buf = int_to_bytes(num_of_partial_blocks);
      asio::write(socket_, asio::buffer(num_buf, num_buf.size()));
      for (int j = 0; j < partial_num; j++) {
        if (partial_flags[j]) {
          std::vector<unsigned char> idx_buf;
          if (partial_scheme) {
            idx_buf = ECProject::int_to_bytes((*fls_idx_ptr)[j]);
          } else {
            idx_buf = ECProject::int_to_bytes((*parity_idx_ptr)[j]);
          }
          asio::write(socket_, asio::buffer(idx_buf, idx_buf.size()));
          asio::write(socket_, asio::buffer(coding[j], block_size));
          value_size += block_size;
        }
      }
      std::vector<unsigned char> decoding_time_buf = double_to_bytes(decoding_time);
      asio::write(socket_, asio::buffer(decoding_time_buf, decoding_time_buf.size()));
      gettimeofday(&end_time, NULL);
      cross_cluster_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_SEND_CROSS, phase_send_start, now_phase_sec());
    } else if(!IF_DIRECT_FROM_NODE)  {
      // send to main proxy (纯网络：connect + write)
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      if (IF_DEBUG) {
        std::cout << "[Helper Proxy" << self_cluster_id_
                  << "] Try to connect main proxy port "
                  << repair_plan.main_proxy_port << std::endl;
      }
      const double phase_send_start = now_phase_sec();
      gettimeofday(&start_time, NULL);
      asio::connect(socket_, resolver.resolve({repair_plan.main_proxy_ip,
            std::to_string(repair_plan.main_proxy_port)}), con_error);
      if (!con_error && IF_DEBUG) {
            std::cout << "[Helper Proxy" << self_cluster_id_
            << "] Connect to " << repair_plan.main_proxy_ip<< ":"
            << repair_plan.main_proxy_port << " success!" << std::endl;
      }
      
      std::vector<unsigned char>
          int_buf_self_cluster_id =
              ECProject::int_to_bytes(static_cast<int>(repair_plan.cluster_id));
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
      gettimeofday(&end_time, NULL);
      cross_cluster_time += end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_SEND_CROSS, phase_send_start, now_phase_sec());
    }
        
    if (IF_DEBUG) {
       std::cout << "[Helper Proxy" << self_cluster_id_
       << "] Send value to proxy" <<  repair_plan.main_proxy_port 
       << "! With length of " << ". Decoding time : " << decoding_time << std::endl;
    }

    HelpRepairResp resp{};
    resp.decoding_time = decoding_time;
    resp.cross_cluster_time = cross_cluster_time;
    resp.io_time = *io_time_ptr;
    resp.inner_network_time = *inner_net_time_ptr;
    resp.phase_spans = std::move(phase_spans);
    resp.phase_total_time = now_phase_sec();
    return resp;
  }

  HelpRepairPrepareResp Proxy::prepare_help_repair_data(HelpRepairPrepareReq req)
  {
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };

    HelpRepairPrepareResp resp{};
    const HelpRepairPlan& repair_plan = req.repair_plan;
    const std::string& task_id = req.task_id;
    if (task_id.empty()) {
      resp.success = false;
      resp.err_msg = "empty task_id";
      return resp;
    }

    bool if_partial_decoding = repair_plan.partial_decoding;
    bool partial_scheme = repair_plan.partial_scheme;
    bool t_flag = repair_plan.partial_less;
    t_flag = (if_partial_decoding && t_flag);
    const int num_of_original_blocks =
        static_cast<int>(repair_plan.inner_cluster_help_blocks_info.size());
    const size_t block_size = repair_plan.block_size;

    PreparedHelpPayload prepared{};
    prepared.block_size = block_size;
    prepared.cluster_id = repair_plan.cluster_id;
    prepared.main_proxy_ip = repair_plan.main_proxy_ip;
    prepared.main_proxy_port = repair_plan.main_proxy_port;

    auto io_time_ptr = std::make_shared<double>(0.0);
    auto inner_net_time_ptr = std::make_shared<double>(0.0);
    auto io_time_mtx = std::make_shared<std::mutex>();
    auto original_lock_ptr = std::make_shared<std::mutex>();
    auto original_blocks_ptr = std::make_shared<std::vector<std::vector<char>>>();
    auto original_blocks_idx_ptr = std::make_shared<std::vector<int>>();

    auto get_from_node = [this, original_lock_ptr, original_blocks_ptr,
                          original_blocks_idx_ptr, block_size, io_time_ptr,
                          inner_net_time_ptr, io_time_mtx](
                             unsigned int block_id, int block_idx,
                             std::string node_ip, int node_port) mutable {
      std::string block_id_str = std::to_string(block_id);
      std::vector<char> tmp_val(block_size);
      double disk_io = 0;
      double inner_net = 0;
      bool read_ok = read_from_datanode(block_id_str.c_str(), block_id_str.size(),
                                        tmp_val.data(), block_size, node_ip.c_str(),
                                        node_port, &disk_io, &inner_net);
      {
        std::lock_guard<std::mutex> lk(*io_time_mtx);
        *io_time_ptr += disk_io;
        *inner_net_time_ptr += inner_net;
      }
      if (!read_ok) {
        return;
      }
      std::lock_guard<std::mutex> lk(*original_lock_ptr);
      original_blocks_ptr->push_back(std::move(tmp_val));
      original_blocks_idx_ptr->push_back(block_idx);
    };

    if (num_of_original_blocks > 0) {
      const double phase_read_start = now_phase_sec();
      std::vector<std::thread> readers;
      readers.reserve(static_cast<size_t>(num_of_original_blocks));
      for (int i = 0; i < num_of_original_blocks; i++) {
        readers.emplace_back(get_from_node,
                             repair_plan.inner_cluster_help_block_ids[i],
                             repair_plan.inner_cluster_help_blocks_info[i].first,
                             repair_plan.inner_cluster_help_blocks_info[i].second.first,
                             repair_plan.inner_cluster_help_blocks_info[i].second.second);
      }
      for (auto& t : readers) {
        t.join();
      }
      add_phase(RepairPhaseType::HELP_READ_INTRA, phase_read_start, now_phase_sec());
    }

    if (static_cast<int>(original_blocks_ptr->size()) != num_of_original_blocks) {
      resp.success = false;
      resp.err_msg = "failed to read original blocks from datanode";
      return resp;
    }

    double decoding_time = 0.0;
    std::unique_ptr<ErasureCode> ec(ec_factory(repair_plan.ec_type, repair_plan.cp));
    ec->init_coding_parameters(repair_plan.cp);
    auto parity_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.parity_blocks_index);
    auto fls_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.failed_blocks_index);
    auto live_idx_ptr = std::make_shared<std::vector<int>>(
        repair_plan.live_blocks_index);

    if (t_flag) {
      int partial_num = static_cast<int>(parity_idx_ptr->size());
      if (partial_scheme) {
        partial_num = static_cast<int>(fls_idx_ptr->size());
      }
      if (partial_num <= 0) {
        resp.success = false;
        resp.err_msg = "invalid partial block number";
        return resp;
      }
      std::vector<char*> v_data(num_of_original_blocks);
      std::vector<char*> v_coding(partial_num);
      char** data = v_data.data();
      char** coding = v_coding.data();
      std::vector<std::vector<char>> v_coding_area(
          partial_num, std::vector<char>(block_size));
      for (int j = 0; j < partial_num; j++) {
        coding[j] = v_coding_area[j].data();
      }
      for (int j = 0; j < num_of_original_blocks; j++) {
        data[j] = (*original_blocks_ptr)[j].data();
      }
      auto partial_flags = std::vector<bool>(partial_num, true);
      const double phase_encode_start = now_phase_sec();
      struct timeval start_time{}, end_time{};
      gettimeofday(&start_time, NULL);
      ec->encode_partial_blocks(data, coding, block_size, *original_blocks_idx_ptr,
                                *parity_idx_ptr, *fls_idx_ptr, *live_idx_ptr,
                                partial_flags, partial_scheme);
      gettimeofday(&end_time, NULL);
      decoding_time = end_time.tv_sec - start_time.tv_sec +
                      (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_ENCODE, phase_encode_start, now_phase_sec());

      prepared.need_send = true;
      prepared.send_decoding_time = true;
      prepared.decoding_time = decoding_time;
      for (int j = 0; j < partial_num; j++) {
        if (!partial_flags[j]) {
          continue;
        }
        if (partial_scheme) {
          prepared.block_indices.push_back((*fls_idx_ptr)[j]);
        } else {
          prepared.block_indices.push_back((*parity_idx_ptr)[j]);
        }
        prepared.block_values.push_back(std::move(v_coding_area[j]));
      }
    } else if (!IF_DIRECT_FROM_NODE) {
      prepared.need_send = true;
      prepared.send_decoding_time = false;
      prepared.block_indices = *original_blocks_idx_ptr;
      prepared.block_values = *original_blocks_ptr;
    } else {
      // direct-from-node 且无需 partial 时，不需要跨机架发送
      prepared.need_send = false;
    }

    prepared.io_time = *io_time_ptr;
    prepared.inner_network_time = *inner_net_time_ptr;
    {
      std::lock_guard<std::mutex> lk(prepared_help_payloads_mutex_);
      prepared_help_payloads_[task_id] = std::move(prepared);
    }

    resp.decoding_time = decoding_time;
    resp.io_time = *io_time_ptr;
    resp.inner_network_time = *inner_net_time_ptr;
    resp.phase_spans = std::move(phase_spans);
    resp.phase_total_time = now_phase_sec();
    return resp;
  }

  HelpRepairSendResp Proxy::send_prepared_help_repair_data(HelpRepairSendReq req)
  {
    const auto total_start = std::chrono::steady_clock::now();
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };

    HelpRepairSendResp resp{};
    if (req.task_id.empty()) {
      resp.success = false;
      resp.err_msg = "empty task_id";
      return resp;
    }

    PreparedHelpPayload prepared{};
    double lookup_sec = 0.0;
    {
      const auto lookup_start = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lk(prepared_help_payloads_mutex_);
      auto it = prepared_help_payloads_.find(req.task_id);
      if (it == prepared_help_payloads_.end()) {
        resp.success = false;
        resp.err_msg = "prepared payload not found";
        return resp;
      }
      prepared = std::move(it->second);
      prepared_help_payloads_.erase(it);
      lookup_sec = trace_elapsed_sec(lookup_start);
    }

    if (!prepared.need_send) {
      std::ostringstream trace;
      trace << "[HCSEND] task_id=" << req.task_id
            << " helper_cluster=" << prepared.cluster_id
            << " main_proxy=" << prepared.main_proxy_ip << ":" << prepared.main_proxy_port
            << " need_send=0"
            << " lookup=" << lookup_sec
            << " total=" << trace_elapsed_sec(total_start);
      write_help_cross_trace(trace);
      resp.phase_spans = std::move(phase_spans);
      resp.phase_total_time = now_phase_sec();
      return resp;
    }

    double resolve_sec = 0.0;
    double connect_sec = 0.0;
    double write_header_sec = 0.0;
    double write_idx_sec = 0.0;
    double write_payload_sec = 0.0;
    double write_decode_sec = 0.0;
    const size_t block_count = prepared.block_indices.size();
    const size_t bytes_total = block_count * prepared.block_size;
      const std::string main_proxy =
          prepared.main_proxy_ip + ":" + std::to_string(prepared.main_proxy_port);

    try {
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      const double phase_send_start = now_phase_sec();
      struct timeval start_time{}, end_time{};
      gettimeofday(&start_time, NULL);
      auto step_start = std::chrono::steady_clock::now();
      asio::ip::tcp::resolver::query query(
          prepared.main_proxy_ip, std::to_string(prepared.main_proxy_port));
      auto endpoints = resolver.resolve(query);
      resolve_sec = trace_elapsed_sec(step_start);
      step_start = std::chrono::steady_clock::now();
      asio::connect(socket_, endpoints, con_error);
      connect_sec = trace_elapsed_sec(step_start);
      if (con_error) {
        resp.success = false;
        resp.err_msg = con_error.message();
        return resp;
      }
      std::vector<unsigned char> cid_buf =
          ECProject::int_to_bytes(static_cast<int>(prepared.cluster_id));
      step_start = std::chrono::steady_clock::now();
      asio::write(socket_, asio::buffer(cid_buf, cid_buf.size()));
      // flag=2 表示 prepared-send 调试协议：后续携带 task_id；旧 flag=1 仍兼容。
      std::vector<unsigned char> flag_buf = ECProject::int_to_bytes(2);
      asio::write(socket_, asio::buffer(flag_buf, flag_buf.size()));
      std::vector<unsigned char> task_size_buf =
          ECProject::int_to_bytes(static_cast<int>(req.task_id.size()));
      asio::write(socket_, asio::buffer(task_size_buf, task_size_buf.size()));
      if (!req.task_id.empty()) {
        asio::write(socket_, asio::buffer(req.task_id.data(), req.task_id.size()));
      }
      std::vector<unsigned char> num_buf =
          int_to_bytes(static_cast<int>(prepared.block_indices.size()));
      asio::write(socket_, asio::buffer(num_buf, num_buf.size()));
      write_header_sec = trace_elapsed_sec(step_start);
      for (size_t i = 0; i < prepared.block_indices.size(); ++i) {
        std::vector<unsigned char> idx_buf =
            ECProject::int_to_bytes(prepared.block_indices[i]);
        step_start = std::chrono::steady_clock::now();
        asio::write(socket_, asio::buffer(idx_buf, idx_buf.size()));
        write_idx_sec += trace_elapsed_sec(step_start);
        write_payload_sec += write_help_cross_payload_chunks(
            socket_, prepared.block_values[i].data(), prepared.block_size, req.task_id,
            prepared.cluster_id, main_proxy, prepared.block_indices[i]);
      }
      if (prepared.send_decoding_time) {
        std::vector<unsigned char> decoding_time_buf =
            double_to_bytes(prepared.decoding_time);
        step_start = std::chrono::steady_clock::now();
        asio::write(socket_, asio::buffer(decoding_time_buf, decoding_time_buf.size()));
        write_decode_sec = trace_elapsed_sec(step_start);
      }
      gettimeofday(&end_time, NULL);
      resp.cross_cluster_time = end_time.tv_sec - start_time.tv_sec +
                                (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_SEND_CROSS, phase_send_start, now_phase_sec());
    } catch (const std::exception& e) {
      resp.success = false;
      resp.err_msg = e.what();
      return resp;
    }

    {
      std::ostringstream trace;
      trace << "[HCSEND] task_id=" << req.task_id
            << " helper_cluster=" << prepared.cluster_id
            << " main_proxy=" << main_proxy
            << " need_send=1"
            << " block_count=" << block_count
            << " block_size=" << prepared.block_size
            << " bytes_total=" << bytes_total
            << " lookup=" << lookup_sec
            << " resolve=" << resolve_sec
            << " connect=" << connect_sec
            << " write_header=" << write_header_sec
            << " write_idx_total=" << write_idx_sec
            << " write_payload_total=" << write_payload_sec
            << " write_decode_time=" << write_decode_sec
            << " cross_cluster_time=" << resp.cross_cluster_time
            << " total=" << trace_elapsed_sec(total_start);
      write_help_cross_trace(trace);
    }

    resp.phase_spans = std::move(phase_spans);
    resp.phase_total_time = now_phase_sec();
    return resp;
  }

  HelpCrossWarmupResp Proxy::receive_help_cross_warmup(HelpCrossWarmupReq req)
  {
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    HelpCrossWarmupResp resp{};
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };

    try {
      asio::ip::tcp::acceptor acceptor(io_context_);
      asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), req.main_proxy_port);
      acceptor.open(endpoint.protocol());
      acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
      acceptor.bind(endpoint);
      acceptor.listen();
      asio::ip::tcp::socket socket_(io_context_);
      const double phase_start = now_phase_sec();
      struct timeval start_time{}, end_time{};
      gettimeofday(&start_time, NULL);
      acceptor.accept(socket_);
      std::vector<char> buf(kHelpCrossTraceChunkBytes);
      size_t remaining = req.bytes_total;
      while (remaining > 0) {
        const size_t chunk = std::min(buf.size(), remaining);
        asio::read(socket_, asio::buffer(buf.data(), chunk));
        remaining -= chunk;
      }
      gettimeofday(&end_time, NULL);
      resp.cross_cluster_time =
          end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_WARMUP_CROSS, phase_start, now_phase_sec());
      {
        std::ostringstream trace;
        trace << "[HCWARMUP_RECV] task_id=" << req.task_id
              << " main_cluster=" << req.main_cluster_id
              << " helper_cluster=" << req.helper_cluster_id
              << " listen_port=" << req.main_proxy_port
              << " bytes_total=" << req.bytes_total
              << " cross_cluster_time=" << resp.cross_cluster_time;
        write_help_cross_trace(trace);
      }
    } catch (const std::exception& e) {
      resp.success = false;
      resp.err_msg = e.what();
    }
    resp.phase_spans = std::move(phase_spans);
    resp.phase_total_time = now_phase_sec();
    return resp;
  }

  HelpCrossWarmupResp Proxy::send_help_cross_warmup(HelpCrossWarmupReq req)
  {
    const auto phase_t0 = std::chrono::steady_clock::now();
    auto now_phase_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - phase_t0;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    HelpCrossWarmupResp resp{};
    std::vector<RepairPhaseSpan> phase_spans;
    auto add_phase = [&](RepairPhaseType phase, double s, double e) {
      if (e <= s) return;
      phase_spans.push_back({static_cast<int>(phase), s, e});
    };

    try {
      std::vector<char> data(kHelpCrossTraceChunkBytes);
      std::mt19937 rng(static_cast<unsigned int>(
          std::chrono::steady_clock::now().time_since_epoch().count()));
      std::uniform_int_distribution<int> dist(0, 255);
      for (char& c : data) {
        c = static_cast<char>(dist(rng));
      }

      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::resolver resolver(io_context_);
      asio::error_code con_error;
      auto endpoints = resolver.resolve(
          asio::ip::tcp::resolver::query(req.main_proxy_ip,
                                         std::to_string(req.main_proxy_port)));
      const double phase_start = now_phase_sec();
      struct timeval start_time{}, end_time{};
      gettimeofday(&start_time, NULL);
      for (int attempt = 0; attempt < 100; ++attempt) {
        asio::error_code ignored;
        socket_.close(ignored);
        asio::connect(socket_, endpoints, con_error);
        if (!con_error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (con_error) {
        resp.success = false;
        resp.err_msg = con_error.message();
        return resp;
      }
      size_t remaining = req.bytes_total;
      while (remaining > 0) {
        const size_t chunk = std::min(data.size(), remaining);
        asio::write(socket_, asio::buffer(data.data(), chunk));
        remaining -= chunk;
      }
      gettimeofday(&end_time, NULL);
      resp.cross_cluster_time =
          end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      add_phase(RepairPhaseType::HELP_WARMUP_CROSS, phase_start, now_phase_sec());
      {
        std::ostringstream trace;
        trace << "[HCWARMUP_SEND] task_id=" << req.task_id
              << " helper_cluster=" << req.helper_cluster_id
              << " main_proxy=" << req.main_proxy_ip << ":" << req.main_proxy_port
              << " bytes_total=" << req.bytes_total
              << " cross_cluster_time=" << resp.cross_cluster_time;
        write_help_cross_trace(trace);
      }
    } catch (const std::exception& e) {
      resp.success = false;
      resp.err_msg = e.what();
    }
    resp.phase_spans = std::move(phase_spans);
    resp.phase_total_time = now_phase_sec();
    return resp;
  }
}
