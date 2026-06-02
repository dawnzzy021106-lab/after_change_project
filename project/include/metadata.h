#pragma once

#include "ec/utils.h"
#include "ec/erasure_code.h"
#include "ec/rs.h"
#include "ec/lrc.h"
#include "ec/pc.h"

#define LOG_TO_FILE true
#define IF_SIMULATION false                 //
#define IF_SIMULATE_CROSS_CLUSTER false     //
#define IF_TEST_TRHROUGHPUT false           //
#define IF_DEBUG true                      
#define IF_DIRECT_FROM_NODE true            // proxy can directly access data from nodes in other clusters
#define SOCKET_PORT_OFFSET 500
#define STORAGE_SERVER_OFFSET 1000
// #define IN_MEMORY true
// #define MEMCACHED true
// #define REDIS true
#define COORDINATOR_PORT 12121
#define CLIENT_PORT 21212

namespace ECProject
{
  enum MultiStripePR
  {
    RAND,
    DISPERSED,
    AGGREGATED,
    HORIZONTAL,
    VERTICAL
  };

  struct ECSchema
  {
    bool partial_decoding;
    bool partial_scheme;
    bool repair_priority;
    bool repair_method;
    ECTYPE ec_type;
    ErasureCode *ec = nullptr;
    PlacementRule placement_rule;
    MultiStripePR multistripe_placement_rule;
    int x;
    size_t block_size;  // bytes
    int repair_stripe_num; // new parameter
    /** help_repair RPC 全局并发上限（三种模式分别控制） */
    int flow_repair_parallel_unordered = 25;
    int flow_repair_parallel_ordered = 8;
    int flow_repair_parallel_join_ordered = 4;

    ~ECSchema()
    {
      if (ec!= nullptr) {
        delete ec;
      }
    }

    void set_ec(ErasureCode *new_ec) {
      if (ec != nullptr) {
        delete ec;
        ec = nullptr;
      }
      ec = new_ec;
    }
  };

  struct ObjectInfo
  {
    size_t value_len;
    std::vector<unsigned int> stripes;
  };
  
  struct Stripe
  {
    unsigned int stripe_id;
    ErasureCode *ec = nullptr;
    size_t block_size;
    // data blocks, local parity blocks, global parity blocks in order
    std::vector<unsigned int> blocks2nodes;
    std::vector<unsigned int> block_ids;
    std::vector<std::string> objects;   // in order with data blocks

    ~Stripe()
    {
      if (ec != nullptr) {
        delete ec;
        ec = nullptr;
      }
    }
  };
  
  struct Node
  {
    unsigned int node_id;
    std::string node_ip;
    int node_port;
    unsigned int map2cluster;
  };

  struct Cluster
  {
    unsigned int cluster_id;
    std::string proxy_ip;
    int proxy_port;
    std::vector<unsigned int> nodes;
    std::unordered_set<unsigned int> holding_stripe_ids;  
  };
  
  struct ParametersInfo
  {
    bool partial_decoding;
    bool partial_scheme;
    bool repair_priority;
    bool repair_method;
    ECTYPE ec_type;
    PlacementRule placement_rule;
    MultiStripePR multistripe_placement_rule;
    CodingParameters cp;
    size_t block_size;
    int repair_stripe_num; // new parameter
    /** help_repair RPC 全局并发度回退值（当三种模式值未单独配置时使用） */
    int flow_repair_max_parallel = 0;
    /** run_client 三种 flow repair 模式的并行度（可在 config.ini 分别设置） */
    int flow_repair_parallel_unordered = 25;
    int flow_repair_parallel_ordered = 8;
    int flow_repair_parallel_join_ordered = 4;
  };
  
  struct PlacementInfo
  {
    bool isvertical = false;
    bool merged_flag = false;
    ECTYPE ec_type;
    size_t value_len;
    size_t block_size;
    std::string key;
    std::string client_ip;
    int client_port;
    CodingParameters cp;
    std::vector<unsigned int> stripe_ids;
    std::vector<int> offsets;
    std::vector<int> seri_nums;
    std::vector<unsigned int> block_ids;
    std::vector<std::pair<unsigned int, std::pair<std::string, int>>> datanode_ip_port;
    
  };

