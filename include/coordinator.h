#pragma once

#include "tinyxml2.h"
#include "metadata.h"
#include "proxy.h"
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>

namespace ECProject
{
  class Coordinator
  {
  public:
    Coordinator(std::string ip, int port, std::string xml_path);
    ~Coordinator();

    void run();
    // rpc调用, coordinator.cpp
    std::string checkalive(std::string msg);
    // set parameters
    void set_erasure_coding_parameters(ParametersInfo paras);
    /** 覆盖流式无序并发修复的批次并行度；n<=0 表示按 hardware_concurrency 自动 */
    void set_flow_repair_max_parallel(int n);
    // set, return proxy's ip and port
    SetResp request_set(std::vector<std::pair<std::string, size_t>> objects);
    void commit_object(std::vector<std::string> keys, bool commit);
    // get, return size of value
    size_t request_get(std::string key, std::string client_ip, int client_port);
    // delete
    void request_delete_by_stripe(std::vector<unsigned int> stripe_ids);
    // repair, repair a list of blocks in specified stripes (stripe_id>=0) or nodes (stripe_id=-1)
    RepairResp request_repair(std::vector<unsigned int> failed_ids, int stripe_id);
    RepairResp request_random_repair(std::vector<unsigned int> failed_ids, int stripe_id);
    RepairResp request_flow_repair(std::vector<unsigned int> failed_ids, int stripe_id);
    RepairResp request_flow_unordered_concurrency_repair(
            std::vector<unsigned int> failed_ids, int stripe_id);
    // 新增：跨机架链路按调度顺序发送的无序并发 flow repair（用于对比拥塞）
    RepairResp request_flow_ordered_concurrency_repair(
            std::vector<unsigned int> failed_ids, int stripe_id);
    // 新增：跨机架链路按调度顺序发送 + 每一轮强制同步 join（用于对比）
    RepairResp request_join_flow_ordered_concurrency_repair(
            std::vector<unsigned int> failed_ids, int stripe_id);
    // merge
    MergeResp request_merge(int step_size);

    // others
    std::vector<unsigned int> list_stripes();
    // aux.cpp
    void init_ec_schema(std::string config_file);

    // 新增：元数据快照与回滚 RPC 接口
    void request_snapshot_metadata();
    void request_revert_metadata();

    private:
    // aux.cpp
    void init_cluster_info();
    void init_proxy_info();
    void reset_metadata();
    Stripe& new_stripe(size_t block_size, ErasureCode *ec);
    ErasureCode* new_ec_for_merge(int step_size);
    void find_out_stripe_partitions(unsigned int stripe_id);
    void init_placement_info(PlacementInfo &placement, std::string key,
                             size_t value_len, size_t block_size);
    bool if_subject_to_fault_tolerance_lrc(
            ErasureCode *ec, std::vector<int> blocks_in_cluster,
            std::unordered_map<int, std::vector<int>> &group_blocks);
    bool if_subject_to_fault_tolerance_pc(
            ErasureCode *ec, std::vector<int> blocks_in_cluster,
            std::unordered_map<int, std::vector<int>> &col_blocks);

    // placement.cpp
    // placement: partition -> place, a partition in a seperate region(cluster)
    void generate_placement(unsigned int stripe_id);
    // node selection
    void select_nodes_by_random(std::vector<unsigned int>& free_clusters,
                                unsigned int stripe_id, int split_idx);
    void select_nodes_in_order(unsigned int stripe_id);
    void print_placement_result(std::string msg);

