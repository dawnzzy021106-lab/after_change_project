#pragma once

#include "coordinator.h"
#include "metadata.h"
#include <ylt/coro_rpc/coro_rpc_client.hpp>

namespace ECProject {
  class Client {
  public:
    Client(std::string ip, int port, std::string coordinator_ip, int coordinator_port);
    ~Client();

    void set_ec_parameters(ParametersInfo parameters);
    /** 设置流式无序并发修复的批次并行度；n<=0 时按 hardware_concurrency 自动 */
    void set_flow_repair_max_parallel(int n);
    // set
    double set(std::string key, std::string value);
    // get
    std::string get(std::string key);
    // delete
    void delete_stripe(unsigned int stripe_id);
    void delete_all_stripes();
    // repair
    RepairResp nodes_repair(std::vector<unsigned int> failed_node_ids);
    RepairResp nodes_random_repair(std::vector<unsigned int> failed_node_ids);
    RepairResp nodes_flow_repair(std::vector<unsigned int> failed_node_ids);
    RepairResp nodes_flow_unordered_concurrency_repair(
            std::vector<unsigned int> failed_node_ids);
    // 新增：跨机架链路按调度顺序发送的无序并发 flow repair（用于对比拥塞）
    RepairResp nodes_flow_ordered_concurrency_repair(
            std::vector<unsigned int> failed_node_ids);
    // 新增：跨机架链路按调度顺序发送 + 每一轮强制同步 join（用于对比）
    RepairResp nodes_join_flow_ordered_concurrency_repair(
            std::vector<unsigned int> failed_node_ids);
    RepairResp blocks_repair(std::vector<unsigned int> failed_block_ids, int stripe_id);
    // merge
    MergeResp merge(int step_size);
    // others
    std::vector<unsigned int> list_stripes();

    // 新增：让客户端可以向 Coordinator 发起快照和回滚的请求
    void snapshot_metadata();
    void revert_metadata();

  private:
    std::unique_ptr<coro_rpc::coro_rpc_client> rpc_coordinator_{nullptr};
    int port_;
    std::string ip_;
    std::string coordinator_ip_;
    int coordinator_port_;
    asio::io_context io_context_{};
    asio::ip::tcp::acceptor acceptor_;
  };
};