  struct DeletePlan
  {
    std::vector<unsigned int> block_ids;
    std::vector<std::pair<std::string, int>> blocks_info;
  };
    
  struct MainRepairPlan
  {
    bool partial_decoding;
    bool partial_scheme;
    bool isvertical = false;
    ECTYPE ec_type;
    CodingParameters cp;
    unsigned int cluster_id;
    size_t block_size;
    int data_port = 0;  // 0=use shared acceptor; >0=dedicated ephemeral port
    std::vector<int> help_clusters_partial_less;
    std::vector<int> parity_blocks_index;
    std::vector<int> failed_blocks_index;
    std::vector<int> live_blocks_index;
    std::vector<std::vector<std::pair<int, std::pair<std::string, int>>>> 
        help_clusters_blocks_info;
    std::vector<std::vector<unsigned int>> help_clusters_block_ids;
    std::vector<std::pair<int, std::pair<std::string, int>>> 
        inner_cluster_help_blocks_info;
    std::vector<unsigned int> inner_cluster_help_block_ids;
    std::vector<std::pair<unsigned int, std::pair<std::string, int>>> new_locations;
    std::vector<unsigned int> failed_block_ids;
  };

  struct HelpRepairPlan
  {
    bool partial_decoding;
    bool partial_scheme;
    bool isvertical = false;
    bool partial_less = false;
    ECTYPE ec_type;
    CodingParameters cp;
    unsigned int cluster_id;
    size_t block_size;
    int main_proxy_port;
    std::string main_proxy_ip;
    std::vector<int> parity_blocks_index;
    std::vector<int> failed_blocks_index;
    std::vector<int> live_blocks_index;
    // the order of blocks index must be consistent with that in main_repair_plan
    std::vector<std::pair<int, std::pair<std::string, int>>> 
        inner_cluster_help_blocks_info;
    std::vector<unsigned int> inner_cluster_help_block_ids;
  };

  struct HelpRepairPrepareReq
  {
    HelpRepairPlan repair_plan;
    std::string task_id;
  };

  struct HelpRepairSendReq
  {
    std::string task_id;
  };

  struct HelpCrossWarmupReq
  {
    std::string task_id;
    std::string main_proxy_ip;
    int main_proxy_port = 0;
    size_t bytes_total = 0;
    unsigned int helper_cluster_id = 0;
    unsigned int main_cluster_id = 0;
  };

  struct LocationInfo
  {
    unsigned int cluster_id;
    int proxy_port;
    std::string proxy_ip;
    std::vector<std::pair<int, std::pair<std::string, int>>> blocks_info;
    std::vector<unsigned int> block_ids;
  };
  
  struct MainRecalPlan
  {
    bool partial_decoding;
    bool isvertical = false;
    ECTYPE ec_type;
    CodingParameters cp;
    unsigned int cluster_id;
    size_t block_size;
    std::vector<unsigned int> new_parity_block_ids;
    std::vector<LocationInfo> help_clusters_info;
    std::vector<std::pair<int, std::pair<std::string, int>>> 
        inner_cluster_help_blocks_info;
    std::vector<unsigned int> inner_cluster_help_block_ids;
    std::vector<std::pair<int, std::pair<std::string, int>>> new_locations;
  };

  struct HelpRecalPlan
  {
    bool partial_decoding;
    bool isvertical = false;
    ECTYPE ec_type;
    CodingParameters cp;
    size_t block_size;
    int main_proxy_port;
    std::string main_proxy_ip;
    std::vector<int> new_parity_block_idxs;
    std::vector<std::pair<int, std::pair<std::string, int>>> 
        inner_cluster_help_blocks_info;
    std::vector<unsigned int> inner_cluster_help_block_ids;
  };