    // repair
    void check_out_failures(
            int stripe_id, std::vector<unsigned int> failed_ids,
            std::unordered_map<unsigned int, std::vector<int>>& failure_map);
    bool concrete_repair_plans(int stripe_id,
                               std::vector<RepairPlan>& repair_plans,
                               std::vector<MainRepairPlan>& main_repairs,
                               std::vector<std::vector<HelpRepairPlan>>& help_repairs);
    bool concrete_flow_repair_plans(int stripe_id,
                               std::vector<RepairPlan>& repair_plans,
                               std::vector<MainRepairPlan>& main_repairs,
                               std::vector<std::vector<HelpRepairPlan>>& help_repairs,
                               unsigned int flow_main_cluster_id,
                               int data_port);
    bool concrete_repair_plans_pc(int stripe_id,
                                  std::vector<RepairPlan>& repair_plans,
                                  std::vector<MainRepairPlan>& main_repairs,
                                  std::vector<std::vector<HelpRepairPlan>>& help_repairs);
    void do_repair(std::vector<unsigned int> failed_ids, int stripe_id,
                   RepairResp& response);
    void do_random_repair(std::vector<unsigned int> failed_ids, int stripe_id,
                   RepairResp& response);
    void do_flow_repair(std::vector<unsigned int> failed_ids, int stripe_id,
                   RepairResp& response);
    void do_flow_unordered_concurrency_repair(std::vector<unsigned int> failed_ids,
                   int stripe_id, RepairResp& response);
    void do_flow_ordered_concurrency_repair(std::vector<unsigned int> failed_ids,
                   int stripe_id, RepairResp& response);
    void do_join_flow_ordered_concurrency_repair(std::vector<unsigned int> failed_ids,
                   int stripe_id, RepairResp& response);
    /** main/help 客户端与互斥量的统一键：ip:port，避免 IP 末段与端口数字粘连 */
    static std::string proxy_endpoint_key(const std::string& proxy_ip, int proxy_port);

    // parallel_stripes=true：条带全并发，help_repair RPC 由全局 FifoSemaphore 控制并发度
    void do_flow_repair_common(std::vector<unsigned int> failed_ids, int stripe_id,
                   RepairResp& response, bool unordered_concurrency_main_repairs,
                   bool parallel_stripes, bool schedule_cross_rack_links,
                   bool schedule_join_per_round);
    void simulation_repair(std::vector<MainRepairPlan>& main_repair,
                           int& cross_cluster_transfers, int& io_cnt);
    bool loadRepairData(const std::string& filename,
                     std::vector<int>& main_help_clusterID,
                     std::vector<std::vector<std::pair<int, int>>>& other_help_clusterID_chunkNum_pairs);

    // merge
    void do_stripe_merge(MergeResp& response, int step_size);
    void rs_merge(MergeResp& response, int step_size);
    void azu_lrc_merge(MergeResp& response, int step_size);
    // void lrc_merge(MergeResp& response, int step_size);
    void pc_merge(MergeResp& response, int step_size);
    void simulation_recalculation(MainRecalPlan& main_plan,
            int& cross_cluster_transfers, int& io_cnt);

    std::unique_ptr<coro_rpc::coro_rpc_server> rpc_server_{nullptr};
    /** 主通道：main_repair、merge 主路径、SET/GET/DELETE 等非 help 独占 RPC */
    std::unordered_map<std::string, std::unique_ptr<coro_rpc::coro_rpc_client>> main_proxies_;
    /** 协作通道：help_repair / help_recal，与主通道套接字隔离，避免主 RPC 阻塞时无法下发 help */
    std::unordered_map<std::string, std::unique_ptr<coro_rpc::coro_rpc_client>> help_proxies_;
    /** join-ordered overlap 专用：机架内读取/编码预取通道，避免与跨机架发送 RPC 互斥 */
    std::unordered_map<std::string, std::unique_ptr<coro_rpc::coro_rpc_client>> help_prepare_proxies_;
    /** join-ordered overlap 专用：发送已预取数据通道，避免占用下一轮预取 */
    std::unordered_map<std::string, std::unique_ptr<coro_rpc::coro_rpc_client>> help_send_proxies_;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> main_proxy_mutexes_;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> help_proxy_mutexes_;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> help_prepare_proxy_mutexes_;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> help_send_proxy_mutexes_;
    ECSchema ec_schema_;
    std::unordered_map<unsigned int, Cluster> cluster_table_;
    std::unordered_map<unsigned int, Node> node_table_;
    std::unordered_map<unsigned int, Stripe> stripe_table_;
    std::unordered_map<std::string, ObjectInfo> commited_object_table_;
    std::unordered_map<std::string, ObjectInfo> updating_object_table_;

    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned int cur_stripe_id_;
    int num_of_clusters_;
    int num_of_nodes_per_cluster_;
    std::string ip_;
    int port_;
    std::string xml_path_;
    double time_;
    unsigned int cur_block_id_;
    unsigned int lucky_cid_;
    std::vector<std::vector<unsigned int>> merge_groups_;
    std::vector<unsigned int> free_clusters_;
    bool merged_flag_ = false;

    // 新增：专门用于对比测试的初始块放置备份
    std::unordered_map<unsigned int, std::vector<unsigned int>> initial_placement_;
  };
}