  struct RelocatePlan
  {
    size_t block_size;
    std::vector<unsigned int> blocks_to_move;
    std::vector<std::pair<unsigned int, std::pair<std::string, int>>> src_nodes;
    std::vector<std::pair<unsigned int, std::pair<std::string, int>>> des_nodes;
  };
  
  // response
  struct SetResp
  {
    std::string proxy_ip;
    int proxy_port;
  };

  struct RepairResp
  {
    double decoding_time;
    double cross_cluster_time;
    /** Proxy->DataNode 局域网传输耗时（按关键路径聚合） */
    double inner_network_time;
    /** DataNode 读写相关耗时（主/辅 proxy 汇总） */
    double io_time;
    double repair_time;
    double meta_time;
    int cross_cluster_transfers;
    int io_cnt;
    bool success;
  };

  struct MergeResp
  {
    double computing_time;
    double cross_cluster_time;
    double merging_time;
    double meta_time;
    int cross_cluster_transfers;
    int io_cnt;
  };

  enum class RepairPhaseType
  {
    RPC = 0,
    MAIN_READ_INTRA = 1,
    MAIN_RECV_CROSS = 2,
    MAIN_DECODE = 3,
    MAIN_WRITEBACK = 4,
    HELP_READ_INTRA = 5,
    HELP_ENCODE = 6,
    HELP_SEND_CROSS = 7,
    HELP_WARMUP_CROSS = 8
  };

  struct RepairPhaseSpan
  {
    int phase = 0;
    double start_sec = 0.0;
    double end_sec = 0.0;
  };

  struct MainRepairResp
  {
    double decoding_time;
    double cross_cluster_time;
    double io_time;
    double inner_network_time;
    std::vector<RepairPhaseSpan> phase_spans;
    double phase_total_time = 0.0;
  };

  /** Helper Proxy 的纯时间指标（用于论文评估口径对齐） */
  struct HelpRepairResp
  {
    /** Helper 侧 CPU 编码/处理时间（若无则为 0） */
    double decoding_time;
    /** Helper->Main 的跨集群纯网络传输时间（不含 Coordinator RPC 阻塞） */
    double cross_cluster_time;
    /** Helper 侧 DataNode 读取耗时 */
    double io_time;
    /** Helper 侧 Proxy<->DataNode 局域网传输耗时（总拉取耗时-磁盘耗时） */
    double inner_network_time;
    std::vector<RepairPhaseSpan> phase_spans;
    double phase_total_time = 0.0;
  };

  struct HelpRepairPrepareResp
  {
    bool success = true;
    std::string err_msg;
    double decoding_time = 0.0;
    double io_time = 0.0;
    double inner_network_time = 0.0;
    std::vector<RepairPhaseSpan> phase_spans;
    double phase_total_time = 0.0;
  };

  struct HelpRepairSendResp
  {
    bool success = true;
    std::string err_msg;
    double cross_cluster_time = 0.0;
    std::vector<RepairPhaseSpan> phase_spans;
    double phase_total_time = 0.0;
  };

  struct HelpCrossWarmupResp
  {
    bool success = true;
    std::string err_msg;
    double cross_cluster_time = 0.0;
    std::vector<RepairPhaseSpan> phase_spans;
    double phase_total_time = 0.0;
  };

  struct MainRecalResp
  {
    double computing_time;
    double cross_cluster_time;
  };

  struct RelocateResp
  {
    double cross_cluster_time;
  };
  
  ECFAMILY check_ec_family(ECTYPE ec_type);
  ErasureCode* ec_factory(ECTYPE ec_type, CodingParameters cp);
  RSCode* rs_factory(ECTYPE ec_type, CodingParameters cp);
  LocallyRepairableCode* lrc_factory(ECTYPE ec_type, CodingParameters cp);
  ProductCode* pc_factory(ECTYPE ec_type, CodingParameters cp);
  ErasureCode* clone_ec(ECTYPE ec_type, ErasureCode* ec);
  void parse_args(ParametersInfo& paras, std::string config_file);
  int stripe_wide_after_merge(ParametersInfo paras, int step_size);
}
