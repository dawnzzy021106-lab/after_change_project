#include "coordinator.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace ECProject
{
  namespace
  {
    // 参考：@f:\ec\multi_stripe_repair\repair_cpp\scheduleRepairLink.cpp
    // 用二分图匹配将跨机架链路分解为多轮“匹配”（每轮同一 src/dst 只出现一次），用于降低拥塞。
    struct ScheduledLink
    {
      int src;   // helper cluster (1-based)
      int dst;   // main cluster (1-based)
      int weight; // 本轮该 (src,dst) 需要发送的任务数（离散 help_repair 次数）
      bool is_dummy;
    };

    struct ScheduleRound
    {
      std::vector<ScheduledLink> edges;
      int weight;
    };

    class RepairScheduler
    {
    private:
      int max_nodes_ = 0;

      bool dfs(int u, const std::vector<std::vector<int>>& adj,
               std::vector<int>& match, std::vector<bool>& vis)
      {
        for (int v : adj[u]) {
          if (vis[v]) continue;
          vis[v] = true;
          if (match[v] < 0 || dfs(match[v], adj, match, vis)) {
            match[v] = u;
            return true;
          }
        }
        return false;
      }

    public:
      std::vector<ScheduleRound> compute_schedule(std::vector<ScheduledLink> physical_links)
      {
        std::map<int, int> out_degree;
        std::map<int, int> in_degree;
        max_nodes_ = 0;

        for (const auto& link : physical_links) {
          out_degree[link.src] += link.weight;
          in_degree[link.dst] += link.weight;
          max_nodes_ = std::max(max_nodes_, std::max(link.src, link.dst));
        }

        int max_deg = 0;
        for (int i = 1; i <= max_nodes_; ++i) {
          max_deg = std::max(max_deg, std::max(out_degree[i], in_degree[i]));
        }

        std::vector<ScheduledLink> all_links = physical_links;
        for (int i = 1; i <= max_nodes_; ++i) {
          while (out_degree[i] < max_deg) {
            for (int j = 1; j <= max_nodes_; ++j) {
              if (in_degree[j] < max_deg) {
                int add_weight = std::min(max_deg - out_degree[i], max_deg - in_degree[j]);
                all_links.push_back({i, j, add_weight, true});
                out_degree[i] += add_weight;
                in_degree[j] += add_weight;
                break;
              }
            }
          }
        }

        std::vector<ScheduleRound> schedule;
        while (true) {
          bool all_zero = true;
          for (const auto& link : all_links) {
            if (link.weight > 0) { all_zero = false; break; }
          }
          if (all_zero) break;

          std::vector<std::vector<int>> adj(max_nodes_ + 1);
          for (size_t k = 0; k < all_links.size(); ++k) {
            if (all_links[k].weight > 0) {
              adj[all_links[k].src].push_back(all_links[k].dst);
            }
          }

          std::vector<int> match(max_nodes_ + 1, -1);
          for (int i = 1; i <= max_nodes_; ++i) {
            std::vector<bool> vis(max_nodes_ + 1, false);
            dfs(i, adj, match, vis);
          }

          std::vector<int> matched_link_indices;
          for (int v = 1; v <= max_nodes_; ++v) {
            int u = match[v];
            if (u != -1) {
              for (size_t k = 0; k < all_links.size(); ++k) {
                if (all_links[k].src == u && all_links[k].dst == v && all_links[k].weight > 0) {
                  matched_link_indices.push_back((int)k);
                  break;
                }
              }
            }
          }

          const int dispatch_weight = 1;
          ScheduleRound round;
          round.weight = dispatch_weight;
          for (int idx : matched_link_indices) {
            all_links[idx].weight -= dispatch_weight;
            if (!all_links[idx].is_dummy) {
              round.edges.push_back(
                  {all_links[idx].src, all_links[idx].dst, dispatch_weight, false});
            }
          }
          if (!round.edges.empty()) {
            schedule.push_back(round);
          }
        }
        return schedule;
      }
    };

    // 每线程独立的 proxy rpc client，避免”长 RPC 持锁”导致端点级互锁卡顿。
    // 每次 do_flow_repair_common 结束时递增 generation，让存活的线程在下次调用时
    // 主动关闭旧连接并重建，避免跨模式 TLS 连接累积导致 bind 错误。
    static std::atomic<int> tls_client_generation_{0};

    // RAII 守卫：无论 do_flow_repair_common 如何退出（正常返回 / 提前 return / 异常），
    // 都会递增 generation，确保下次调用时所有线程的 TLS client 连接被刷新。
    std::shared_ptr<void> tls_gen_guard_(
        nullptr, [](void*) { tls_client_generation_.fetch_add(1, std::memory_order_release); });

    static coro_rpc::coro_rpc_client& tls_proxy_client(
        const std::string& proxy_key,
        const std::string& proxy_ip,
        int proxy_port,
        bool force_reconnect = false)
    {
      thread_local std::unordered_map<std::string,
          std::unique_ptr<coro_rpc::coro_rpc_client>> clients;
      thread_local int tls_my_gen = -1;
      if (tls_my_gen != tls_client_generation_.load(std::memory_order_acquire)) {
        clients.clear();
        tls_my_gen = tls_client_generation_.load(std::memory_order_acquire);
      }
      if (force_reconnect) {
        clients.erase(proxy_key);
      }
      auto& slot = clients[proxy_key];
      if (!slot) {
        slot = std::make_unique<coro_rpc::coro_rpc_client>();
        async_simple::coro::syncAwait(
            slot->connect(proxy_ip, std::to_string(proxy_port)));
      }
      return *slot;
    }

    struct TimelineEvent
    {
      int lane = 0;
      double start_sec = 0.0;
      double end_sec = 0.0;
      std::string color;
      std::string label;
      // 细粒度分解：用于在同一时间条中显示组成部分（单位：秒）
      double decode_sec = 0.0;
      double cross_sec = 0.0;
      double inner_sec = 0.0;
      double io_sec = 0.0;
      double meta_sec = 0.0;
      std::vector<RepairPhaseSpan> phase_spans;
      double phase_total_sec = 0.0;
    };

    // FIFO 信号量：保证按 acquire 调用顺序（即线程创建顺序）唤醒，用于 RPC 并发度控制
    class FifoSemaphore
    {
    public:
      explicit FifoSemaphore(int count) : count_(count) {}
      void acquire()
      {
        std::unique_lock<std::mutex> lock(mtx_);
        size_t ticket = next_ticket_++;
        wait_queue_.push(ticket);
        bool would_block = (count_ <= 0 || wait_queue_.front() != ticket);
        if (would_block) {
          std::cerr << "[SEM] BLOCK ticket=" << ticket
                    << " count=" << count_
                    << " queue=" << wait_queue_.size()
                    << " tid=" << std::this_thread::get_id() << std::endl;
        }
        cv_.wait(lock, [&]() { return count_ > 0 && wait_queue_.front() == ticket; });
        wait_queue_.pop();
        --count_;
        std::cerr << "[SEM] ACQ ticket=" << ticket
                  << " count_after=" << count_
                  << " tid=" << std::this_thread::get_id() << std::endl;
      }
      void release()
      {
        {
          std::lock_guard<std::mutex> lock(mtx_);
          ++count_;
        }
        std::cerr << "[SEM] REL count=" << count_
                  << " tid=" << std::this_thread::get_id() << std::endl;
        cv_.notify_all();
      }

    private:
      std::mutex mtx_;
      std::condition_variable cv_;
      int count_;
      std::queue<size_t> wait_queue_;
      size_t next_ticket_ = 0;
    };

    std::string repair_output_dir()
    {
      char buf[1024] = {0};
      if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return ".";
      }
      std::string cwd(buf);
      if (cwd.size() >= 6 && cwd.substr(cwd.size() - 6) == "/build") {
        return cwd.substr(0, cwd.size() - 6);
      }
      return cwd;
    }

    void write_timeline_svg(const std::string& path, const std::string& title,
                            const std::vector<std::string>& lane_labels,
                            const std::vector<TimelineEvent>& events,
                            const std::vector<std::string>& notes = {})
    {
      if (lane_labels.empty()) return;
      double max_end = 1.0;
      for (const auto& e : events) max_end = std::max(max_end, e.end_sec);
      const int lane_h = 24;
      const int notes_h = notes.empty() ? 0 : (18 + static_cast<int>(notes.size()) * 14);
      const int top = 60 + notes_h;
      const int left = 180;
      const int width = 1600;
      const int height = top + static_cast<int>(lane_labels.size()) * lane_h + 50;
      const double scale = (width - left - 20) / max_end;

      // 先把全部内容写入内存缓冲，再一次性写入文件。
      // 这样即使进程在写入过程中被强制中断，也不会产生半截的 SVG 文件（不会出现
      // "AttValue: ' or ' expected" 等 XML 解析错误）。
      std::ostringstream out;
      out << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width
          << "' height='" << height << "'>\n";
      out << "<rect width='100%' height='100%' fill='white'/>\n";
      out << "<text x='10' y='22' font-size='16' font-family='monospace'>"
          << title << "</text>\n";
      out << "<text x='10' y='42' font-size='12' font-family='monospace'>unit: seconds</text>\n";
      if (!notes.empty()) {
        int ny = 58;
        out << "<text x='10' y='" << ny
            << "' font-size='12' font-family='monospace' fill='#333'>compute_schedule:</text>\n";
        ny += 14;
        for (const auto& line : notes) {
          out << "<text x='18' y='" << ny
              << "' font-size='11' font-family='monospace' fill='#333'>"
              << line << "</text>\n";
          ny += 14;
        }
      }
      for (size_t i = 0; i < lane_labels.size(); ++i) {
        int y = top + static_cast<int>(i) * lane_h;
        out << "<line x1='" << left << "' y1='" << y + lane_h - 4
            << "' x2='" << width - 20 << "' y2='" << y + lane_h - 4
            << "' stroke='#efefef' stroke-width='1'/>\n";
        out << "<text x='8' y='" << y + 15 << "' font-size='11' font-family='monospace'>"
            << lane_labels[i] << "</text>\n";
      }
      for (const auto& e : events) {
        if (e.lane < 0 || e.lane >= static_cast<int>(lane_labels.size())) continue;
        const int y = top + e.lane * lane_h + 4;
        const double x = left + e.start_sec * scale;
        const double w = std::max(1.5, (e.end_sec - e.start_sec) * scale);
        out << "<rect x='" << std::fixed << std::setprecision(2) << x
            << "' y='" << y << "' width='" << w << "' height='14' fill='"
            << e.color << "' opacity='0.85'>"
            << "<title>" << e.label << " [" << (e.end_sec - e.start_sec) << "s]</title>"
            << "</rect>\n";
        auto phase_fill = [](int phase) -> std::string {
          switch (phase) {
            case static_cast<int>(RepairPhaseType::RPC): return "#85C1E9";
            case static_cast<int>(RepairPhaseType::MAIN_READ_INTRA): return "#A9CCE3";
            case static_cast<int>(RepairPhaseType::MAIN_RECV_CROSS): return "#2E86DE";
            case static_cast<int>(RepairPhaseType::MAIN_DECODE): return "#F5B041";
            case static_cast<int>(RepairPhaseType::MAIN_WRITEBACK): return "#AF7AC5";
            case static_cast<int>(RepairPhaseType::HELP_READ_INTRA): return "#D6EAF8";
            case static_cast<int>(RepairPhaseType::HELP_ENCODE): return "#F8C471";
            case static_cast<int>(RepairPhaseType::HELP_SEND_CROSS): return "#5DADE2";
            case static_cast<int>(RepairPhaseType::HELP_WARMUP_CROSS): return "#7FB3D5";
            default: return "#95A5A6";
          }
        };
        auto phase_label = [](int phase) -> std::string {
          switch (phase) {
            case static_cast<int>(RepairPhaseType::RPC): return "RPC";
            case static_cast<int>(RepairPhaseType::MAIN_READ_INTRA): return "Main Intra Read";
            case static_cast<int>(RepairPhaseType::MAIN_RECV_CROSS): return "Main Cross Receive";
            case static_cast<int>(RepairPhaseType::MAIN_DECODE): return "Main Decode";
            case static_cast<int>(RepairPhaseType::MAIN_WRITEBACK): return "Main Write Back";
            case static_cast<int>(RepairPhaseType::HELP_READ_INTRA): return "Help Intra Read";
            case static_cast<int>(RepairPhaseType::HELP_ENCODE): return "Help Partial Encode";
            case static_cast<int>(RepairPhaseType::HELP_SEND_CROSS): return "Help Cross Send";
            case static_cast<int>(RepairPhaseType::HELP_WARMUP_CROSS): return "Round Warmup Cross Send";
            default: return "Phase";
          }
        };
        if (!e.phase_spans.empty() && e.phase_total_sec > 1e-9) {
          double acc_phase_time = 0.0;
          for (const auto& ps : e.phase_spans) {
            const double s = std::max(0.0, std::min(ps.start_sec, e.phase_total_sec));
            const double t = std::max(s, std::min(ps.end_sec, e.phase_total_sec));
            if (t - s <= 1e-9) continue;
            const double phase_duration = t - s;
            const double seg_start = e.start_sec + acc_phase_time;
            const double px = left + seg_start * scale;
            const double pw = std::max(0.8, phase_duration * scale);
            out << "<rect x='" << std::fixed << std::setprecision(2) << px
                << "' y='" << y << "' width='" << pw << "' height='14' fill='"
                << phase_fill(ps.phase) << "' opacity='0.92'>"
                << "<title>" << e.label << " / " << phase_label(ps.phase)
                << ": " << phase_duration << "s</title></rect>\n";
            acc_phase_time += phase_duration;
          }
        } else {
          // 回退：无阶段打点数据时，按聚合时长顺序绘制
          const double wall = std::max(0.0, e.end_sec - e.start_sec);
          const double sum_parts = std::max(
              0.0, e.decode_sec + e.cross_sec + e.inner_sec + e.io_sec + e.meta_sec);
          if (wall > 1e-9 && sum_parts > 1e-9) {
            const double clip_ratio = std::min(1.0, wall / sum_parts);
            const double dw = e.decode_sec * clip_ratio * scale;
            const double cw = e.cross_sec * clip_ratio * scale;
            const double iw = e.inner_sec * clip_ratio * scale;
            const double iow = e.io_sec * clip_ratio * scale;
            const double mw = e.meta_sec * clip_ratio * scale;
            auto draw_part = [&](double px, double pw, const std::string& fill,
                                 double opacity, const std::string& part_label,
                                 double part_sec) {
              if (pw < 0.8) return;
              out << "<rect x='" << std::fixed << std::setprecision(2) << px
                  << "' y='" << y << "' width='" << pw << "' height='14' fill='" << fill
                  << "' opacity='" << opacity << "'>"
                  << "<title>" << e.label << " / " << part_label << ": " << part_sec
                  << "s</title></rect>\n";
            };
            double curx = x;
            draw_part(curx, dw, "#F5B041", 0.88, "Decoding", e.decode_sec);
            curx += dw;
            draw_part(curx, cw, "#2E86DE", 0.92, "Network(Cross Rack)", e.cross_sec);
            curx += cw;
            draw_part(curx, iw, "#85C1E9", 0.92, "Network(Intra Rack)", e.inner_sec);
            curx += iw;
            draw_part(curx, iow, "#AF7AC5", 0.85, "Disk IO", e.io_sec);
            curx += iow;
            draw_part(curx, mw, "#95A5A6", 0.80, "Meta", e.meta_sec);
          }
        }
        if (w >= 80.0) {
          out << "<text x='" << (x + 3.0) << "' y='" << (y + 11)
              << "' font-size='9' font-family='monospace' fill='#111'>"
              << e.label << "</text>\n";
        }
      }
      for (int tick = 0; tick <= 10; ++tick) {
        const double t = max_end * tick / 10.0;
        const double x = left + t * scale;
        out << "<line x1='" << x << "' y1='50' x2='" << x
            << "' y2='" << height - 20 << "' stroke='#ddd' stroke-width='1'/>\n";
        out << "<text x='" << x + 2 << "' y='48' font-size='10' font-family='monospace'>"
            << std::fixed << std::setprecision(2) << t << "</text>\n";
      }
      out << "</svg>\n";
      // 先写临时文件，再原子 rename，避免进程被中断时产生半截 SVG。
      const std::string tmp_path = path + ".tmp";
      {
        std::ofstream file(tmp_path);
        if (file.is_open()) {
          file << out.str();
          file.close();
        }
      }
      std::remove(path.c_str());
      std::rename(tmp_path.c_str(), path.c_str());
    }
  } // namespace

  void Coordinator::do_repair(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response)
  {
    struct timeval e2e_start_time{}, e2e_end_time{};
    gettimeofday(&e2e_start_time, NULL);
    struct timeval start_time, end_time;
    struct timeval m_start_time, m_end_time;
    double repair_time = 0;
    double decoding_time = 0;
    double cross_cluster_time = 0;
    double inner_network_time = 0;
    double io_time = 0;
    double meta_time = 0;
    int cross_cluster_transfers = 0;
    int io_cnt = 0;
    std::unordered_map<unsigned int, std::vector<int>> failures_map;
    // 检测故障范围
    check_out_failures(stripe_id, failed_ids, failures_map);
    
    for (auto& pair : failures_map) {
      gettimeofday(&start_time, NULL);
      gettimeofday(&m_start_time, NULL);
      Stripe& stripe = stripe_table_[pair.first];
      stripe.ec->placement_rule = ec_schema_.placement_rule;
      stripe.ec->generate_partition();
      find_out_stripe_partitions(pair.first);
      if (IF_DEBUG) {
        // std::cout << "Stripe " << stripe.stripe_id << " block placement:\n";
        // for (auto& vec : stripe.ec->partition_plan) {
        //   unsigned int node_id = stripe.blocks2nodes[vec[0]];
        //   unsigned int cluster_id = node_table_[node_id].map2cluster;
        //   std::cout << cluster_id << ": ";
        //   for (int ele : vec) {
        //     std::cout << "B" << ele << "N" << stripe.blocks2nodes[ele] << " ";
        //   }
        //   std::cout << "\n";
        // }
        // std::cout << "Generating repair plan for failures:" << std::endl;
        // for (auto& failure : pair.second) {
        //   std::cout << failure << " ";
        // }
        // std::cout << std::endl;
      }
      std::vector<RepairPlan> repair_plans;
      // 生成修复方案
      bool flag = stripe.ec->generate_repair_plan(pair.second, repair_plans,
                                                  ec_schema_.partial_scheme,
                                                  ec_schema_.repair_priority,
                                                  ec_schema_.repair_method);
      if (!flag) {
        response.success = false;
        return;
      }
      if (IF_DEBUG) {
        // std::cout << "Repair Plan: " << std::endl;
        // for (int i = 0; i < int(repair_plans.size()); i++) {
        //   RepairPlan& tmp = repair_plans[i];
        //   std::cout << "> Failed Blocks: ";
        //   for (int j = 0; 
        //        j < int(tmp.failure_idxs.size()); j++) {
        //     std::cout << tmp.failure_idxs[j] << " ";
        //   }
        //   std::cout << std::endl;
        //   std::cout << "> Repair by Blocks: ";
        //   for (auto& help_blocks : tmp.help_blocks) {
        //     for(auto& block : help_blocks) {
        //       std::cout << block << " ";
        //     }
        //   }
        //   std::cout << std::endl;
        //   std::cout << "> local_or_column: " << tmp.local_or_column << std::endl;
        //   std::cout << "> Parity idx: ";
        //   for (auto& idx : tmp.parity_idxs) {
        //     std::cout << idx << " ";
        //   }
        //   std::cout << std::endl;
        // }
      }
      std::vector<MainRepairPlan> main_repairs;
      std::vector<std::vector<HelpRepairPlan>> help_repairs;
      // 生成具体的修复方案
      if (check_ec_family(ec_schema_.ec_type) == PCs) { // 乘积码
        concrete_repair_plans_pc(pair.first, repair_plans, main_repairs, help_repairs);
      } else { // 通用处理
        concrete_repair_plans(pair.first, repair_plans, main_repairs, help_repairs);
      }
      
      if (IF_DEBUG) {
        std::cout << "Finish generate repair plan." << std::endl;
      }

      const int nm = int(main_repairs.size());
      std::vector<double> per_plan_main_dec(nm, 0.0);
      std::vector<double> per_plan_help_max_dec(nm, 0.0);
      std::vector<double> per_plan_main_net(nm, 0.0);
      std::vector<double> per_plan_help_max_net(nm, 0.0);
      std::vector<double> per_plan_main_inner_net(nm, 0.0);
      std::vector<double> per_plan_help_max_inner_net(nm, 0.0);
      std::vector<double> per_plan_main_io(nm, 0.0);
      std::vector<double> per_plan_help_max_io(nm, 0.0);
      std::mutex per_plan_help_max_mtx;
      std::atomic<bool> rpc_ok{true};

      auto send_main_repair_plan =
          [this, main_repairs, &per_plan_main_dec, &per_plan_main_net,
           &per_plan_main_inner_net, &per_plan_main_io, &rpc_ok](
              int i, int main_cluster_id) mutable
      {
        std::string chosen_proxy = proxy_endpoint_key(
            cluster_table_[main_cluster_id].proxy_ip,
            cluster_table_[main_cluster_id].proxy_port);
        // 向主修复proxy发送修复任务，主修复proxy指故障块最多的集群，负责解码计算
        auto& task_client = tls_proxy_client(
            chosen_proxy,
            cluster_table_[main_cluster_id].proxy_ip,
            cluster_table_[main_cluster_id].proxy_port);
        auto r = async_simple::coro::syncAwait(
            task_client.call_for<&Proxy::main_repair>(
                std::chrono::seconds{500}, main_repairs[i]));
        if (!r) {
          std::cerr << "[RPC ERROR] main_repair failed to proxy " << chosen_proxy
                    << ": " << r.error().msg << std::endl;
          rpc_ok.store(false);
          return;
        }
        auto resp = r.value();
        per_plan_main_dec[static_cast<size_t>(i)] = resp.decoding_time;
        per_plan_main_net[static_cast<size_t>(i)] = resp.cross_cluster_time;
        per_plan_main_inner_net[static_cast<size_t>(i)] = resp.inner_network_time;
        per_plan_main_io[static_cast<size_t>(i)] = resp.io_time;
        if (IF_DEBUG) {
          std::cout << "Selected main proxy " << chosen_proxy << " of cluster"
                    << main_cluster_id << ". Decoding time : "
                    << resp.decoding_time << std::endl;
        }
      };

      auto send_help_repair_plan =
          [this, help_repairs, &per_plan_help_max_dec, &per_plan_help_max_net,
           &per_plan_help_max_inner_net, &per_plan_help_max_io,
           &per_plan_help_max_mtx, &rpc_ok](
              int i, int j, std::string proxy_ip, int proxy_port) mutable
      {
        std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
        auto& hlpc = tls_proxy_client(chosen_proxy, proxy_ip, proxy_port);
        auto r = async_simple::coro::syncAwait(
            hlpc.call_for<&Proxy::help_repair>(
                std::chrono::seconds{500}, help_repairs[i][j]));
        if (!r) {
          std::cerr << "[RPC ERROR] help_repair failed to proxy " << chosen_proxy
                    << ": " << r.error().msg << std::endl;
          rpc_ok.store(false);
          return;
        }
        auto resp = r.value();
        {
          std::lock_guard<std::mutex> lk(per_plan_help_max_mtx);
          per_plan_help_max_dec[static_cast<size_t>(i)] =
              std::max(per_plan_help_max_dec[static_cast<size_t>(i)], resp.decoding_time);
          per_plan_help_max_net[static_cast<size_t>(i)] =
              std::max(per_plan_help_max_net[static_cast<size_t>(i)], resp.cross_cluster_time);
          per_plan_help_max_inner_net[static_cast<size_t>(i)] =
              std::max(per_plan_help_max_inner_net[static_cast<size_t>(i)],
                       resp.inner_network_time);
          per_plan_help_max_io[static_cast<size_t>(i)] =
              std::max(per_plan_help_max_io[static_cast<size_t>(i)], resp.io_time);
        }
        if (IF_DEBUG) {
          std::cout << "Selected help proxy " << chosen_proxy << std::endl;
        }
      };

      // simulation-统计理论上的跨集群传输量、IO次数
      simulation_repair(main_repairs, cross_cluster_transfers, io_cnt);
      if (IF_DEBUG) {
        std::cout << "Finish simulation! " << cross_cluster_transfers << std::endl;
      }
      gettimeofday(&m_end_time, NULL);
      meta_time += m_end_time.tv_sec - m_start_time.tv_sec +
          (m_end_time.tv_usec - m_start_time.tv_usec) * 1.0 / 1000000;

      if (!IF_SIMULATION) {
        for (int i = 0; i < nm; i++) {
          try
          {
            MainRepairPlan& tmp = main_repairs[i];
            int failed_num = int(tmp.failed_blocks_index.size());
            unsigned int main_cluster_id = tmp.cluster_id;
            // 多线程发送修复任务
            std::thread my_main_thread(send_main_repair_plan, i, main_cluster_id);
            std::vector<std::thread> senders;
            int index = 0;
            for (int j = 0; j < int(tmp.help_clusters_blocks_info.size()); j++) {
              int num_of_blocks_in_help_cluster = 
                  (int)tmp.help_clusters_blocks_info[j].size();
              my_assert(num_of_blocks_in_help_cluster == 
                  int(help_repairs[i][j].inner_cluster_help_blocks_info.size()));
              bool t_flag = tmp.help_clusters_partial_less[j];
              if ((IF_DIRECT_FROM_NODE && ec_schema_.partial_decoding
                   && t_flag) ||
                  !IF_DIRECT_FROM_NODE)
              {
                Cluster &cluster = cluster_table_[help_repairs[i][j].cluster_id];
                senders.push_back(std::thread(send_help_repair_plan, i, j,
                                  cluster.proxy_ip, cluster.proxy_port));
              }
            }
            for (int j = 0; j < int(senders.size()); j++) {
              senders[j].join();
            }
            my_main_thread.join();
            if (!rpc_ok.load()) {
              response.success = false;
              return;
            }
            // Pure Network 统一口径：main/help 并行，取两者 max（关键路径）
            const double plan_net =
                std::max(per_plan_main_net[static_cast<size_t>(i)],
                         per_plan_help_max_net[static_cast<size_t>(i)]);
            cross_cluster_time += plan_net;
            const double plan_io =
                std::max(per_plan_main_io[static_cast<size_t>(i)],
                         per_plan_help_max_io[static_cast<size_t>(i)]);
            const double plan_inner =
                std::max(per_plan_main_inner_net[static_cast<size_t>(i)],
                         per_plan_help_max_inner_net[static_cast<size_t>(i)]);
            io_time += plan_io;
            inner_network_time += plan_inner;
            const double plan_dec =
                std::max(per_plan_main_dec[static_cast<size_t>(i)],
                         per_plan_help_max_dec[static_cast<size_t>(i)]);
            decoding_time += plan_dec;
          }
          catch(const std::exception& e)
          {
            std::cerr << e.what() << '\n';
          }
        }
      }
      // 更新元数据
      for (int i = 0; i < int(main_repairs.size()); i++) {
        MainRepairPlan& tmp = main_repairs[i];
        int j = 0;
        for (auto& idx : tmp.failed_blocks_index) {
          stripe.blocks2nodes[idx] = tmp.new_locations[j++].first;
        }
      }
      gettimeofday(&end_time, NULL);
      double temp_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      repair_time += temp_time;

      if (IF_DEBUG) {
        std::cout << "New blocks placement for stripe " << stripe.stripe_id << ":" << std::endl;
        for (int i = 0; i < int(main_repairs.size()); i++) {
          MainRepairPlan& tmp = main_repairs[i];
          int j = 0;
          for (auto& idx : tmp.failed_blocks_index) {
              unsigned int old_node_id = stripe.blocks2nodes[idx]; // 更新前的节点
              unsigned int old_cluster_id = node_table_[old_node_id].map2cluster;
              unsigned int new_node_id = tmp.new_locations[j].first;
              unsigned int new_cluster_id = node_table_[new_node_id].map2cluster;
              std::cout << "  Block " << idx << ": from node " << old_node_id
                        << " (cluster " << old_cluster_id << ") to node " << new_node_id
                        << " (cluster " << new_cluster_id << ")" << std::endl;
              j++;
          }
        }
      }

      std::cout << "Repair[ ";
      for (auto& failure : pair.second) {
        std::cout << failure << " ";
      }
      std::cout << "]: total = " << repair_time << "s, latest = "
                << temp_time << "s. Decode: total = "
                << decoding_time << std::endl;
    }
    response.decoding_time = decoding_time;
    response.cross_cluster_time = cross_cluster_time;
    response.inner_network_time = inner_network_time;
    response.io_time = io_time;
    gettimeofday(&e2e_end_time, NULL);
    response.repair_time = (e2e_end_time.tv_sec - e2e_start_time.tv_sec) +
        (e2e_end_time.tv_usec - e2e_start_time.tv_usec) * 1.0 / 1000000;
    response.meta_time = meta_time;
    response.cross_cluster_transfers = cross_cluster_transfers;
    response.io_cnt = io_cnt;
    response.success = true;
  }

  // check_out_failures：检测故障影响范围
  // stripe_id ≥ 0：修复指定条带的故障块（块故障）
  // stripe_id = -1：修复指定节点上的所有故障块（节点故障）
  void Coordinator::check_out_failures(
          int stripe_id, std::vector<unsigned int> failed_ids,
          std::unordered_map<unsigned int, std::vector<int>> &failures_map)
  {
    if (stripe_id >= 0) {   // block failures
      for (auto id : failed_ids) {
        failures_map[stripe_id].push_back((int)id);
      }
    } else {   // node failures，输出：{条带ID: [该节点在该条带中的块索引列表]}
      int num_of_failed_nodes = int(failed_ids.size());
      int repair_stripe_num = ec_schema_.repair_stripe_num;

      for (int i = 0; i < num_of_failed_nodes; i++) {
        unsigned int node_id = failed_ids[i];
        // 遍历所有条带，找到故障节点上的所有块
        for (auto it = stripe_table_.begin(); it != stripe_table_.end(); it++) {
          int t_stripe_id = it->first;
          auto& stripe = it->second;
          // [修改1] 使用 blocks2nodes 的实际大小作为边界，避免越界访问产生未定义行为
          int n = stripe.blocks2nodes.size();
          // int n = stripe.ec->k + stripe.ec->m;
          // [修改2] 使用 vector 收集所有匹配的块，替代原先只找第一个就 break 的逻辑
          std::vector<int> current_failed_blocks;
          for (int j = 0; j < n; j++) {
            if (stripe.blocks2nodes[j] == node_id) {
              current_failed_blocks.push_back(j);
            }
          }
          if (!current_failed_blocks.empty()) {
            auto map_it = failures_map.find(t_stripe_id);
            if (map_it != failures_map.end()) {
              // 如果已存在该条带，直接追加新的故障块索引
              for (int idx : current_failed_blocks) {
                map_it->second.push_back(idx);
              }
            } else {
              if (failures_map.size() < (int)repair_stripe_num) {
                failures_map[t_stripe_id] = current_failed_blocks;
              } else {
                continue;
              }
            }
          }

          // int failed_block_idx = -1;
          // // 查找该条带中的块是否有在故障节点node_id中的
          // for (int j = 0; j < n; j++) {
          //   if (stripe.blocks2nodes[j] == node_id) {
          //     failed_block_idx = j;
          //     break;
          //   }
          // }
          // if (failed_block_idx != -1) {
          //   auto map_it = failures_map.find(t_stripe_id);
          //   if (map_it != failures_map.end()) {
          //     map_it->second.push_back(failed_block_idx); // 记录故障块索引
          //   } else {
          //     if (failures_map.size() < (int)repair_stripe_num) {
          //       std::vector<int> failed_blocks;
          //       failed_blocks.push_back(failed_block_idx);
          //       failures_map[t_stripe_id] = failed_blocks;
          //     } else {
          //       continue;
          //     }
          //   }
          // }
          // BUG: 少了判断
          // if (failures_map.find(t_stripe_id) != failures_map.end()) {
          //   failures_map[t_stripe_id].push_back(failed_block_idx); // 记录故障块索引
          // } else {
          //   std::vector<int> failed_blocks;
          //   failed_blocks.push_back(failed_block_idx);
          //   failures_map[t_stripe_id] = failed_blocks;
          // }
        }
      }
    }
    // --- Diagnostic Output ---
    std::cout << "[REPAIR] Total failed stripes to repair: " << failures_map.size() << std::endl;
    if (!failures_map.empty()) {
        std::cout << "[REPAIR] Failed Stripe IDs and their failed block indices: " << std::endl;
        for (auto const& [sid, f_indices] : failures_map) {
            std::cout << "  - Stripe ID " << sid << ": [ ";
            for (int idx : f_indices) {
                std::cout << idx << " ";
            }
            std::cout << "]" << std::endl;
        }
    }
    // -------------------------
  }

  // 为RS码、LRC码生成具体的修复执行计划
  bool Coordinator::concrete_repair_plans(
          int stripe_id,
          std::vector<RepairPlan>& repair_plans,
          std::vector<MainRepairPlan>& main_repairs,
          std::vector<std::vector<HelpRepairPlan>>& help_repairs)
  {
    Stripe& stripe = stripe_table_[stripe_id];
    std::vector<unsigned int> t_blocks2nodes(stripe.blocks2nodes.begin(),
        stripe.blocks2nodes.end());
    // for new locations, to optimize
    std::unordered_map<unsigned int, std::vector<unsigned int>> free_nodes_in_clusters;
    for (auto& repair_plan : repair_plans) {
      std::unordered_map<int, unsigned int> map2clusters;
      int cnt = 0;
      // 对修复方案中的可用块，统计每个集群中的可用块数量
      for (auto& help_block : repair_plan.help_blocks) {
        // 【新增检查】：防止 help_block[0] 越界崩溃
        if (help_block.empty()) {
            map2clusters[cnt++] = -1; 
            continue;
        }
        unsigned int nid = t_blocks2nodes[help_block[0]];
        unsigned int cid = node_table_[nid].map2cluster;
        map2clusters[cnt++] = cid;
      }
      // repair_plan.failure_idxs：该条带的失效块逻辑ID
      // 统计每个cluster的故障块数，选故障块最多的cluster作为主修复集群
      std::unordered_map<unsigned int, int> failures_cnt;
      unsigned int main_cid = 0;
      int max_cnt_val = 0;
      for (auto& idx : repair_plan.failure_idxs) {
        unsigned int nid = t_blocks2nodes[idx];
        unsigned int cid = node_table_[nid].map2cluster;
        if (failures_cnt.find(cid) == failures_cnt.end()) {
          failures_cnt[cid] = 1;
        } else {
          failures_cnt[cid]++; // 统计每个cluster的故障块数
        }
        // 选故障块最多的cluster作为主修复集群
        // 由于我们测试的节点故障，并且它是单个条带串行修复的，所以这里的主修复集群就是故障节点所在集群
        if (failures_cnt[cid] > max_cnt_val) { 
          max_cnt_val = failures_cnt[cid];
          main_cid = cid;
        }
      }
      std::unordered_set<unsigned int> failed_cluster_sets;
      for (auto it = repair_plan.failure_idxs.begin();
           it != repair_plan.failure_idxs.end(); it++) {
        unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        failed_cluster_sets.insert(cluster_id);
        if (free_nodes_in_clusters.find(cluster_id) ==
            free_nodes_in_clusters.end()) {
          std::vector<unsigned int> free_nodes;
          Cluster &cluster = cluster_table_[cluster_id];
          for (int i = 0; i < num_of_nodes_per_cluster_; i++) {
            free_nodes.push_back(cluster.nodes[i]);
          }
          free_nodes_in_clusters[cluster_id] = free_nodes;
        }
        auto iter = std::find(free_nodes_in_clusters[cluster_id].begin(), 
                              free_nodes_in_clusters[cluster_id].end(), node_id);
        if (iter != free_nodes_in_clusters[cluster_id].end()) {
          free_nodes_in_clusters[cluster_id].erase(iter);
        }
      }
      MainRepairPlan main_plan;
      int clusters_num = repair_plan.help_blocks.size();
      CodingParameters cp;
      ec_schema_.ec->get_coding_parameters(cp);
      for (int i = 0; i < clusters_num; i++) {
        for (auto block_idx : repair_plan.help_blocks[i]) {
          main_plan.live_blocks_index.push_back(block_idx);
        }
        if (failed_cluster_sets.find(map2clusters[i]) != failed_cluster_sets.end()) {
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            main_plan.inner_cluster_help_blocks_info.push_back(
                std::make_pair(block_idx, std::make_pair(node_ip, node_port)));
            main_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            // for new locations
            unsigned int cluster_id = node_table_[node_id].map2cluster;
            auto iter = std::find(free_nodes_in_clusters[cluster_id].begin(), 
                                  free_nodes_in_clusters[cluster_id].end(), node_id);
            if (iter != free_nodes_in_clusters[cluster_id].end()) {
              free_nodes_in_clusters[cluster_id].erase(iter);
            }
          }
        }
      }
      main_plan.ec_type = ec_schema_.ec_type;
      stripe.ec->get_coding_parameters(main_plan.cp);
      main_plan.cluster_id = main_cid;
      main_plan.cp.x = ec_schema_.x;
      main_plan.cp.seri_num = stripe_id % ec_schema_.x;
      main_plan.cp.local_or_column = repair_plan.local_or_column;
      main_plan.block_size = stripe.block_size;
      main_plan.partial_decoding = ec_schema_.partial_decoding;
      main_plan.partial_scheme = ec_schema_.partial_scheme;
      // 添加故障块信息
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        main_plan.failed_blocks_index.push_back(*it);
        main_plan.failed_block_ids.push_back(stripe.block_ids[*it]);
      }
      for (auto block_idx : repair_plan.parity_idxs) {
        main_plan.parity_blocks_index.push_back(block_idx);
      }
      std::vector<HelpRepairPlan> help_plans;
      // 添加helper块信息
      for (int i = 0; i < clusters_num; i++) {
        // 【新增检查】
        if (repair_plan.help_blocks[i].empty()) continue;
        if (map2clusters[i] != main_cid) {
          HelpRepairPlan help_plan;
          help_plan.ec_type = main_plan.ec_type;
          help_plan.cp = main_plan.cp;
          help_plan.cluster_id = map2clusters[i];
          help_plan.block_size = main_plan.block_size;
          help_plan.partial_decoding = main_plan.partial_decoding;
          help_plan.partial_scheme = main_plan.partial_scheme;
          help_plan.isvertical = main_plan.isvertical;
          if (ec_schema_.partial_scheme) {
            for(auto it = repair_plan.failure_idxs.begin();
              it != repair_plan.failure_idxs.end(); it++) {
              help_plan.failed_blocks_index.push_back(*it);
            }
          }
          for(auto it = main_plan.parity_blocks_index.begin(); 
              it != main_plan.parity_blocks_index.end(); it++) {
            help_plan.parity_blocks_index.push_back(*it);
          }
          for(auto it = main_plan.live_blocks_index.begin(); 
              it != main_plan.live_blocks_index.end(); it++) {
            help_plan.live_blocks_index.push_back(*it);
          }
          int num_of_help_blocks = 0;
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            help_plan.inner_cluster_help_blocks_info.push_back(
                std::make_pair(block_idx, std::make_pair(node_ip, node_port)));
            help_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            num_of_help_blocks++;
          }
          // 判断是否能够使用局部修复，减少数据传输
          if (ec_schema_.partial_scheme) {
            int failed_num = (int) help_plan.failed_blocks_index.size();
            if (num_of_help_blocks > failed_num) {
              help_plan.partial_less = true;
            }
          } else {
            int num_of_partial_blocks =
                ec_schema_.ec->num_of_partial_blocks_to_transfer(
                    repair_plan.help_blocks[i], help_plan.parity_blocks_index);
            if (num_of_partial_blocks < num_of_help_blocks) {
              help_plan.partial_less = true;
            }
          }
          main_plan.help_clusters_partial_less.push_back(help_plan.partial_less);
          main_plan.help_clusters_blocks_info.push_back(
              help_plan.inner_cluster_help_blocks_info);
          main_plan.help_clusters_block_ids.push_back(
              help_plan.inner_cluster_help_block_ids);
          help_plan.main_proxy_ip = cluster_table_[main_cid].proxy_ip;
          help_plan.main_proxy_port =
              cluster_table_[main_cid].proxy_port + SOCKET_PORT_OFFSET;
          help_plans.push_back(help_plan);
        }
      }
      // 选择新存储位置
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        std::vector<unsigned int> &free_nodes = free_nodes_in_clusters[cluster_id];
        int ran_node_idx = -1;
        unsigned int new_node_id = 0;
        // 从该cluster的可用节点中随机选择一个新节点
        ran_node_idx = random_index(free_nodes.size());
        new_node_id = free_nodes[ran_node_idx];
        auto iter = std::find(free_nodes.begin(), free_nodes.end(), new_node_id);
        if (iter != free_nodes.end()) {
          free_nodes.erase(iter);
        }
        t_blocks2nodes[*it] = new_node_id;
        std::string node_ip = node_table_[new_node_id].node_ip;
        int node_port = node_table_[new_node_id].node_port;
        main_plan.new_locations.push_back(
            std::make_pair(new_node_id, std::make_pair(node_ip, node_port)));
      }
      main_repairs.push_back(main_plan);
      help_repairs.push_back(help_plans);
    }
    return true;
  }

  // 为Product Code生成特殊的修复计划
  bool Coordinator::concrete_repair_plans_pc(
          int stripe_id,
          std::vector<RepairPlan>& repair_plans,
          std::vector<MainRepairPlan>& main_repairs,
          std::vector<std::vector<HelpRepairPlan>>& help_repairs)
  {
    Stripe& stripe = stripe_table_[stripe_id];
    std::vector<unsigned int> t_blocks2nodes(stripe.blocks2nodes.begin(),
        stripe.blocks2nodes.end());
    std::unordered_map<unsigned int, std::vector<unsigned int>> free_nodes_in_clusters;
    CodingParameters cp;
    stripe.ec->get_coding_parameters(cp);
    ProductCode pc;
    pc.init_coding_parameters(cp);
    for (auto& repair_plan : repair_plans) {
      std::unordered_map<int, unsigned int> map2clusters;
      int cnt = 0;
      for (auto& help_block : repair_plan.help_blocks) {
        unsigned int nid = t_blocks2nodes[help_block[0]];
        unsigned int cid = node_table_[nid].map2cluster;
        map2clusters[cnt++] = cid;
      }
      std::unordered_map<unsigned int, int> failures_cnt;
      unsigned int main_cid = 0;
      int max_cnt_val = 0;
      for (auto& idx : repair_plan.failure_idxs) {
        unsigned int nid = t_blocks2nodes[idx];
        unsigned int cid = node_table_[nid].map2cluster;
        if (failures_cnt.find(cid) == failures_cnt.end()) {
          failures_cnt[cid] = 1;
        } else {
          failures_cnt[cid]++;
        }
        if (failures_cnt[cid] > max_cnt_val) {
          max_cnt_val = failures_cnt[cid];
          main_cid = cid;
        }
      }
      // for new locations, to optimize
      std::unordered_set<unsigned int> failed_cluster_sets;
      for (auto it = repair_plan.failure_idxs.begin();
           it != repair_plan.failure_idxs.end(); it++) {
        unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        failed_cluster_sets.insert(cluster_id);
        if (free_nodes_in_clusters.find(cluster_id) ==
            free_nodes_in_clusters.end()) {
          std::vector<unsigned int> free_nodes;
          Cluster &cluster = cluster_table_[cluster_id];
          for (int i = 0; i < num_of_nodes_per_cluster_; i++) {
            free_nodes.push_back(cluster.nodes[i]);
          }
          free_nodes_in_clusters[cluster_id] = free_nodes;
        }
        auto iter = std::find(free_nodes_in_clusters[cluster_id].begin(), 
                              free_nodes_in_clusters[cluster_id].end(), node_id);
        if (iter != free_nodes_in_clusters[cluster_id].end()) {
          free_nodes_in_clusters[cluster_id].erase(iter);
        }
      }
      int row = -1, col = -1;
      MainRepairPlan main_plan;
      if (ec_schema_.multistripe_placement_rule == VERTICAL) {
          main_plan.isvertical = true;
      }
      int clusters_num = repair_plan.help_blocks.size();
      for (int i = 0; i < clusters_num; i++) {
        for (auto block_idx : repair_plan.help_blocks[i]) {
          pc.bid2rowcol(block_idx, row, col);
          if (repair_plan.local_or_column) {
            main_plan.live_blocks_index.push_back(row);
          } else {
            main_plan.live_blocks_index.push_back(col);
          }
        }
        if (failed_cluster_sets.find(map2clusters[i]) != failed_cluster_sets.end()) {
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            pc.bid2rowcol(block_idx, row, col);
            if (repair_plan.local_or_column) {
              main_plan.inner_cluster_help_blocks_info.push_back(
                  std::make_pair(row, std::make_pair(node_ip, node_port)));
            } else {
              main_plan.inner_cluster_help_blocks_info.push_back(
                  std::make_pair(col, std::make_pair(node_ip, node_port)));
            }

            main_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            
            // for new locations
            unsigned int cluster_id = node_table_[node_id].map2cluster;
            auto iter = std::find(free_nodes_in_clusters[cluster_id].begin(), 
                                  free_nodes_in_clusters[cluster_id].end(), node_id);
            if (iter != free_nodes_in_clusters[cluster_id].end()) {
              free_nodes_in_clusters[cluster_id].erase(iter);
            }
          }
        }
      }
      
      main_plan.ec_type = RS;
      if (repair_plan.local_or_column) {
        main_plan.cp.k = pc.k2;
        main_plan.cp.m = pc.m2;
      } else {
        main_plan.cp.k = pc.k1;
        main_plan.cp.m = pc.m1;
      }
      main_plan.cluster_id = main_cid;
      main_plan.cp.x = ec_schema_.x;
      main_plan.cp.seri_num = stripe_id % ec_schema_.x;
      main_plan.cp.local_or_column = repair_plan.local_or_column;
      main_plan.block_size = stripe.block_size;
      main_plan.partial_decoding = ec_schema_.partial_decoding;
      main_plan.partial_scheme = ec_schema_.partial_scheme;
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        pc.bid2rowcol(*it, row, col);
        if (repair_plan.local_or_column) {
          main_plan.failed_blocks_index.push_back(row);
        } else {
          main_plan.failed_blocks_index.push_back(col);
        }
        main_plan.failed_block_ids.push_back(stripe.block_ids[*it]);
      }
      for (auto block_idx : repair_plan.parity_idxs) {
        pc.bid2rowcol(block_idx, row, col);
        if (repair_plan.local_or_column) {
          main_plan.parity_blocks_index.push_back(row);
        } else {
          main_plan.parity_blocks_index.push_back(col);
        }
      }
      std::vector<HelpRepairPlan> help_plans;
      for (int i = 0; i < clusters_num; i++) {
        if (map2clusters[i] != main_cid) {
          HelpRepairPlan help_plan;
          help_plan.ec_type = main_plan.ec_type;
          help_plan.cp = main_plan.cp;
          help_plan.cluster_id = map2clusters[i];
          help_plan.block_size = main_plan.block_size;
          help_plan.partial_decoding = main_plan.partial_decoding;
          help_plan.partial_scheme = main_plan.partial_scheme;
          help_plan.isvertical = main_plan.isvertical;
          if (ec_schema_.partial_scheme) {
            for(auto it = main_plan.failed_blocks_index.begin();
                it != main_plan.failed_blocks_index.end(); it++) {
              help_plan.failed_blocks_index.push_back(*it);
            }
          }
          for(auto it = main_plan.parity_blocks_index.begin(); 
              it != main_plan.parity_blocks_index.end(); it++) {
            help_plan.parity_blocks_index.push_back(*it);
          }
          for(auto it = main_plan.live_blocks_index.begin(); 
              it != main_plan.live_blocks_index.end(); it++) {
            help_plan.live_blocks_index.push_back(*it);
          }
          int num_of_help_blocks = 0;
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            pc.bid2rowcol(block_idx, row, col);
            if (repair_plan.local_or_column) {
              help_plan.inner_cluster_help_blocks_info.push_back(
                  std::make_pair(row, std::make_pair(node_ip, node_port)));
            } else {
              help_plan.inner_cluster_help_blocks_info.push_back(
                  std::make_pair(col, std::make_pair(node_ip, node_port)));
            }
            help_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            num_of_help_blocks++;
          }
          if (ec_schema_.partial_scheme) {
            int failed_num = (int) help_plan.failed_blocks_index.size();
            if (num_of_help_blocks > failed_num) {
              help_plan.partial_less = true;
            }
          } else {
            int num_of_partial_blocks =
                ec_schema_.ec->num_of_partial_blocks_to_transfer(
                    repair_plan.help_blocks[i], help_plan.parity_blocks_index);
            if (num_of_partial_blocks < num_of_help_blocks) {
              help_plan.partial_less = true;
            }
          }
          main_plan.help_clusters_partial_less.push_back(help_plan.partial_less);
          main_plan.help_clusters_blocks_info.push_back(
              help_plan.inner_cluster_help_blocks_info);
          main_plan.help_clusters_block_ids.push_back(
              help_plan.inner_cluster_help_block_ids);
          help_plan.main_proxy_ip = cluster_table_[main_cid].proxy_ip;
          help_plan.main_proxy_port =
              cluster_table_[main_cid].proxy_port + SOCKET_PORT_OFFSET;
          help_plans.push_back(help_plan);
        }
      }
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        std::vector<unsigned int> &free_nodes = free_nodes_in_clusters[cluster_id];
        int ran_node_idx = -1;
        unsigned int new_node_id = 0;
        ran_node_idx = random_index(free_nodes.size());
        new_node_id = free_nodes[ran_node_idx];
        auto iter = std::find(free_nodes.begin(), free_nodes.end(), new_node_id);
        if (iter != free_nodes.end()) {
          free_nodes.erase(iter);
        }

        t_blocks2nodes[*it] = new_node_id;
        std::string node_ip = node_table_[new_node_id].node_ip;
        int node_port = node_table_[new_node_id].node_port;
        main_plan.new_locations.push_back(
            std::make_pair(new_node_id, std::make_pair(node_ip, node_port)));
      }
      main_repairs.push_back(main_plan);
      help_repairs.push_back(help_plans);
    }
    return true;
  }

  // 模拟修复过程，计算性能指标：跨集群传输量、IO次数
  void Coordinator::simulation_repair(
          std::vector<MainRepairPlan>& main_repair,
          int& cross_cluster_transfers,
          int& io_cnt)
  {
    if (IF_DEBUG) {
      std::cout << "Simulation:\n"; 
    }
    for (int i = 0; i < int(main_repair.size()); i++) {
      int failed_num = int(main_repair[i].failed_blocks_index.size());
      for (int j = 0; j < int(main_repair[i].help_clusters_blocks_info.size()); j++) {
        int num_of_help_blocks = int(main_repair[i].help_clusters_blocks_info[j].size());
        int num_of_partial_blocks = failed_num;
        if (IF_DEBUG) {
          std::cout << "Cluster " << j << ": ";
          for (auto& kv : main_repair[i].help_clusters_blocks_info[j]) {
            std::cout << kv.first << " ";
          }
          std::cout << " | ";
        }
        if (!ec_schema_.partial_scheme) {
        std::vector<int> local_data_idxs;
          for (auto& kv : main_repair[i].help_clusters_blocks_info[j]) {
            local_data_idxs.push_back(kv.first);
          }
          ec_schema_.ec->local_or_column = main_repair[i].cp.local_or_column;
          num_of_partial_blocks =
              ec_schema_.ec->num_of_partial_blocks_to_transfer(
                  local_data_idxs, main_repair[i].parity_blocks_index);
        }
        
        if (num_of_help_blocks > num_of_partial_blocks && ec_schema_.partial_decoding) {
          cross_cluster_transfers += num_of_partial_blocks;
          if (IF_DEBUG) {
            std::cout << num_of_partial_blocks << std::endl;
          }
        } else {
          cross_cluster_transfers += num_of_help_blocks;
          if (IF_DEBUG) {
            std::cout << num_of_help_blocks << std::endl;
          }
        }
        io_cnt += num_of_help_blocks;
      }
      for (auto& kv : main_repair[i].new_locations) {
        unsigned int cid = node_table_[kv.first].map2cluster;
        if (cid != main_repair[i].cluster_id) {
          cross_cluster_transfers++;
        }
        if (IF_DEBUG) {
          std::cout << "New block will be placed at node " << kv.first
                    << " (cluster " << cid << ")" << std::endl;
        }
      }
      io_cnt += failed_num;
      io_cnt += int(main_repair[i].inner_cluster_help_block_ids.size());
    }
  }

  // 读取最小费用最大流修复方案
  bool Coordinator::loadRepairData(const std::string& filename,
                     std::vector<int>& main_help_clusterID,
                     std::vector<std::vector<std::pair<int, int>>>& other_help_clusterID_chunkNum_pairs) {
                      
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "错误: 无法打开文件 " << filename << " 进行读取" << std::endl;
        perror("具体错误");
        return false;
    }
    
    // 清空原有数据
    main_help_clusterID.clear();
    other_help_clusterID_chunkNum_pairs.clear();
    
    // 1. 读取main_help_clusterID数组
    size_t main_size = 0;
    file.read(reinterpret_cast<char*>(&main_size), sizeof(main_size));
    
    if (main_size > 0) {
        main_help_clusterID.resize(main_size);
        file.read(reinterpret_cast<char*>(main_help_clusterID.data()), 
                  main_size * sizeof(int));
    }
    
    // 2. 读取other_help_clusterID_chunkNum_pairs
    size_t outer_size = 0;
    file.read(reinterpret_cast<char*>(&outer_size), sizeof(outer_size));
    
    if (outer_size > 0) {
        other_help_clusterID_chunkNum_pairs.resize(outer_size);
        
        for (size_t i = 0; i < outer_size; ++i) {
            size_t inner_size = 0;
            file.read(reinterpret_cast<char*>(&inner_size), sizeof(inner_size));
            
            if (inner_size > 0) {
                other_help_clusterID_chunkNum_pairs[i].resize(inner_size);
                
                for (size_t j = 0; j < inner_size; ++j) {
                    int first, second;
                    file.read(reinterpret_cast<char*>(&first), sizeof(first));
                    file.read(reinterpret_cast<char*>(&second), sizeof(second));
                    other_help_clusterID_chunkNum_pairs[i][j] = {first, second};
                }
            }
        }
    }
    
    if (file.fail()) {
        std::cerr << "错误: 读取文件 " << filename << " 时发生错误" << std::endl;
        return false;
    }
    
    file.close();
    
    std::cout << "成功从 " << filename << " 读取集群数据:" << std::endl;
    std::cout << "  - main_help_clusterID 大小: " << main_help_clusterID.size() << std::endl;
    std::cout << "  - other_help_clusterID_chunkNum_pairs 大小: " 
              << other_help_clusterID_chunkNum_pairs.size() << std::endl;
    
    return true;
  }

  void Coordinator::do_flow_repair_common(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response, bool unordered_concurrency_main_repairs,
          bool parallel_stripes, bool schedule_cross_rack_links,
          bool schedule_join_per_round)
  {
    struct timeval e2e_start_time{}, e2e_end_time{};
    gettimeofday(&e2e_start_time, NULL);
    double repair_time = 0;
    double decoding_time = 0;
    double cross_cluster_time = 0;
    double inner_network_time = 0;
    double io_time = 0;
    double meta_time = 0;
    double repair_excluded_warmup_sec = 0;
    int cross_cluster_transfers = 0;
    int io_cnt = 0;
    std::unordered_map<unsigned int, std::vector<int>> failures_map;

    auto assign_flow_resp = [&](bool ok) {
      response.decoding_time = decoding_time;
      response.cross_cluster_time = cross_cluster_time;
      response.inner_network_time = inner_network_time;
      response.io_time = io_time;
      response.repair_time = std::max(0.0, repair_time - repair_excluded_warmup_sec);
      response.meta_time = meta_time;
      response.cross_cluster_transfers = cross_cluster_transfers;
      response.io_cnt = io_cnt;
      response.success = ok;
    };

    // 检测故障范围
    check_out_failures(stripe_id, failed_ids, failures_map);
    // 如果没有检测到受影响的条带，直接判定修复成功并返回
    if (failures_map.empty()) {
      std::cerr << "[INFO] Flow repair: no affected stripes — failed node(s) hold no blocks "
                   "in coordinator metadata (check global node id vs test_node_repair arg).\n";
      assign_flow_resp(true);
      return;
    }
    // ----------------------------------------------
    // 将 failures_map 中的条带以 Available 矩阵格式输入至文件，供 min_cost_max_flow.cpp 读取生成修复方案
    
    // 获取集群数量（假设集群ID从0到num_clusters-1连续）
    int num_clusters = cluster_table_.size();
    if (num_clusters == 0) {
        std::cerr << "[ERROR] No clusters available!" << std::endl;
        assign_flow_resp(false);
        return;
    }

    // 收集所有待修复的条带，按 failures_map 的遍历顺序（即当前无序，但为了保持与后续处理一致，我们显式保存顺序）
    std::vector<int> ordered_stripe_ids;
    for (const auto& pair : failures_map) {
        ordered_stripe_ids.push_back(pair.first);
    }

    // 构建 Available 矩阵：行 = 条带（按 ordered_stripe_ids 顺序），列 = 集群ID（0..num_clusters-1）
    std::vector<std::vector<int>> available_matrix;
    available_matrix.reserve(ordered_stripe_ids.size());
    
    for (int stripe_id : ordered_stripe_ids) {
        Stripe& stripe = stripe_table_[stripe_id];
        const auto& failed_blocks = failures_map[stripe_id];  // 故障块索引列表
        std::unordered_set<int> failed_set(failed_blocks.begin(), failed_blocks.end());

        // 初始化该条带在每个集群的可用块计数为0
        std::vector<int> cluster_counts(num_clusters, 0);

        // 遍历该条带的所有块（假设总块数为 k+m，可以从 stripe 中获取，例如 stripe.ec->k + stripe.ec->m）
        // int total_blocks = stripe.ec->k + stripe.ec->m;
        // 【修改后】使用真实的 blocks2nodes 大小防止越界读取垃圾数据
        int total_blocks = stripe.blocks2nodes.size();
        for (int block_idx = 0; block_idx < total_blocks; ++block_idx) {
            if (failed_set.find(block_idx) != failed_set.end()) {
                continue;  // 故障块不计入可用
            }
            // 获取块所在的节点ID，再映射到集群ID
            unsigned int node_id = stripe.blocks2nodes[block_idx];
            unsigned int cluster_id = node_table_[node_id].map2cluster;
            // 确保 cluster_id 在合法范围内
            if (cluster_id >= 0 && cluster_id < num_clusters) {
                cluster_counts[cluster_id]++;
            } else {
                std::cerr << "[WARNING] Block " << block_idx << " of stripe " << stripe_id
                          << " is on invalid cluster " << cluster_id << std::endl;
            }
        }
        available_matrix.push_back(cluster_counts);
    }

    // 将矩阵写入文件 "../Available_matrix"
    std::string filename = "/home/hadoop/zzy/ec_prototype-master/flow_repair/Available_matrix";
    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        std::cerr << "[ERROR] Cannot create Available matrix file: " << filename << std::endl;
        assign_flow_resp(false);
        return;
    }

    outfile << "Available = {\n";
    for (size_t i = 0; i < available_matrix.size(); ++i) {
        outfile << "    {";
        const auto& row = available_matrix[i];
        for (size_t j = 0; j < row.size(); ++j) {
            outfile << row[j];
            if (j < row.size() - 1) outfile << ",";
        }
        outfile << "}";
        if (i < available_matrix.size() - 1) outfile << ",";
        outfile << "\n";
    }
    outfile << "};\n";
    outfile.close();

    std::cout << "[INFO] Available matrix written to " << filename << " with "
              << available_matrix.size() << " rows." << std::endl;

    // 自动调用 min_cost_max_flow 程序生成 cluster_data.bin
    // 编译后的可执行文件名为 "min_cost_max_flow"
    std::string command = "/home/hadoop/zzy/ec_prototype-master/flow_repair/complete_min_cost_max_flow";
    struct timeval solver_start_time{}, solver_end_time{};
    gettimeofday(&solver_start_time, NULL);
    int ret = system(command.c_str());
    gettimeofday(&solver_end_time, NULL);
    const double solver_duration =
        (solver_end_time.tv_sec - solver_start_time.tv_sec) +
        (solver_end_time.tv_usec - solver_start_time.tv_usec) * 1.0 / 1000000;
    // 将求解算法的执行时间计入元数据时间
    meta_time += solver_duration;
    if (ret != 0) {
        std::cerr << "[ERROR] Failed to execute min_cost_max_flow. Return code: " << ret << std::endl;
        std::cerr << "Please ensure the program is compiled and available in the current directory." << std::endl;
        assign_flow_resp(false);
        return;
    }
    std::cout << "[INFO] min_cost_max_flow executed successfully." << std::endl;


    // ----------------------------------------------
    // 读取最小费用最大流算法生成的修复方案
    std::vector<int> main_help_clusterID;
    std::vector<std::vector<std::pair<int, int>>> other_help_clusterID_chunkNum_pairs;
    if (!loadRepairData("/home/hadoop/zzy/ec_prototype-master/flow_repair/cluster_data.bin", main_help_clusterID, other_help_clusterID_chunkNum_pairs)) {
        std::cerr << "[ERROR]: Read cluster_data.bin error! " << std::endl;
        assign_flow_resp(false);
        return;
    }
    // 复制一份 main_help_clusterID，因为后面会修改为 分区ID 的映射关系
    std::vector<int> main_help_clusterID_original = main_help_clusterID;
    
    // 验证数据是否正确加载
    if (IF_DEBUG) {
      std::cout << "\n[Check]: element in main_help_clusterID : " << std::endl;
      for (size_t i = 0; i < main_help_clusterID.size(); ++i) {
          std::cout << main_help_clusterID[i] << " ";
      }
      std::cout << std::endl;
      
      std::cout << "[Check]: first vector size in other_help_clusterID_chunkNum_pairs : " << std::endl;
      for (size_t i = 0; i < other_help_clusterID_chunkNum_pairs.size(); ++i) {
        std::cout << "[Check]: Stripe " << i << " 's {help_cluster, chunk_num} vector: " ;
        
        for (size_t j = 0; j < other_help_clusterID_chunkNum_pairs[i].size(); ++j) {
            const auto& p = other_help_clusterID_chunkNum_pairs[i][j];
            std::cout << "(" << p.first << ", " << p.second << ") ";
        }
        
        if (other_help_clusterID_chunkNum_pairs[i].empty()) {
            std::cout << "null";
        }
        std::cout << std::endl;
      }
    }

    // -------------------------------------------------
    std::mutex stripe_accum_mutex;
    /** 条带间并行时，多线程不得同时改 main_help_clusterID / other_* 或拷贝它们进 generate_flow_repair_plan（UB，可表现为卡死） */
    std::mutex flow_shared_mcmcf_mutex;
    std::atomic<bool> stripe_ok{true};
    // 条带间并行时，各条带 temp_time 会重叠，不能简单相加；response.repair_time 用下面墙钟区间。
    struct timeval flow_stripe_phase_start, flow_stripe_phase_end;
    double stripe_durations_sum = 0;
    // 每条带聚合后的 decode/cross（已按条带内并发取 max 再按批相加）；条带间再按 parallel_stripes 规则汇总到 response
    const size_t n_flow_stripes = ordered_stripe_ids.size();
    
    // 判断是否开启全局调度：如果允许无序并发且要求链路调度，就开启“将所有条带的传输合起来统一做二分图匹配”的高级特性
    const bool enable_global_schedule =
        unordered_concurrency_main_repairs && schedule_cross_rack_links;
    std::vector<double> flow_stripe_decode(n_flow_stripes, 0.0);
    std::vector<double> flow_stripe_cross(n_flow_stripes, 0.0);
    std::vector<double> flow_stripe_inner_net(n_flow_stripes, 0.0);
    std::vector<double> flow_stripe_io(n_flow_stripes, 0.0);
    std::vector<double> flow_stripe_meta(n_flow_stripes, 0.0);
    struct GlobalHelpTask {
      int stripe_idx = -1;
      int plan_i = -1;
      int help_j = -1;
    };
    struct StripeGlobalContext {
      bool ready = false;
      int failure_stripe_ID = -1;
      int stripe_id = -1;
      std::vector<int> failed_blocks;
      std::vector<MainRepairPlan> main_repairs;
      std::vector<std::vector<HelpRepairPlan>> help_repairs;
      std::vector<double> per_plan_main_dec;
      std::vector<double> per_plan_main_net;
      std::vector<double> per_plan_main_inner_net;
      std::vector<double> per_plan_main_io;
      std::vector<double> per_plan_help_max_dec;
      std::vector<double> per_plan_help_max_net;
      std::vector<double> per_plan_help_max_inner_net;
      std::vector<double> per_plan_help_max_io;
      std::mutex per_plan_help_max_mtx;
      double stripe_begin_sec = 0.0;
      double stripe_end_sec = 0.0;
    };
    // 全局调度上下文，预分配内存保存所有条带的物理方案。
    std::vector<StripeGlobalContext> global_ctx(n_flow_stripes);
    // 与条带内 ordered 分支一致：vector + pop_back；同一轮内 take>1 时仍并发起多个 help 线程
    // 记录 {源集群, 目的集群} 之间到底有多少个帮手传输任务
    std::map<std::pair<int, int>, std::vector<GlobalHelpTask>> global_link2tasks;
    std::mutex global_schedule_mutex;
    std::mutex timeline_mutex;
    std::vector<TimelineEvent> stripe_events;
    std::unordered_map<int, int> stripe_lane_by_id;
    std::vector<std::string> stripe_lane_labels;
    std::vector<TimelineEvent> block_events;
    std::unordered_map<std::string, int> block_lane_by_key;
    std::vector<std::string> block_lane_labels;
    std::vector<std::string> schedule_notes;
    std::atomic<int> help_dispatch_seq{0};
    std::atomic<int> warmup_dispatch_seq{0};
    const auto timeline_start = std::chrono::steady_clock::now();
    auto now_sec = [&]() -> double {
      const auto dt = std::chrono::steady_clock::now() - timeline_start;
      return std::chrono::duration_cast<std::chrono::duration<double>>(dt).count();
    };
    auto make_help_task_id = [](const StripeGlobalContext& ctx, int plan_i, int help_j) {
      return std::to_string(ctx.stripe_id) + ":" + std::to_string(plan_i) + ":" +
             std::to_string(help_j);
    };
    auto estimate_help_cross_bytes = [](const HelpRepairPlan& plan) -> size_t {
      size_t send_blocks = 0;
      const bool partial_send = plan.partial_decoding && plan.partial_less;
      if (partial_send) {
        send_blocks = plan.partial_scheme ? plan.failed_blocks_index.size()
                                          : plan.parity_blocks_index.size();
      } else {
        send_blocks = plan.inner_cluster_help_blocks_info.size();
      }
      return send_blocks * plan.block_size;
    };
    const int global_flow_rpc_timeout_sec = 500;
    auto send_global_help_repair_task =
        [&](StripeGlobalContext& ctx, int i, int j, std::string proxy_ip, int proxy_port,
            int round_id, int src_cluster_1b, int dst_cluster_1b, int dispatch_seq) {
          const double evt_start = now_sec();
          double evt_dec = 0.0;
          double evt_cross = 0.0;
          double evt_inner = 0.0;
          double evt_io = 0.0;
          std::vector<RepairPhaseSpan> evt_phase_spans;
          double evt_phase_total = 0.0;
          try
          {
            std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
            std::cout << "[DEBUG send_global_help] stripe=" << ctx.stripe_id
                      << " plan_i=" << i << " help_j=" << j
                      << " sending_to_proxy=" << chosen_proxy
                      << " plan.main_proxy_ip=" << ctx.help_repairs[i][j].main_proxy_ip
                      << " plan.main_proxy_port=" << ctx.help_repairs[i][j].main_proxy_port
                      << std::endl;
            auto& hlpc = tls_proxy_client(chosen_proxy, proxy_ip, proxy_port);
            auto r = async_simple::coro::syncAwait(
                hlpc.call_for<&Proxy::help_repair>(
                    std::chrono::seconds{global_flow_rpc_timeout_sec},
                    ctx.help_repairs[i][j]));
            if (!r) {
              std::cerr << "[RPC ERROR] help_repair failed to proxy " << chosen_proxy
                        << ": " << r.error().msg << std::endl;
              throw std::runtime_error("help_repair rpc failed");
            }
            auto resp = r.value();
            evt_dec = resp.decoding_time;
            evt_cross = resp.cross_cluster_time;
            evt_inner = resp.inner_network_time;
            evt_io = resp.io_time;
            evt_phase_spans = resp.phase_spans;
            evt_phase_total = resp.phase_total_time;
            {
              std::lock_guard<std::mutex> lk(ctx.per_plan_help_max_mtx);
              ctx.per_plan_help_max_dec[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_dec[static_cast<size_t>(i)], evt_dec);
              ctx.per_plan_help_max_net[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_net[static_cast<size_t>(i)], evt_cross);
              ctx.per_plan_help_max_inner_net[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_inner_net[static_cast<size_t>(i)],
                           evt_inner);
              ctx.per_plan_help_max_io[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_io[static_cast<size_t>(i)], evt_io);
            }
          }
          catch (...)
          {
            stripe_ok.store(false);
          }
          const double evt_end = now_sec();
          if (IF_DEBUG && unordered_concurrency_main_repairs) {
            std::lock_guard<std::mutex> lk(timeline_mutex);
            for (const auto& bi : ctx.help_repairs[i][j].inner_cluster_help_blocks_info) {
              const int blk = bi.first;
              const std::string lane_key =
                  std::to_string(ctx.stripe_id) + ":help:block" + std::to_string(blk);
              int lane = 0;
              auto it = block_lane_by_key.find(lane_key);
              if (it == block_lane_by_key.end()) {
                lane = static_cast<int>(block_lane_labels.size());
                block_lane_by_key[lane_key] = lane;
                block_lane_labels.push_back("S" + std::to_string(ctx.stripe_id) +
                                            " B" + std::to_string(blk) + " help");
              } else {
                lane = it->second;
              }
              std::ostringstream oss;
              oss << "#" << dispatch_seq << " r" << round_id << " "
                  << src_cluster_1b << "->" << dst_cluster_1b;
              static const char* kRoundColors[] = {
                  "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
              };
              const std::string c =
                  kRoundColors[static_cast<size_t>(round_id >= 0 ? round_id : 0) % 6];
              block_events.push_back({lane, evt_start, evt_end, c, oss.str(),
                                      evt_dec, evt_cross, evt_inner, evt_io, 0.0,
                                      evt_phase_spans, evt_phase_total});
            }
          }
        };

    auto prepare_global_help_repair_task =
        [&](StripeGlobalContext& ctx, int i, int j, std::string proxy_ip, int proxy_port,
            int round_id, int src_cluster_1b, int dst_cluster_1b, int dispatch_seq) {
          const double evt_start = now_sec();
          double evt_dec = 0.0;
          double evt_inner = 0.0;
          double evt_io = 0.0;
          std::vector<RepairPhaseSpan> evt_phase_spans;
          double evt_phase_total = 0.0;
          try {
            std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
            HelpRepairPrepareReq req;
            req.repair_plan = ctx.help_repairs[i][j];
            req.task_id = make_help_task_id(ctx, i, j);
            auto r = [&]() {
              // 使用独立的 prepare 通道，避免与同一 proxy 的 send 通道互斥，
              // 从而让 N 轮跨机架发送与 N+1 轮机架内读取/编码真正并行。
              std::lock_guard<std::mutex> plk(*help_prepare_proxy_mutexes_[chosen_proxy]);
              return async_simple::coro::syncAwait(
                  help_prepare_proxies_[chosen_proxy]->call_for<&Proxy::prepare_help_repair_data>(
                      std::chrono::seconds{global_flow_rpc_timeout_sec}, req));
            }();
            if (!r) {
              std::cerr << "[RPC ERROR] prepare_help_repair_data failed to proxy "
                        << chosen_proxy << ": " << r.error().msg << std::endl;
              throw std::runtime_error("prepare_help_repair_data rpc failed");
            }
            auto prep_resp = r.value();
            if (!prep_resp.success) {
              std::cerr << "[RPC ERROR] prepare_help_repair_data rejected by proxy "
                        << chosen_proxy << ": " << prep_resp.err_msg << std::endl;
              throw std::runtime_error("prepare_help_repair_data proxy returned failed");
            }
            evt_dec = prep_resp.decoding_time;
            evt_inner = prep_resp.inner_network_time;
            evt_io = prep_resp.io_time;
            evt_phase_spans = prep_resp.phase_spans;
            evt_phase_total = prep_resp.phase_total_time;
            {
              std::lock_guard<std::mutex> lk(ctx.per_plan_help_max_mtx);
              ctx.per_plan_help_max_dec[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_dec[static_cast<size_t>(i)], evt_dec);
              ctx.per_plan_help_max_inner_net[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_inner_net[static_cast<size_t>(i)],
                           evt_inner);
              ctx.per_plan_help_max_io[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_io[static_cast<size_t>(i)], evt_io);
            }
          } catch (...) {
            stripe_ok.store(false);
          }
          const double evt_end = now_sec();
          if (IF_DEBUG && unordered_concurrency_main_repairs) {
            std::lock_guard<std::mutex> lk(timeline_mutex);
            for (const auto& bi : ctx.help_repairs[i][j].inner_cluster_help_blocks_info) {
              const int blk = bi.first;
              const std::string lane_key =
                  std::to_string(ctx.stripe_id) + ":help:block" + std::to_string(blk);
              int lane = 0;
              auto it = block_lane_by_key.find(lane_key);
              if (it == block_lane_by_key.end()) {
                lane = static_cast<int>(block_lane_labels.size());
                block_lane_by_key[lane_key] = lane;
                block_lane_labels.push_back("S" + std::to_string(ctx.stripe_id) +
                                            " B" + std::to_string(blk) + " help");
              } else {
                lane = it->second;
              }
              std::ostringstream oss;
              oss << "#" << dispatch_seq << " r" << round_id << " "
                  << src_cluster_1b << "->" << dst_cluster_1b << " prepare";
              static const char* kRoundColors[] = {
                  "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
              };
              const std::string c =
                  kRoundColors[static_cast<size_t>(round_id >= 0 ? round_id : 0) % 6];
              block_events.push_back({lane, evt_start, evt_end, c, oss.str(), evt_dec, 0.0,
                                      evt_inner, evt_io, 0.0, evt_phase_spans,
                                      evt_phase_total});
            }
          }
        };

    auto send_global_prepared_help_repair_task =
        [&](StripeGlobalContext& ctx, int i, int j, std::string proxy_ip, int proxy_port,
            int round_id, int src_cluster_1b, int dst_cluster_1b, int dispatch_seq) {
          const double evt_start = now_sec();
          double evt_cross = 0.0;
          std::vector<RepairPhaseSpan> evt_phase_spans;
          double evt_phase_total = 0.0;
          try {
            std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
            HelpRepairSendReq req;
            req.task_id = make_help_task_id(ctx, i, j);
            const std::string main_proxy = ctx.help_repairs[i][j].main_proxy_ip + ":" +
                std::to_string(ctx.help_repairs[i][j].main_proxy_port);
            {
              std::lock_guard<std::mutex> lk(stripe_accum_mutex);
              std::cout << "[HCCTRL] stage=dispatch"
                        << " dispatch_seq=" << dispatch_seq
                        << " round=" << round_id
                        << " src=" << src_cluster_1b
                        << " dst=" << dst_cluster_1b
                        << " stripe=" << ctx.stripe_id
                        << " plan_i=" << i
                        << " help_j=" << j
                        << " task_id=" << req.task_id
                        << " helper_proxy=" << chosen_proxy
                        << " main_proxy=" << main_proxy
                        << std::endl;
            }
            const double rpc_start = now_sec();
            auto r = [&]() {
              // 使用独立的 send 通道，与 prepare 通道的锁互不干扰。
              std::lock_guard<std::mutex> plk(*help_send_proxy_mutexes_[chosen_proxy]);
              return async_simple::coro::syncAwait(
                  help_send_proxies_[chosen_proxy]->call_for<&Proxy::send_prepared_help_repair_data>(
                      std::chrono::seconds{global_flow_rpc_timeout_sec}, req));
            }();
            const double rpc_wall = now_sec() - rpc_start;
            if (!r) {
              std::cerr << "[RPC ERROR] send_prepared_help_repair_data failed to proxy "
                        << chosen_proxy << ": " << r.error().msg << std::endl;
              throw std::runtime_error("send_prepared_help_repair_data rpc failed");
            }
            auto send_resp = r.value();
            if (!send_resp.success) {
              std::cerr << "[RPC ERROR] send_prepared_help_repair_data rejected by proxy "
                        << chosen_proxy << ": " << send_resp.err_msg << std::endl;
              throw std::runtime_error("send_prepared_help_repair_data proxy returned failed");
            }
            evt_cross = send_resp.cross_cluster_time;
            evt_phase_spans = send_resp.phase_spans;
            evt_phase_total = send_resp.phase_total_time;
            {
              std::lock_guard<std::mutex> lk(stripe_accum_mutex);
              std::cout << "[HCCTRL] stage=done"
                        << " dispatch_seq=" << dispatch_seq
                        << " round=" << round_id
                        << " src=" << src_cluster_1b
                        << " dst=" << dst_cluster_1b
                        << " stripe=" << ctx.stripe_id
                        << " plan_i=" << i
                        << " help_j=" << j
                        << " task_id=" << req.task_id
                        << " helper_proxy=" << chosen_proxy
                        << " main_proxy=" << main_proxy
                        << " rpc_wall=" << rpc_wall
                        << " cross_cluster_time=" << evt_cross
                        << " phase_total=" << evt_phase_total
                        << std::endl;
            }
            {
              std::lock_guard<std::mutex> lk(ctx.per_plan_help_max_mtx);
              ctx.per_plan_help_max_net[static_cast<size_t>(i)] =
                  std::max(ctx.per_plan_help_max_net[static_cast<size_t>(i)], evt_cross);
            }
          } catch (...) {
            stripe_ok.store(false);
          }
          const double evt_end = now_sec();
          if (IF_DEBUG && unordered_concurrency_main_repairs) {
            std::lock_guard<std::mutex> lk(timeline_mutex);
            for (const auto& bi : ctx.help_repairs[i][j].inner_cluster_help_blocks_info) {
              const int blk = bi.first;
              const std::string lane_key =
                  std::to_string(ctx.stripe_id) + ":help:block" + std::to_string(blk);
              int lane = 0;
              auto it = block_lane_by_key.find(lane_key);
              if (it == block_lane_by_key.end()) {
                lane = static_cast<int>(block_lane_labels.size());
                block_lane_by_key[lane_key] = lane;
                block_lane_labels.push_back("S" + std::to_string(ctx.stripe_id) +
                                            " B" + std::to_string(blk) + " help");
              } else {
                lane = it->second;
              }
              std::ostringstream oss;
              oss << "#" << dispatch_seq << " r" << round_id << " "
                  << src_cluster_1b << "->" << dst_cluster_1b << " send";
              static const char* kRoundColors[] = {
                  "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
              };
              const std::string c =
                  kRoundColors[static_cast<size_t>(round_id >= 0 ? round_id : 0) % 6];
              block_events.push_back({lane, evt_start, evt_end, c, oss.str(), 0.0,
                                      evt_cross, 0.0, 0.0, 0.0, evt_phase_spans,
                                      evt_phase_total});
            }
          }
        };

    auto send_global_round_warmup_task =
        [&](StripeGlobalContext& ctx, int i, int j,
            int round_id, int src_cluster_1b, int dst_cluster_1b, int warmup_seq) {
          const HelpRepairPlan& plan = ctx.help_repairs[i][j];
          const size_t bytes_total = estimate_help_cross_bytes(plan);
          if (bytes_total == 0) {
            return;
          }
          const double evt_start = now_sec();
          double evt_cross = 0.0;
          std::vector<RepairPhaseSpan> evt_phase_spans;
          double evt_phase_total = 0.0;
          try {
            const int main_cluster_idx = dst_cluster_1b - 1;
            if (main_cluster_idx < 0 ||
                cluster_table_.find(static_cast<unsigned int>(main_cluster_idx)) ==
                    cluster_table_.end()) {
              throw std::runtime_error("invalid warmup main cluster");
            }
            Cluster& main_cluster = cluster_table_[static_cast<unsigned int>(main_cluster_idx)];
            Cluster& helper_cluster = cluster_table_[plan.cluster_id];
            HelpCrossWarmupReq req;
            req.task_id = "warmup:" + make_help_task_id(ctx, i, j);
            req.main_proxy_ip = main_cluster.proxy_ip;
            req.main_proxy_port =
                main_cluster.proxy_port + SOCKET_PORT_OFFSET + 10000 + warmup_seq;
            req.bytes_total = bytes_total;
            req.helper_cluster_id = plan.cluster_id;
            req.main_cluster_id = static_cast<unsigned int>(main_cluster_idx);

            HelpCrossWarmupResp recv_resp{};
            HelpCrossWarmupResp send_resp{};
            std::thread recv_thread([&]() {
              std::string main_key =
                  proxy_endpoint_key(main_cluster.proxy_ip, main_cluster.proxy_port);
              auto& main_client = tls_proxy_client(
                  main_key, main_cluster.proxy_ip, main_cluster.proxy_port);
              auto r = async_simple::coro::syncAwait(
                  main_client.call_for<&Proxy::receive_help_cross_warmup>(
                      std::chrono::seconds{global_flow_rpc_timeout_sec}, req));
              if (!r) {
                recv_resp.success = false;
                recv_resp.err_msg = r.error().msg;
              } else {
                recv_resp = r.value();
              }
            });
            std::thread send_thread([&]() {
              std::string helper_key =
                  proxy_endpoint_key(helper_cluster.proxy_ip, helper_cluster.proxy_port);
              auto& helper_client = tls_proxy_client(
                  helper_key, helper_cluster.proxy_ip, helper_cluster.proxy_port);
              auto r = async_simple::coro::syncAwait(
                  helper_client.call_for<&Proxy::send_help_cross_warmup>(
                      std::chrono::seconds{global_flow_rpc_timeout_sec}, req));
              if (!r) {
                send_resp.success = false;
                send_resp.err_msg = r.error().msg;
              } else {
                send_resp = r.value();
              }
            });
            recv_thread.join();
            send_thread.join();
            if (!recv_resp.success || !send_resp.success) {
              std::cerr << "[HCWARMUP ERROR] round=" << round_id
                        << " src=" << src_cluster_1b
                        << " dst=" << dst_cluster_1b
                        << " recv=" << recv_resp.err_msg
                        << " send=" << send_resp.err_msg << std::endl;
              throw std::runtime_error("help cross warmup failed");
            }
            evt_cross = send_resp.cross_cluster_time;
            evt_phase_spans = send_resp.phase_spans;
            evt_phase_total = send_resp.phase_total_time;
          } catch (...) {
            stripe_ok.store(false);
          }
          const double evt_end = now_sec();
          if (IF_DEBUG && unordered_concurrency_main_repairs) {
            std::lock_guard<std::mutex> lk(timeline_mutex);
            for (const auto& bi : ctx.help_repairs[i][j].inner_cluster_help_blocks_info) {
              const int blk = bi.first;
              const std::string lane_key =
                  std::to_string(ctx.stripe_id) + ":help:block" + std::to_string(blk);
              int lane = 0;
              auto it = block_lane_by_key.find(lane_key);
              if (it == block_lane_by_key.end()) {
                lane = static_cast<int>(block_lane_labels.size());
                block_lane_by_key[lane_key] = lane;
                block_lane_labels.push_back("S" + std::to_string(ctx.stripe_id) +
                                            " B" + std::to_string(blk) + " help");
              } else {
                lane = it->second;
              }
              std::ostringstream oss;
              oss << "#W" << warmup_seq << " r" << round_id << " "
                  << src_cluster_1b << "->" << dst_cluster_1b << " round_warmup";
              block_events.push_back({lane, evt_start, evt_end, "#7FB3D5", oss.str(),
                                      0.0, evt_cross, 0.0, 0.0, 0.0,
                                      evt_phase_spans, evt_phase_total});
            }
          }
        };

    // 全局调度路径用：与条带内 send_main_repair_plan 等价，但写入 StripeGlobalContext
    auto send_global_main_repair_task =
        [&](StripeGlobalContext& ctx, int plan_i, int main_cluster_id) {
          const double evt_start = now_sec();
          double evt_dec = 0.0;
          double evt_cross = 0.0;
          double evt_inner = 0.0;
          double evt_io = 0.0;
          std::vector<RepairPhaseSpan> evt_phase_spans;
          double evt_phase_total = 0.0;
          try
          {
            std::string chosen_proxy = proxy_endpoint_key(
                cluster_table_[main_cluster_id].proxy_ip,
                cluster_table_[main_cluster_id].proxy_port);
            auto& task_client = tls_proxy_client(
                chosen_proxy,
                cluster_table_[main_cluster_id].proxy_ip,
                cluster_table_[main_cluster_id].proxy_port);
            auto r = async_simple::coro::syncAwait(
                task_client.call_for<&Proxy::main_repair>(
                    std::chrono::seconds{global_flow_rpc_timeout_sec},
                    ctx.main_repairs[plan_i]));
            if (!r) {
              std::cerr << "[RPC ERROR] main_repair failed to proxy " << chosen_proxy
                        << ": " << r.error().msg << std::endl;
              throw std::runtime_error("main_repair rpc failed");
            }
            auto resp = r.value();
            evt_dec = resp.decoding_time;
            evt_cross = resp.cross_cluster_time;
            evt_inner = resp.inner_network_time;
            evt_io = resp.io_time;
            evt_phase_spans = resp.phase_spans;
            evt_phase_total = resp.phase_total_time;
            ctx.per_plan_main_dec[static_cast<size_t>(plan_i)] = evt_dec;
            ctx.per_plan_main_net[static_cast<size_t>(plan_i)] = evt_cross;
            ctx.per_plan_main_inner_net[static_cast<size_t>(plan_i)] = evt_inner;
            ctx.per_plan_main_io[static_cast<size_t>(plan_i)] = evt_io;
            if (IF_DEBUG) {
              std::cout << "Selected main proxy " << chosen_proxy
                        << " of cluster" << main_cluster_id << " plan "
                        << plan_i << " stripe " << ctx.stripe_id
                        << ". Decoding time : " << resp.decoding_time << std::endl;
            }
          }
          catch (...)
          {
            stripe_ok.store(false);
          }
          const double evt_end = now_sec();
          if (IF_DEBUG && unordered_concurrency_main_repairs) {
            std::lock_guard<std::mutex> lk(timeline_mutex);
            for (int blk : ctx.main_repairs[plan_i].failed_blocks_index) {
              const std::string lane_key =
                  std::to_string(ctx.stripe_id) + ":main:block" + std::to_string(blk);
              int lane = 0;
              auto it = block_lane_by_key.find(lane_key);
              if (it == block_lane_by_key.end()) {
                lane = static_cast<int>(block_lane_labels.size());
                block_lane_by_key[lane_key] = lane;
                block_lane_labels.push_back("S" + std::to_string(ctx.stripe_id) +
                                            " B" + std::to_string(blk) + " main");
              } else {
                lane = it->second;
              }
              block_events.push_back(
                  {lane, evt_start, evt_end, "#E74C3C",
                   "main cluster=" +
                       std::to_string(main_cluster_id),
                   evt_dec, evt_cross, evt_inner, evt_io, 0.0,
                   evt_phase_spans, evt_phase_total});
            }
          }
        };

    // 当前修复模式的 help_repair RPC 全局并发度（信号量容量）
    // 放在 run_one_stripe 外层，所有并行 stripe 共享同一个信号量，而不是每个 stripe 各自独立
    int help_rpc_cap;
    if (!schedule_cross_rack_links && !schedule_join_per_round) {
      help_rpc_cap = ec_schema_.flow_repair_parallel_unordered;
    } else if (schedule_cross_rack_links && !schedule_join_per_round) {
      help_rpc_cap = ec_schema_.flow_repair_parallel_ordered;
    } else {
      help_rpc_cap = ec_schema_.flow_repair_parallel_join_ordered;
    }
    if (help_rpc_cap <= 0) {
      help_rpc_cap = static_cast<int>(std::thread::hardware_concurrency());
      if (help_rpc_cap <= 0) help_rpc_cap = 4;
    }
    FifoSemaphore help_rpc_sem(help_rpc_cap);
    std::cout << "[SEM] Global help_rpc_sem created, capacity=" << help_rpc_cap
              << " mode=" << (schedule_cross_rack_links ?
                  (schedule_join_per_round ? "join_ordered" : "ordered") : "unordered")
              << std::endl;

    // 每个条带的 per-plan data port（避免同 proxy 上多 main_repair acceptor 冲突）
    std::vector<int> stripe_data_port;

    // 当需要修复一个条带时，会调用 run_one_stripe
    auto run_one_stripe = [&](int failure_stripe_ID) {
      int stripe_id = ordered_stripe_ids[failure_stripe_ID];
      std::vector<int>& failed_blocks =
          failures_map[static_cast<unsigned int>(stripe_id)];

      struct timeval start_time, end_time;
      struct timeval m_start_time, m_end_time;
      gettimeofday(&start_time, NULL);
      gettimeofday(&m_start_time, NULL);
      const double stripe_begin_sec = now_sec();
      Stripe& stripe = stripe_table_[static_cast<unsigned int>(stripe_id)];
      auto& other_vec = other_help_clusterID_chunkNum_pairs[failure_stripe_ID];
      bool main_help_cluster_flag = false;   // 新增变量：用于处理目的机架无可用块的情况
      bool flag = false;
      std::vector<RepairPlan> repair_plans;

      {
        // 条带级规划全盘串行：含 generate_partition；若只锁后半段，条带间并行时会并发
        // partition_random/print_info 与 MCMCF 向量，易竞态（乱码、偶发卡死）。RPC 仍在锁外并行。
        std::lock_guard<std::mutex> plan_lk(flow_shared_mcmcf_mutex);
        stripe.ec->placement_rule = ec_schema_.placement_rule;
        stripe.ec->generate_partition();
        find_out_stripe_partitions(static_cast<unsigned int>(stripe_id));
        int target_des_cluster = main_help_clusterID[failure_stripe_ID];
        std::unordered_map<int, size_t> cluster_to_idx;
        for (size_t idx = 0; idx < other_vec.size(); ++idx) {
          cluster_to_idx[other_vec[idx].first] = idx;
        }

        unsigned int partition_id = 0;
        int helper_cluster_find_num = 0;

        if (IF_DEBUG) {
          std::cout << "Stripe " << stripe.stripe_id << " block placement:\n";
        }

        for (auto& vec : stripe.ec->partition_plan) {
          unsigned int node_id = stripe.blocks2nodes[vec[0]];
          unsigned int cluster_id = node_table_[node_id].map2cluster;
          if (IF_DEBUG) {
            std::cout << cluster_id << ": ";
            for (int ele : vec) {
              std::cout << "B" << ele << "N" << stripe.blocks2nodes[ele] << " ";
            }
          }
          if (cluster_id == target_des_cluster) {
            if (IF_DEBUG) {
              std::cout << "  --> des_rack ";
            }
            main_help_clusterID[failure_stripe_ID] = partition_id;
            main_help_cluster_flag = true;
            auto it = cluster_to_idx.find(cluster_id);
            if (it != cluster_to_idx.end()) {
              if (IF_DEBUG) {
                std::cout << "  --> other_helper ";
              }
              other_vec[it->second].first = partition_id;
              helper_cluster_find_num++;
              cluster_to_idx.erase(it);
            }
          } else {
            auto it = cluster_to_idx.find(cluster_id);
            if (it != cluster_to_idx.end()) {
              if (IF_DEBUG) {
                std::cout << "  --> other_helper ";
              }
              other_vec[it->second].first = partition_id;
              helper_cluster_find_num++;
              cluster_to_idx.erase(it);
            }
          }
          if (IF_DEBUG) {
            std::cout << "\n";
          }
          partition_id++;
        }

        if (IF_DEBUG) {
          std::cout << "Generating repair plan for failures:" << std::endl;
          for (auto& failure : failed_blocks) {
            std::cout << failure << " ";
          }
          std::cout << std::endl;
        }

        if (helper_cluster_find_num != other_vec.size()) {
          std::cerr << "[ERROR] : other_help_clusterID_chunkNum_pairs not found all! "
                    << "Found " << helper_cluster_find_num
                    << ", expected " << other_vec.size() << std::endl;
        }
        if (!main_help_cluster_flag) {
          std::cerr << "[NOTE] : main_help_cluster has no chunk" << std::endl;
        }

        flag = stripe.ec->generate_flow_repair_plan(failed_blocks, repair_plans,
                                                    ec_schema_.partial_scheme,
                                                    ec_schema_.repair_priority,
                                                    ec_schema_.repair_method,
                                                    main_help_clusterID,
                                                    other_help_clusterID_chunkNum_pairs,
                                                    failure_stripe_ID,
                                                    main_help_cluster_flag);
      }

      if (!flag) {
        std::lock_guard<std::mutex> lk(stripe_accum_mutex);
        response.success = false;
        stripe_ok = false;
        return;
      }
      if (IF_DEBUG) {
        std::cout << "Repair Plan: " << std::endl;
        for (int i = 0; i < int(repair_plans.size()); i++) {
          RepairPlan& tmp = repair_plans[i];
          std::cout << "> Failed Blocks: ";
          for (int j = 0; 
               j < int(tmp.failure_idxs.size()); j++) {
            std::cout << tmp.failure_idxs[j] << " ";
          }
          std::cout << std::endl;
          std::cout << "> Repair by Blocks: ";
          for (auto& help_blocks : tmp.help_blocks) {
            for(auto& block : help_blocks) {
              std::cout << block << " ";
            }
          }
          std::cout << std::endl;
          std::cout << "> local_or_column: " << tmp.local_or_column << std::endl;
          std::cout << "> Parity idx: ";
          for (auto& idx : tmp.parity_idxs) {
            std::cout << idx << " ";
          }
          std::cout << std::endl;
        }
      }
      std::vector<MainRepairPlan> main_repairs;
      std::vector<std::vector<HelpRepairPlan>> help_repairs;
      // 生成具体的修复方案
      if (check_ec_family(ec_schema_.ec_type) == PCs) { // 乘积码
        concrete_repair_plans_pc(static_cast<unsigned int>(stripe_id), repair_plans,
            main_repairs, help_repairs);
      } else { // 通用处理
        int data_port = stripe_data_port[failure_stripe_ID];
        concrete_flow_repair_plans(static_cast<unsigned int>(stripe_id), repair_plans,
            main_repairs, help_repairs, main_help_clusterID_original[failure_stripe_ID], data_port);
      }
      
      if (IF_DEBUG) {
        std::cout << "Finish generate repair plan." << std::endl;
      }

      const int nm = int(main_repairs.size());
      // const int flow_rpc_timeout_sec = schedule_join_per_round ? 120 : 500;
      const int flow_rpc_timeout_sec = 500;
      std::vector<double> per_plan_main_dec(nm, 0.0);
      std::vector<double> per_plan_help_max_dec(nm, 0.0);
      std::vector<double> per_plan_main_net(nm, 0.0);
      std::vector<double> per_plan_help_max_net(nm, 0.0);
      std::vector<double> per_plan_main_inner_net(nm, 0.0);
      std::vector<double> per_plan_help_max_inner_net(nm, 0.0);
      std::vector<double> per_plan_main_io(nm, 0.0);
      std::vector<double> per_plan_help_max_io(nm, 0.0);
      std::mutex per_plan_help_max_mtx;

      // send_main_repair_plan 和 send_help_repair_plan 负责打包 RPC 请求发送给特定的 Proxy 节点，并等待返回耗时（decoding_time, net_time 等）
      auto send_main_repair_plan =
          [&, this, main_repairs, flow_rpc_timeout_sec](
              int i, int main_cluster_id) mutable {
            const double evt_start = now_sec();
            double evt_dec = 0.0;
            double evt_cross = 0.0;
            double evt_inner = 0.0;
            double evt_io = 0.0;
            std::vector<RepairPhaseSpan> evt_phase_spans;
            double evt_phase_total = 0.0;
            try
            {
              std::string chosen_proxy = proxy_endpoint_key(
                  cluster_table_[main_cluster_id].proxy_ip,
                  cluster_table_[main_cluster_id].proxy_port);
              auto& task_client = tls_proxy_client(
                  chosen_proxy,
                  cluster_table_[main_cluster_id].proxy_ip,
                  cluster_table_[main_cluster_id].proxy_port);
              auto r = async_simple::coro::syncAwait(
                  task_client.call_for<&Proxy::main_repair>(
                      std::chrono::seconds{flow_rpc_timeout_sec}, main_repairs[i]));
              if (!r) {
                std::cerr << "[RPC ERROR] main_repair failed to proxy " << chosen_proxy
                          << ": " << r.error().msg << std::endl;
                throw std::runtime_error("main_repair rpc failed");
              }
              auto resp = r.value();
              evt_dec = resp.decoding_time;
              evt_cross = resp.cross_cluster_time;
              evt_inner = resp.inner_network_time;
              evt_io = resp.io_time;
              evt_phase_spans = resp.phase_spans;
              evt_phase_total = resp.phase_total_time;
              per_plan_main_dec[static_cast<size_t>(i)] = evt_dec;
              per_plan_main_net[static_cast<size_t>(i)] = evt_cross;
              per_plan_main_inner_net[static_cast<size_t>(i)] = evt_inner;
              per_plan_main_io[static_cast<size_t>(i)] = evt_io;
              if (IF_DEBUG) {
                std::cout << "Selected main proxy " << chosen_proxy
                          << " of cluster" << main_cluster_id << " plan "
                          << i << ". Decoding time : " << resp.decoding_time
                          << std::endl;
              }
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
            const double evt_end = now_sec();
            if (IF_DEBUG && unordered_concurrency_main_repairs) {
              std::lock_guard<std::mutex> lk(timeline_mutex);
              for (int blk : main_repairs[i].failed_blocks_index) {
                const std::string lane_key =
                    std::to_string(stripe_id) + ":main:block" + std::to_string(blk);
                int lane = 0;
                auto it = block_lane_by_key.find(lane_key);
                if (it == block_lane_by_key.end()) {
                  lane = static_cast<int>(block_lane_labels.size());
                  block_lane_by_key[lane_key] = lane;
                  block_lane_labels.push_back("S" + std::to_string(stripe_id) +
                                              " B" + std::to_string(blk) + " main");
                } else {
                  lane = it->second;
                }
                block_events.push_back(
                    {lane, evt_start, evt_end, "#E74C3C",
                     "main cluster=" +
                         std::to_string(main_cluster_id),
                     evt_dec, evt_cross, evt_inner, evt_io, 0.0,
                     evt_phase_spans, evt_phase_total});
              }
            }
          };

      auto send_help_repair_plan =
          [&, this, help_repairs, flow_rpc_timeout_sec](
              int i, int j, std::string proxy_ip, int proxy_port,
              int round_id, int src_cluster_1b, int dst_cluster_1b, bool scheduled_dispatch,
              int dispatch_seq) mutable {
            const double evt_start = now_sec();
            double evt_dec = 0.0;
            double evt_cross = 0.0;
            double evt_inner = 0.0;
            double evt_io = 0.0;
            std::vector<RepairPhaseSpan> evt_phase_spans;
            double evt_phase_total = 0.0;
            try
            {
              std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
              auto& hlpc = tls_proxy_client(chosen_proxy, proxy_ip, proxy_port);
              auto r = async_simple::coro::syncAwait(
                  hlpc.call_for<&Proxy::help_repair>(
                      std::chrono::seconds{flow_rpc_timeout_sec}, help_repairs[i][j]));
              if (!r) {
                std::cerr << "[RPC ERROR] help_repair failed to proxy " << chosen_proxy
                          << ": " << r.error().msg << std::endl;
                throw std::runtime_error("help_repair rpc failed");
              }
              auto resp = r.value();
              evt_dec = resp.decoding_time;
              evt_cross = resp.cross_cluster_time;
              evt_inner = resp.inner_network_time;
              evt_io = resp.io_time;
              evt_phase_spans = resp.phase_spans;
              evt_phase_total = resp.phase_total_time;
              {
                std::lock_guard<std::mutex> lk(per_plan_help_max_mtx);
                per_plan_help_max_dec[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_dec[static_cast<size_t>(i)], evt_dec);
                per_plan_help_max_net[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_net[static_cast<size_t>(i)], evt_cross);
                per_plan_help_max_inner_net[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_inner_net[static_cast<size_t>(i)],
                             evt_inner);
                per_plan_help_max_io[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_io[static_cast<size_t>(i)], evt_io);
              }
              if (IF_DEBUG) {
                std::cout << "Selected help proxy " << chosen_proxy
                          << std::endl;
              }
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
            const double evt_end = now_sec();
            if (IF_DEBUG && unordered_concurrency_main_repairs) {
              std::lock_guard<std::mutex> lk(timeline_mutex);
              for (const auto& bi : help_repairs[i][j].inner_cluster_help_blocks_info) {
                const int blk = bi.first;
                const std::string lane_key =
                    std::to_string(stripe_id) + ":help:block" + std::to_string(blk);
                int lane = 0;
                auto it = block_lane_by_key.find(lane_key);
                if (it == block_lane_by_key.end()) {
                  lane = static_cast<int>(block_lane_labels.size());
                  block_lane_by_key[lane_key] = lane;
                  block_lane_labels.push_back("S" + std::to_string(stripe_id) +
                                              " B" + std::to_string(blk) + " help");
                } else {
                  lane = it->second;
                }
                std::ostringstream oss;
                oss << "#" << dispatch_seq << " ";
                if (round_id >= 0) {
                  oss << "r" << round_id << " ";
                }
                if (scheduled_dispatch) {
                  oss << src_cluster_1b << "->" << dst_cluster_1b;
                }
                static const char* kRoundColors[] = {
                    "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
                };
                const std::string c = (round_id >= 0)
                    ? kRoundColors[static_cast<size_t>(round_id) % 6]
                    : "#7FB3D5";
                block_events.push_back(
                    {lane, evt_start, evt_end,
                     c, oss.str(), evt_dec, evt_cross, evt_inner, evt_io, 0.0,
                     evt_phase_spans, evt_phase_total});
              }
            }
          };
      auto make_local_help_task_id = [&](int i, int j) {
        return std::to_string(stripe_id) + ":" + std::to_string(i) + ":" +
               std::to_string(j);
      };
      auto prepare_help_repair_plan =
          [&, this, help_repairs, flow_rpc_timeout_sec, make_local_help_task_id](
              int i, int j, std::string proxy_ip, int proxy_port,
              int round_id, int src_cluster_1b, int dst_cluster_1b, int dispatch_seq) mutable {
            const double evt_start = now_sec();
            double evt_dec = 0.0;
            double evt_inner = 0.0;
            double evt_io = 0.0;
            std::vector<RepairPhaseSpan> evt_phase_spans;
            double evt_phase_total = 0.0;
            try
            {
              std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
              HelpRepairPrepareReq req;
              req.repair_plan = help_repairs[i][j];
              req.task_id = make_local_help_task_id(i, j);
              auto r = [&]() {
                // 使用独立的 prepare 通道，与同一 proxy 的 send 通道不互斥，
                // 确保单条带内 N 轮跨机架发送与 N+1 轮机架内读取/编码并行。
                std::lock_guard<std::mutex> plk(
                    *help_prepare_proxy_mutexes_[chosen_proxy]);
                return async_simple::coro::syncAwait(
                    help_prepare_proxies_[chosen_proxy]->call_for<&Proxy::prepare_help_repair_data>(
                        std::chrono::seconds{flow_rpc_timeout_sec}, req));
              }();
              if (!r) {
                std::cerr << "[RPC ERROR] prepare_help_repair_data failed to proxy "
                          << chosen_proxy << ": " << r.error().msg << std::endl;
                throw std::runtime_error("prepare_help_repair_data rpc failed");
              }
              auto prep_resp = r.value();
              if (!prep_resp.success) {
                std::cerr << "[RPC ERROR] prepare_help_repair_data rejected by proxy "
                          << chosen_proxy << ": " << prep_resp.err_msg << std::endl;
                throw std::runtime_error("prepare_help_repair_data proxy returned failed");
              }
              evt_dec = prep_resp.decoding_time;
              evt_inner = prep_resp.inner_network_time;
              evt_io = prep_resp.io_time;
              evt_phase_spans = prep_resp.phase_spans;
              evt_phase_total = prep_resp.phase_total_time;
              {
                std::lock_guard<std::mutex> lk(per_plan_help_max_mtx);
                per_plan_help_max_dec[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_dec[static_cast<size_t>(i)], evt_dec);
                per_plan_help_max_inner_net[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_inner_net[static_cast<size_t>(i)],
                             evt_inner);
                per_plan_help_max_io[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_io[static_cast<size_t>(i)], evt_io);
              }
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
            const double evt_end = now_sec();
            if (IF_DEBUG && unordered_concurrency_main_repairs) {
              std::lock_guard<std::mutex> lk(timeline_mutex);
              for (const auto& bi : help_repairs[i][j].inner_cluster_help_blocks_info) {
                const int blk = bi.first;
                const std::string lane_key =
                    std::to_string(stripe_id) + ":help:block" + std::to_string(blk);
                int lane = 0;
                auto it = block_lane_by_key.find(lane_key);
                if (it == block_lane_by_key.end()) {
                  lane = static_cast<int>(block_lane_labels.size());
                  block_lane_by_key[lane_key] = lane;
                  block_lane_labels.push_back("S" + std::to_string(stripe_id) +
                                              " B" + std::to_string(blk) + " help");
                } else {
                  lane = it->second;
                }
                std::ostringstream oss;
                oss << "#" << dispatch_seq << " r" << round_id << " "
                    << src_cluster_1b << "->" << dst_cluster_1b << " prepare";
                static const char* kRoundColors[] = {
                    "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
                };
                const std::string c =
                    kRoundColors[static_cast<size_t>(round_id >= 0 ? round_id : 0) % 6];
                block_events.push_back(
                    {lane, evt_start, evt_end, c, oss.str(), evt_dec, 0.0, evt_inner, evt_io,
                     0.0, evt_phase_spans, evt_phase_total});
              }
            }
          };
      auto send_prepared_help_repair_plan =
          [&, this, flow_rpc_timeout_sec, make_local_help_task_id](
              int i, int j, std::string proxy_ip, int proxy_port,
              int round_id, int src_cluster_1b, int dst_cluster_1b, int dispatch_seq) mutable {
            const double evt_start = now_sec();
            double evt_cross = 0.0;
            std::vector<RepairPhaseSpan> evt_phase_spans;
            double evt_phase_total = 0.0;
            try
            {
              std::string chosen_proxy = proxy_endpoint_key(proxy_ip, proxy_port);
              HelpRepairSendReq req;
              req.task_id = make_local_help_task_id(i, j);
              auto r = [&]() {
                // 使用独立的 send 通道，与 prepare 通道的锁互不干扰。
                std::lock_guard<std::mutex> plk(
                    *help_send_proxy_mutexes_[chosen_proxy]);
                return async_simple::coro::syncAwait(
                    help_send_proxies_[chosen_proxy]->call_for<&Proxy::send_prepared_help_repair_data>(
                        std::chrono::seconds{flow_rpc_timeout_sec}, req));
              }();
              if (!r) {
                std::cerr << "[RPC ERROR] send_prepared_help_repair_data failed to proxy "
                          << chosen_proxy << ": " << r.error().msg << std::endl;
                throw std::runtime_error("send_prepared_help_repair_data rpc failed");
              }
              auto send_resp = r.value();
              if (!send_resp.success) {
                std::cerr << "[RPC ERROR] send_prepared_help_repair_data rejected by proxy "
                          << chosen_proxy << ": " << send_resp.err_msg << std::endl;
                throw std::runtime_error("send_prepared_help_repair_data proxy returned failed");
              }
              evt_cross = send_resp.cross_cluster_time;
              evt_phase_spans = send_resp.phase_spans;
              evt_phase_total = send_resp.phase_total_time;
              {
                std::lock_guard<std::mutex> lk(per_plan_help_max_mtx);
                per_plan_help_max_net[static_cast<size_t>(i)] =
                    std::max(per_plan_help_max_net[static_cast<size_t>(i)], evt_cross);
              }
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
            const double evt_end = now_sec();
            if (IF_DEBUG && unordered_concurrency_main_repairs) {
              std::lock_guard<std::mutex> lk(timeline_mutex);
              for (const auto& bi : help_repairs[i][j].inner_cluster_help_blocks_info) {
                const int blk = bi.first;
                const std::string lane_key =
                    std::to_string(stripe_id) + ":help:block" + std::to_string(blk);
                int lane = 0;
                auto it = block_lane_by_key.find(lane_key);
                if (it == block_lane_by_key.end()) {
                  lane = static_cast<int>(block_lane_labels.size());
                  block_lane_by_key[lane_key] = lane;
                  block_lane_labels.push_back("S" + std::to_string(stripe_id) +
                                              " B" + std::to_string(blk) + " help");
                } else {
                  lane = it->second;
                }
                std::ostringstream oss;
                oss << "#" << dispatch_seq << " r" << round_id << " "
                    << src_cluster_1b << "->" << dst_cluster_1b << " send";
                static const char* kRoundColors[] = {
                    "#2E86DE", "#E67E22", "#16A085", "#8E44AD", "#C0392B", "#2C3E50"
                };
                const std::string c =
                    kRoundColors[static_cast<size_t>(round_id >= 0 ? round_id : 0) % 6];
                block_events.push_back(
                    {lane, evt_start, evt_end, c, oss.str(), 0.0, evt_cross, 0.0, 0.0, 0.0,
                     evt_phase_spans, evt_phase_total});
              }
            }
          };

      // simulation-统计理论上的跨集群传输量、IO次数
      {
        std::lock_guard<std::mutex> lk(stripe_accum_mutex);
        simulation_repair(main_repairs, cross_cluster_transfers, io_cnt);
      }
      if (IF_DEBUG) {
        std::cout << "Finish simulation! " << cross_cluster_transfers << std::endl;
      }
      gettimeofday(&m_end_time, NULL);
      double meta_delta = m_end_time.tv_sec - m_start_time.tv_sec +
          (m_end_time.tv_usec - m_start_time.tv_usec) * 1.0 / 1000000;
      flow_stripe_meta[failure_stripe_ID] = meta_delta;

      // 【重要分支】：如果开启了全局调度，这里绝对不发请求！
      // 而是将刚才算好的 main_repairs 和 help_repairs 塞进 global_ctx 结构体里，
      // 把帮手跨机架的边推入 global_link2tasks 中，然后直接 return。
      if (enable_global_schedule) {
        // ordered / join-ordered：此处只收集全局 help 任务，不在此发送 main_repair。
        // main 在 proxy 侧会对跨集群路径 accept 阻塞，若在此 join main 而 help 尚未下发，会永久死锁。
        {
          std::lock_guard<std::mutex> lk(global_schedule_mutex);
          StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(failure_stripe_ID)];
          ctx.ready = true;
          ctx.failure_stripe_ID = failure_stripe_ID;
          ctx.stripe_id = stripe_id;
          ctx.failed_blocks = failed_blocks;
          ctx.main_repairs = std::move(main_repairs);
          ctx.help_repairs = std::move(help_repairs);
          ctx.per_plan_main_dec = per_plan_main_dec;
          ctx.per_plan_main_net = per_plan_main_net;
          ctx.per_plan_main_inner_net = per_plan_main_inner_net;
          ctx.per_plan_main_io = per_plan_main_io;
          ctx.per_plan_help_max_dec.assign(ctx.per_plan_main_dec.size(), 0.0);
          ctx.per_plan_help_max_net.assign(ctx.per_plan_main_dec.size(), 0.0);
          ctx.per_plan_help_max_inner_net.assign(ctx.per_plan_main_dec.size(), 0.0);
          ctx.per_plan_help_max_io.assign(ctx.per_plan_main_dec.size(), 0.0);
          ctx.stripe_begin_sec = stripe_begin_sec;

          for (int i = 0; i < static_cast<int>(ctx.main_repairs.size()); ++i) {
            MainRepairPlan& tmp = ctx.main_repairs[i];
            const int main_cluster_id = static_cast<int>(tmp.cluster_id);
            for (int j = 0; j < static_cast<int>(tmp.help_clusters_blocks_info.size()); ++j) {
              int num_of_blocks_in_help_cluster =
                  static_cast<int>(tmp.help_clusters_blocks_info[j].size());
              my_assert(num_of_blocks_in_help_cluster ==
                  static_cast<int>(ctx.help_repairs[i][j].inner_cluster_help_blocks_info.size()));
              bool t_flag = tmp.help_clusters_partial_less[j];
              if ((IF_DIRECT_FROM_NODE && ec_schema_.partial_decoding && t_flag) ||
                  !IF_DIRECT_FROM_NODE)
              {
                const unsigned int helper_cluster_id = ctx.help_repairs[i][j].cluster_id;
                const int src = static_cast<int>(helper_cluster_id) + 1;
                const int dst = main_cluster_id + 1;
                global_link2tasks[{src, dst}].push_back({failure_stripe_ID, i, j});
              }
            }
          }
        }
        return;
      }

      // help_rpc_sem 已在外层创建，所有 stripe 共享同一个全局信号量
      if (!IF_SIMULATION) {
        // unordered_concurrency_main_repairs: true-并行，false-串行
          // schedule_cross_rack_links: false-无序并行，true-有序并行
            // schedule_join_per_round: false-理论有序并行，true-调度轮次同步并行
        if (unordered_concurrency_main_repairs) {
          // 每个 MainRepairPlan 内部仍为主线程 + 多 help 线程协作；
          // 多个 Plan 之间无序并发，所有 Plan 同时运行取关键路径。
          // 仅调整 help_repair 的跨机架发送顺序；main_repair 仍优先并发发出以便 main proxy 准备接收。
          auto run_batch_with_optional_scheduling = [&](int base, int batch_end) {
            try
            {
              std::vector<std::thread> main_threads;
              main_threads.reserve((size_t)(batch_end - base));
              // key=(srcCid1b,dstCid1b) -> tasks(i,j)
              std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> link2tasks;
              // unordered 模式下可下发任务的自然顺序（按 i,j 收集顺序）
              std::vector<std::pair<int, int>> ordered_tasks;

              std::cout << "[SCHEDULE] main_repair_batch=[" << base << "," << batch_end
                        << ") schedule_cross_rack_links=" << (schedule_cross_rack_links ? 1 : 0)
                        << " schedule_join_per_round=" << (schedule_join_per_round ? 1 : 0)
                        << " rpc_timeout_sec=" << flow_rpc_timeout_sec
                        << std::endl;

              for (int i = base; i < batch_end; ++i) {
                MainRepairPlan& tmp = main_repairs[i];
                unsigned int main_cluster_id = tmp.cluster_id;
                main_threads.emplace_back(send_main_repair_plan, i, (int)main_cluster_id);

                for (int j = 0; j < int(tmp.help_clusters_blocks_info.size()); j++) {
                  int num_of_blocks_in_help_cluster =
                      (int)tmp.help_clusters_blocks_info[j].size();
                  my_assert(num_of_blocks_in_help_cluster ==
                      int(help_repairs[i][j].inner_cluster_help_blocks_info.size()));
                  bool t_flag = tmp.help_clusters_partial_less[j];
                  if ((IF_DIRECT_FROM_NODE && ec_schema_.partial_decoding && t_flag) ||
                      !IF_DIRECT_FROM_NODE)
                  {
                    const unsigned int helper_cluster_id = help_repairs[i][j].cluster_id;
                    // 统一用 1-based 以复用 scheduleRepairLink 的实现
                    const int src = (int)helper_cluster_id + 1;
                    const int dst = (int)main_cluster_id + 1;
                    ordered_tasks.push_back({i, j});
                    link2tasks[{src, dst}].push_back({i, j});
                  }
                }
              }

              auto send_one_help_task = [&](int i, int j) {
                Cluster& cluster = cluster_table_[help_repairs[i][j].cluster_id];
                const int seq = ++help_dispatch_seq;
                send_help_repair_plan(i, j, cluster.proxy_ip, cluster.proxy_port,
                                      -1, -1, -1, false, seq);
              };

            
              // 根据 schedule_cross_rack_links 的值：
              // 如果为 false，全部线程一起 start 发送；
              // 如果为 true，就调用 RepairScheduler 对局部链路做匹配生成 Round，按照 Round 依次发射线程。
              if (!schedule_cross_rack_links || link2tasks.empty()) {
                // unordered 模式：不调用 compute_schedule，直接按 (i,j) 自然顺序下发
                // 使用全局 FifoSemaphore 限制并发 help_repair RPC 数量
                std::vector<std::thread> help_threads;
                help_threads.reserve(ordered_tasks.size());
                for (const auto& task : ordered_tasks) {
                  help_threads.emplace_back([&, task]() {
                    help_rpc_sem.acquire();
                    send_one_help_task(task.first, task.second);
                    help_rpc_sem.release();
                  });
                }
                std::cout << "[SCHEDULE] baseline_help_threads=" << help_threads.size()
                          << " (no cross-rack scheduling)" << std::endl;
                for (auto& t : help_threads) t.join();
              } else {
                // ordered 模式：按 compute_schedule 的轮次顺序创建线程并派发跨机架 help_repair。
                // - schedule_join_per_round=true：每轮 join 同步（必须等这一轮全部完成才进入下一轮）
                // - schedule_join_per_round=false：线程按轮次顺序创建，末尾统一 join（join ordered）
                std::vector<ScheduledLink> links;
                links.reserve(link2tasks.size());
                for (const auto& kv : link2tasks) {
                  const int src = kv.first.first;
                  const int dst = kv.first.second;
                  const int w = (int)kv.second.size(); // 以 help_repair 次数为权重
                  links.push_back({src, dst, w, false});
                }
                RepairScheduler scheduler;
                auto rounds = scheduler.compute_schedule(std::move(links));

                if (schedule_join_per_round) {
                  struct LocalRoundTask {
                    int plan_i = -1;
                    int help_j = -1;
                    int src = -1;
                    int dst = -1;
                    std::string proxy_ip;
                    int proxy_port = 0;
                  };
                  std::vector<std::vector<LocalRoundTask>> round_task_table;
                  round_task_table.reserve(rounds.size());

                  int round_id = 0;
                  for (const auto& round : rounds) {
                    size_t round_dispatch = 0;
                    std::vector<LocalRoundTask> round_tasks;
                    std::map<std::pair<int, int>, int> round_edge_dispatch;
                    for (const auto& e : round.edges) {
                      auto& tasks = link2tasks[{e.src, e.dst}];
                      const int take = std::min(e.weight, (int)tasks.size());
                      for (int t = 0; t < take; ++t) {
                        auto one = tasks.back();
                        tasks.pop_back();
                        Cluster& cluster = cluster_table_[help_repairs[one.first][one.second].cluster_id];
                        round_tasks.push_back(
                            {one.first, one.second, e.src, e.dst, cluster.proxy_ip,
                             cluster.proxy_port});
                      }
                      round_dispatch += static_cast<size_t>(take);
                      round_edge_dispatch[{e.src, e.dst}] += take;
                    }
                    round_task_table.push_back(std::move(round_tasks));
                    std::cout << "[SCHEDULE] round=" << round_id
                              << " edges=" << round.edges.size()
                              << " dispatch_threads=" << round_dispatch
                              << " join_per_round=1 pipeline=1" << std::endl;
                    if (IF_DEBUG) {
                      std::ostringstream ss;
                      ss << "round=" << round_id << " ";
                      for (const auto& kv : round_edge_dispatch) {
                        ss << "(" << kv.first.first << "->" << kv.first.second
                           << ":" << kv.second << ") ";
                      }
                      {
                        std::lock_guard<std::mutex> lk(timeline_mutex);
                        schedule_notes.push_back(ss.str());
                      }
                    }
                    if (IF_DEBUG) {
                      std::cout << "[SCHEDULE] round=" << round_id << " dispatch_by_link: ";
                      for (const auto& kv : round_edge_dispatch) {
                        std::cout << "(" << kv.first.first << "->" << kv.first.second
                                  << ":" << kv.second << ") ";
                      }
                      std::cout << std::endl;
                    }
                    ++round_id;
                  }

                  auto launch_prepare_round = [&](int rid) {
                    std::vector<std::thread> prep_threads;
                    const auto& tasks = round_task_table[static_cast<size_t>(rid)];
                    prep_threads.reserve(tasks.size());
                    for (const auto& t : tasks) {
                      const int seq = ++help_dispatch_seq;
                      prep_threads.emplace_back(
                          prepare_help_repair_plan, t.plan_i, t.help_j, t.proxy_ip, t.proxy_port,
                          rid, t.src, t.dst, seq);
                    }
                    for (auto& th : prep_threads) {
                      if (th.joinable()) {
                        th.join();
                      }
                    }
                  };

                  // 预热 round-0：先把第一轮数据准备好，再进入第 0 轮发送。
                  if (!round_task_table.empty()) {
                    launch_prepare_round(0);
                  }

                  for (int rid = 0; rid < static_cast<int>(round_task_table.size()); ++rid) {
                    std::vector<std::thread> send_threads;
                    const auto& send_tasks = round_task_table[static_cast<size_t>(rid)];
                    send_threads.reserve(send_tasks.size());
                    for (const auto& t : send_tasks) {
                      const int seq = ++help_dispatch_seq;
                      send_threads.emplace_back(
                          send_prepared_help_repair_plan, t.plan_i, t.help_j, t.proxy_ip,
                          t.proxy_port, rid, t.src, t.dst, seq);
                    }

                    std::vector<std::thread> next_prepare_threads;
                    if (rid + 1 < static_cast<int>(round_task_table.size())) {
                      const auto& prep_tasks =
                          round_task_table[static_cast<size_t>(rid + 1)];
                      next_prepare_threads.reserve(prep_tasks.size());
                      for (const auto& t : prep_tasks) {
                        const int seq = ++help_dispatch_seq;
                        next_prepare_threads.emplace_back(
                            prepare_help_repair_plan, t.plan_i, t.help_j, t.proxy_ip, t.proxy_port,
                            rid + 1, t.src, t.dst, seq);
                      }
                    }

                    for (auto& th : send_threads) {
                      if (th.joinable()) {
                        th.join();
                      }
                    }
                    for (auto& th : next_prepare_threads) {
                      if (th.joinable()) {
                        th.join();
                      }
                    }
                    if (!stripe_ok.load()) {
                      break;
                    }
                  }
                } else {
                  std::vector<std::thread> help_threads;
                  help_threads.reserve(link2tasks.size());
                  int round_id = 0;
                  for (const auto& round : rounds) {
                    size_t round_dispatch = 0;
                    std::map<std::pair<int, int>, int> round_edge_dispatch;
                    for (const auto& e : round.edges) {
                      auto& tasks = link2tasks[{e.src, e.dst}];
                      const int take = std::min(e.weight, (int)tasks.size());
                      for (int t = 0; t < take; ++t) {
                        auto one = tasks.back();
                        tasks.pop_back();
                        Cluster& cluster = cluster_table_[help_repairs[one.first][one.second].cluster_id];
                        const int seq = ++help_dispatch_seq;
                        help_threads.emplace_back([&, one, proxy_ip = cluster.proxy_ip,
                                                   proxy_port = cluster.proxy_port,
                                                   round_id, src = e.src, dst = e.dst, seq]() {
                          help_rpc_sem.acquire();
                          send_help_repair_plan(one.first, one.second, proxy_ip, proxy_port,
                                                round_id, src, dst, true, seq);
                          help_rpc_sem.release();
                        });
                      }
                      round_dispatch += static_cast<size_t>(take);
                      round_edge_dispatch[{e.src, e.dst}] += take;
                    }
                    std::cout << "[SCHEDULE] round=" << round_id
                              << " edges=" << round.edges.size()
                              << " dispatch_threads=" << round_dispatch
                              << " join_per_round=0" << std::endl;
                    if (IF_DEBUG) {
                      std::ostringstream ss;
                      ss << "round=" << round_id << " ";
                      for (const auto& kv : round_edge_dispatch) {
                        ss << "(" << kv.first.first << "->" << kv.first.second
                           << ":" << kv.second << ") ";
                      }
                      {
                        std::lock_guard<std::mutex> lk(timeline_mutex);
                        schedule_notes.push_back(ss.str());
                      }
                    }
                    if (IF_DEBUG) {
                      std::cout << "[SCHEDULE] round=" << round_id << " dispatch_by_link: ";
                      for (const auto& kv : round_edge_dispatch) {
                        std::cout << "(" << kv.first.first << "->" << kv.first.second
                                  << ":" << kv.second << ") ";
                      }
                      std::cout << std::endl;
                    }
                    ++round_id;
                  }
                  std::cout << "[SCHEDULE] all_rounds_dispatched help_threads="
                            << help_threads.size() << " (join at end)" << std::endl;
                  for (auto& t : help_threads) {
                    if (t.joinable()) {
                      t.join();
                    }
                  }
                }
              }

              for (auto& t : main_threads) t.join();
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
          };

          // 需求：compute_schedule 全局只执行一次，让当前条带内所有修复任务统一参与调度计算。
          // 因此在启用 unordered_concurrency_main_repairs 时，不再按 batch 切分调度，
          // 而是一次性将 [0, nm) 全量 MainRepairPlan 纳入同一个 schedule。
          run_batch_with_optional_scheduling(0, nm);
          if (!stripe_ok.load()) return;
        } else {
          for (int i = 0; i < nm; i++) {
            try
            {
              MainRepairPlan& tmp = main_repairs[i];
              int failed_num = int(tmp.failed_blocks_index.size());
              unsigned int main_cluster_id = tmp.cluster_id;
              // 多线程发送修复任务
              std::thread my_main_thread(send_main_repair_plan, i, main_cluster_id);
              std::vector<std::thread> senders;
              int index = 0;
              for (int j = 0; j < int(tmp.help_clusters_blocks_info.size()); j++) {
                int num_of_blocks_in_help_cluster =
                    (int)tmp.help_clusters_blocks_info[j].size();
                my_assert(num_of_blocks_in_help_cluster ==
                    int(help_repairs[i][j].inner_cluster_help_blocks_info.size()));
                bool t_flag = tmp.help_clusters_partial_less[j];
                if ((IF_DIRECT_FROM_NODE && ec_schema_.partial_decoding
                     && t_flag) ||
                    !IF_DIRECT_FROM_NODE)
                {
                  Cluster &cluster = cluster_table_[help_repairs[i][j].cluster_id];
                  const int seq = ++help_dispatch_seq;
                  senders.push_back(std::thread(send_help_repair_plan, i, j,
                                    cluster.proxy_ip, cluster.proxy_port,
                                    -1, -1, -1, false, seq));
                }
              }
              for (int j = 0; j < int(senders.size()); j++) {
                senders[j].join();
              }
              my_main_thread.join();
            }
            catch (...)
            {
              stripe_ok.store(false);
            }
          }
        }
      }

      double stripe_d = 0;
      double stripe_c = 0;
      double stripe_inner = 0;
      double stripe_io = 0;
      if (!IF_SIMULATION && nm > 0) {
        if (unordered_concurrency_main_repairs) {
          for (int i = 0; i < nm; ++i) {
            const size_t ui = static_cast<size_t>(i);
            stripe_d = std::max(stripe_d, std::max(per_plan_main_dec[ui], per_plan_help_max_dec[ui]));
            stripe_c = std::max(stripe_c, std::max(per_plan_main_net[ui], per_plan_help_max_net[ui]));
            stripe_inner = std::max(stripe_inner, std::max(per_plan_main_inner_net[ui], per_plan_help_max_inner_net[ui]));
            stripe_io = std::max(stripe_io, std::max(per_plan_main_io[ui], per_plan_help_max_io[ui]));
          }
        } else {
          for (int i = 0; i < nm; ++i) {
            const size_t ui = static_cast<size_t>(i);
            const double plan_dec = std::max(per_plan_main_dec[ui], per_plan_help_max_dec[ui]);
            const double plan_net = std::max(per_plan_main_net[ui], per_plan_help_max_net[ui]);
            const double plan_inner =
                std::max(per_plan_main_inner_net[ui], per_plan_help_max_inner_net[ui]);
            const double plan_io = std::max(per_plan_main_io[ui], per_plan_help_max_io[ui]);
            stripe_d += plan_dec;
            stripe_c += plan_net;
            stripe_inner += plan_inner;
            stripe_io += plan_io;
          }
        }
      }
      flow_stripe_decode[failure_stripe_ID] = stripe_d;
      flow_stripe_cross[failure_stripe_ID] = stripe_c;
      flow_stripe_inner_net[failure_stripe_ID] = stripe_inner;
      flow_stripe_io[failure_stripe_ID] = stripe_io;

      // 更新元数据
      for (int i = 0; i < int(main_repairs.size()); i++) {
        MainRepairPlan& tmp = main_repairs[i];
        int j = 0;
        for (auto& idx : tmp.failed_blocks_index) {
          stripe.blocks2nodes[idx] = tmp.new_locations[j++].first;
        }
      }
      gettimeofday(&end_time, NULL);
      double temp_time = end_time.tv_sec - start_time.tv_sec +
          (end_time.tv_usec - start_time.tv_usec) * 1.0 / 1000000;
      const double stripe_end_sec = now_sec();
      {
        std::lock_guard<std::mutex> lk(stripe_accum_mutex);
        stripe_durations_sum += temp_time;
        std::cout << "Repair[ ";
        for (auto& failure : failed_blocks) {
          std::cout << failure << " ";
        }
        std::cout << "]: sum_stripe_durations = " << stripe_durations_sum << "s, this_stripe = "
                  << temp_time << "s. Decode/Cross/InnerNet/IO(agg): "
                  << stripe_d << " / " << stripe_c << " / " << stripe_inner
                  << " / " << stripe_io
                  << std::endl;
      }
      {
        std::lock_guard<std::mutex> lk(timeline_mutex);
        int lane = 0;
        auto it_lane = stripe_lane_by_id.find(stripe_id);
        if (it_lane == stripe_lane_by_id.end()) {
          lane = static_cast<int>(stripe_lane_labels.size());
          stripe_lane_by_id[stripe_id] = lane;
          stripe_lane_labels.push_back("stripe " + std::to_string(stripe_id));
        } else {
          lane = it_lane->second;
        }
        TimelineEvent e;
        e.lane = lane;
        e.start_sec = stripe_begin_sec;
        e.end_sec = stripe_end_sec;
        e.color = schedule_cross_rack_links ? "#4F8EF7" : "#5CB85C";
        e.label = "stripe " + std::to_string(stripe_id);
        e.decode_sec = stripe_d;
        e.cross_sec = stripe_c;
        e.inner_sec = stripe_inner;
        e.io_sec = stripe_io;
        e.meta_sec = flow_stripe_meta[failure_stripe_ID];
        stripe_events.push_back(std::move(e));
      }
    };

    auto finish_flow_stripe_phase_wall = [&]() {
      gettimeofday(&flow_stripe_phase_end, NULL);
      repair_time =
          (flow_stripe_phase_end.tv_sec - flow_stripe_phase_start.tv_sec) +
          (flow_stripe_phase_end.tv_usec - flow_stripe_phase_start.tv_usec) * 1.0 /
          1000000;
    };

    /** 统计口径：
     *  - nodes_flow_repair（parallel_stripes=false）：多条带累加和（保留原口径）
     *  - 三种并行修复（parallel_stripes=true）：关键路径口径（所有修复任务中最长）
     */
    auto finalize_flow_timing_metrics = [&]() {
      decoding_time = 0;
      cross_cluster_time = 0;
      inner_network_time = 0;
      io_time = 0;
      meta_time = 0;
      const size_t ns = ordered_stripe_ids.size();
      if (!parallel_stripes) {
        for (size_t k = 0; k < ns; ++k) {
          decoding_time += flow_stripe_decode[k];
          cross_cluster_time += flow_stripe_cross[k];
          inner_network_time += flow_stripe_inner_net[k];
          io_time += flow_stripe_io[k];
          meta_time += flow_stripe_meta[k];
        }
      } else {
        for (size_t k = 0; k < ns; ++k) {
          decoding_time = std::max(decoding_time, flow_stripe_decode[k]);
          cross_cluster_time = std::max(cross_cluster_time, flow_stripe_cross[k]);
          inner_network_time = std::max(inner_network_time, flow_stripe_inner_net[k]);
          io_time = std::max(io_time, flow_stripe_io[k]);
          meta_time = std::max(meta_time, flow_stripe_meta[k]);
        }
      }
    };

    // RepairResp 默认 success=false；条带循环内仅在 generate_flow_repair_plan 失败时置 false。
    // 若此处不先置 true，串行路径会在第一条带成功后仍因 !response.success 提前退出，后续条带不会被修复。
    response.success = true;

    gettimeofday(&flow_stripe_phase_start, NULL);
    // 为每条待修条带分配独立的 data_port，避免同 proxy 上多 main_repair acceptor 冲突
    {
      const size_t nstripes = ordered_stripe_ids.size();
      stripe_data_port.resize(nstripes);
      {
        std::map<std::string, int> proxy_port_offset;
        for (size_t k = 0; k < nstripes; ++k) {
          int main_cid = main_help_clusterID_original[k];
          std::string proxy_key = proxy_endpoint_key(
              cluster_table_[main_cid].proxy_ip,
              cluster_table_[main_cid].proxy_port);
          int offset = proxy_port_offset[proxy_key]++;
          stripe_data_port[k] = cluster_table_[main_cid].proxy_port + SOCKET_PORT_OFFSET + 1 + offset;
        }
      }
    }
    // parallel_stripes: true-允许条带并行，false-不允许条带并行
    if (!parallel_stripes) {
      // 如果不允许条带间并行，简单地用 for 循环依次调用 run_one_stripe
      for (size_t k = 0; k < ordered_stripe_ids.size(); ++k) {
        run_one_stripe((int)k);
        if (!response.success) {
          finalize_flow_timing_metrics();
          finish_flow_stripe_phase_wall();
          assign_flow_resp(false);
          return;
        }
      }
    } else {
      const size_t nstripes = ordered_stripe_ids.size();
      std::vector<std::thread> all_stripe_workers;
      all_stripe_workers.reserve(nstripes);
      for (size_t k = 0; k < nstripes; ++k) {
        const int fk = static_cast<int>(k);
        all_stripe_workers.emplace_back([&, fk]() {
          try {
            run_one_stripe(fk);
          } catch (...) {
            stripe_ok.store(false);
          }
        });
      }
      for (auto& t : all_stripe_workers) {
        t.join();
      }
      if (!stripe_ok.load()) {
        finalize_flow_timing_metrics();
        finish_flow_stripe_phase_wall();
        assign_flow_resp(false);
        return;
      }
    }
    // 上面各个子线程已经把成百上千个待修复的数据块链路全都登记在 global_link2tasks 里了
    if (enable_global_schedule && !IF_SIMULATION) {
      // 1. 优先下发所有的 MainRepair 任务。
      // 主代理（Main Proxy）的作用是接收数据并解码。只有它们先起来监听网络端口，后续 Help Proxy 发送数据才不会卡死（Deadlock）
      std::vector<std::thread> global_main_threads;
      size_t global_main_job_count = 0;
      for (size_t k = 0; k < global_ctx.size(); ++k) {
        if (global_ctx[k].ready) {
          global_main_job_count += global_ctx[k].main_repairs.size();
        }
      }
      global_main_threads.reserve(global_main_job_count);
      for (size_t k = 0; k < global_ctx.size(); ++k) {
        StripeGlobalContext& ctx = global_ctx[k];
        if (!ctx.ready) {
          continue;
        }
        const int nm_ctx = static_cast<int>(ctx.main_repairs.size());
        for (int i = 0; i < nm_ctx; ++i) {
          const int main_cid = static_cast<int>(ctx.main_repairs[i].cluster_id);
          global_main_threads.emplace_back(
              send_global_main_repair_task, std::ref(ctx), i, main_cid);
        }
      }
      std::cout << "[SCHEDULE] global_main_threads=" << global_main_threads.size()
                << " (started before help; avoid main accept deadlock)"
                << std::endl;

      // 2. 将收集到的全局跨机架边转换为调度器格式
      std::vector<ScheduledLink> links;
      links.reserve(global_link2tasks.size());
      for (const auto& kv : global_link2tasks) {
        links.push_back({kv.first.first, kv.first.second, static_cast<int>(kv.second.size()), false});
      }
      std::cout << "[SCHEDULE] global_compute_schedule stripes=" << ordered_stripe_ids.size()
                << " links=" << links.size()
                << " schedule_join_per_round=" << (schedule_join_per_round ? 1 : 0)
                << std::endl;

      // 3. 核心大招：传入所有的链路，利用二分图着色/匹配算法，算出多轮（Rounds）调度计划。
      RepairScheduler scheduler;
      auto rounds = scheduler.compute_schedule(std::move(links));

      // 4. 根据是否开启 Join 锁决定发送方式
      if (schedule_join_per_round) {
        struct GlobalRoundTask {
          int stripe_idx = -1;
          int plan_i = -1;
          int help_j = -1;
          int src = -1;
          int dst = -1;
          std::string proxy_ip;
          int proxy_port = 0;
        };
        std::vector<std::vector<GlobalRoundTask>> round_task_table;
        round_task_table.reserve(rounds.size());

        int round_id = 0;
        for (const auto& round : rounds) {
          std::vector<GlobalRoundTask> round_tasks;
          std::map<std::pair<int, int>, int> round_edge_dispatch;
          for (const auto& e : round.edges) {
            auto& tasks = global_link2tasks[{e.src, e.dst}];
            const int take = std::min(e.weight, static_cast<int>(tasks.size()));
            for (int t = 0; t < take; ++t) {
              auto one = tasks.back();
              tasks.pop_back();
              StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(one.stripe_idx)];
              Cluster& cluster =
                  cluster_table_[ctx.help_repairs[one.plan_i][one.help_j].cluster_id];
              round_tasks.push_back(
                  {one.stripe_idx, one.plan_i, one.help_j, e.src, e.dst, cluster.proxy_ip,
                   cluster.proxy_port});
            }
            round_edge_dispatch[{e.src, e.dst}] += take;
          }
          round_task_table.push_back(std::move(round_tasks));
          std::cout << "[SCHEDULE] global_round=" << round_id
                    << " edges=" << round.edges.size()
                    << " join_per_round=1 pipeline=1" << std::endl;
          if (IF_DEBUG) {
            std::ostringstream ss;
            ss << "global round=" << round_id << " ";
            for (const auto& kv : round_edge_dispatch) {
              ss << "(" << kv.first.first << "->" << kv.first.second
                 << ":" << kv.second << ") ";
            }
            std::lock_guard<std::mutex> lk(timeline_mutex);
            schedule_notes.push_back(ss.str());
          }
          ++round_id;
        }

        auto launch_prepare_round = [&](int rid) {
          std::vector<std::thread> prep_threads;
          const auto& tasks = round_task_table[static_cast<size_t>(rid)];
          prep_threads.reserve(tasks.size());
          for (const auto& t : tasks) {
            StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(t.stripe_idx)];
            const int seq = ++help_dispatch_seq;
            prep_threads.emplace_back(
                prepare_global_help_repair_task, std::ref(ctx), t.plan_i, t.help_j,
                t.proxy_ip, t.proxy_port, rid, t.src, t.dst, seq);
          }
          for (auto& th : prep_threads) {
            if (th.joinable()) {
              th.join();
            }
          }
        };

        // 预热第一轮：先做机架内读取/编码，再进入 round-0 发送
        if (!round_task_table.empty()) {
          launch_prepare_round(0);
          const double warmup_start = now_sec();
          std::vector<std::thread> warmup_threads;
          const auto& warmup_tasks = round_task_table[0];
          warmup_threads.reserve(warmup_tasks.size());
          for (const auto& t : warmup_tasks) {
            StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(t.stripe_idx)];
            const int seq = ++warmup_dispatch_seq;
            warmup_threads.emplace_back(
                send_global_round_warmup_task, std::ref(ctx), t.plan_i, t.help_j,
                0, t.src, t.dst, seq);
          }
          for (auto& th : warmup_threads) {
            if (th.joinable()) {
              th.join();
            }
          }
          repair_excluded_warmup_sec += std::max(0.0, now_sec() - warmup_start);
        }

        for (int rid = 0; rid < static_cast<int>(round_task_table.size()); ++rid) {
          std::vector<std::thread> send_threads;
          const auto& send_tasks = round_task_table[static_cast<size_t>(rid)];
          send_threads.reserve(send_tasks.size());
          for (const auto& t : send_tasks) {
            StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(t.stripe_idx)];
            const int seq = ++help_dispatch_seq;
            send_threads.emplace_back(
                send_global_prepared_help_repair_task, std::ref(ctx), t.plan_i, t.help_j,
                t.proxy_ip, t.proxy_port, rid, t.src, t.dst, seq);
          }

          std::vector<std::thread> next_prepare_threads;
          if (rid + 1 < static_cast<int>(round_task_table.size())) {
            const auto& prep_tasks = round_task_table[static_cast<size_t>(rid + 1)];
            next_prepare_threads.reserve(prep_tasks.size());
            for (const auto& t : prep_tasks) {
              StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(t.stripe_idx)];
              const int seq = ++help_dispatch_seq;
              next_prepare_threads.emplace_back(
                  prepare_global_help_repair_task, std::ref(ctx), t.plan_i, t.help_j,
                  t.proxy_ip, t.proxy_port, rid + 1, t.src, t.dst, seq);
            }
          }

          for (auto& th : send_threads) {
            if (th.joinable()) {
              th.join();
            }
          }
          for (auto& th : next_prepare_threads) {
            if (th.joinable()) {
              th.join();
            }
          }
          if (!stripe_ok.load()) {
            break;
          }
        }
      } else {
        // ordered 非 join 模式：按轮次顺序创建 help 线程，线程压入全局 vector，
        // 所有轮次派发完毕后再统一 join。使用全局 FIFO 信号量：
        // - 限制并发 help_repair RPC 数量（≤ help_rpc_sem 容量）
        // - FIFO 唤醒保证按轮次顺序派发（先创建的 Round 0 线程优先获取许可）
        int help_rpc_cap;
        if (!schedule_cross_rack_links && !schedule_join_per_round) {
          help_rpc_cap = ec_schema_.flow_repair_parallel_unordered;
        } else if (schedule_cross_rack_links && !schedule_join_per_round) {
          help_rpc_cap = ec_schema_.flow_repair_parallel_ordered;
        } else {
          help_rpc_cap = ec_schema_.flow_repair_parallel_join_ordered;
        }
        if (help_rpc_cap <= 0) {
          help_rpc_cap = static_cast<int>(std::thread::hardware_concurrency());
          if (help_rpc_cap <= 0) help_rpc_cap = 4;
        }
        FifoSemaphore help_rpc_sem(help_rpc_cap);
        std::cout << "[SEM] Global-ordered help_rpc_sem created, capacity=" << help_rpc_cap
                  << std::endl;
        int round_id = 0;
        std::vector<std::thread> global_all_help_threads;
        for (const auto& round : rounds) {
          std::map<std::pair<int, int>, int> round_edge_dispatch;
          for (const auto& e : round.edges) {
            auto& tasks = global_link2tasks[{e.src, e.dst}];
            const int take = std::min(e.weight, static_cast<int>(tasks.size()));
            for (int t = 0; t < take; ++t) {
              auto one = tasks.back();
              tasks.pop_back();
              StripeGlobalContext& ctx = global_ctx[static_cast<size_t>(one.stripe_idx)];
              Cluster& cluster = cluster_table_[ctx.help_repairs[one.plan_i][one.help_j].cluster_id];
              const int seq = ++help_dispatch_seq;
              std::string proxy_ip_val = cluster.proxy_ip;
              int proxy_port_val = cluster.proxy_port;
              global_all_help_threads.emplace_back([&, one, e, round_id, seq,
                                                    proxy_ip_val, proxy_port_val]() {
                help_rpc_sem.acquire();
                StripeGlobalContext& ctx_ref = global_ctx[static_cast<size_t>(one.stripe_idx)];
                send_global_help_repair_task(ctx_ref, one.plan_i, one.help_j,
                    proxy_ip_val, proxy_port_val, round_id, e.src, e.dst, seq);
                help_rpc_sem.release();
              });
            }
            round_edge_dispatch[{e.src, e.dst}] += take;
          }
          std::cout << "[SCHEDULE] global_round=" << round_id
                    << " edges=" << round.edges.size()
                    << " join_per_round=0 join_help_after_round=1(global)"
                    << std::endl;
          if (IF_DEBUG) {
            std::ostringstream ss;
            ss << "global round=" << round_id << " ";
            for (const auto& kv : round_edge_dispatch) {
              ss << "(" << kv.first.first << "->" << kv.first.second
                 << ":" << kv.second << ") ";
            }
            std::lock_guard<std::mutex> lk(timeline_mutex);
            schedule_notes.push_back(ss.str());
          }
          ++round_id;
        }
        for (auto& t : global_all_help_threads) {
          if (t.joinable()) {
            t.join();
          }
        }
        std::cout << "[SCHEDULE] global_all_help_threads_joined stripe_ok="
                  << (stripe_ok.load() ? 1 : 0) << std::endl;
      }

      for (auto& t : global_main_threads) {
        if (t.joinable()) {
          t.join();
        }
      }

      for (size_t k = 0; k < global_ctx.size(); ++k) {
        StripeGlobalContext& ctx = global_ctx[k];
        if (!ctx.ready) {
          continue;
        }
        double stripe_d = 0.0;
        double stripe_c = 0.0;
        double stripe_inner = 0.0;
        double stripe_io = 0.0;
        const int nm = static_cast<int>(ctx.main_repairs.size());
        for (int i = 0; i < nm; ++i) {
          const size_t ui = static_cast<size_t>(i);
          stripe_d = std::max(stripe_d, std::max(ctx.per_plan_main_dec[ui], ctx.per_plan_help_max_dec[ui]));
          stripe_c = std::max(stripe_c, std::max(ctx.per_plan_main_net[ui], ctx.per_plan_help_max_net[ui]));
          stripe_inner = std::max(stripe_inner, std::max(ctx.per_plan_main_inner_net[ui], ctx.per_plan_help_max_inner_net[ui]));
          stripe_io = std::max(stripe_io, std::max(ctx.per_plan_main_io[ui], ctx.per_plan_help_max_io[ui]));
        }
        flow_stripe_decode[k] = stripe_d;
        flow_stripe_cross[k] = stripe_c;
        flow_stripe_inner_net[k] = stripe_inner;
        flow_stripe_io[k] = stripe_io;

        Stripe& stripe = stripe_table_[static_cast<unsigned int>(ctx.stripe_id)];
        for (int i = 0; i < static_cast<int>(ctx.main_repairs.size()); ++i) {
          MainRepairPlan& tmp = ctx.main_repairs[i];
          int j = 0;
          for (auto& idx : tmp.failed_blocks_index) {
            stripe.blocks2nodes[idx] = tmp.new_locations[j++].first;
          }
        }
        ctx.stripe_end_sec = now_sec();
        {
          std::lock_guard<std::mutex> lk(stripe_accum_mutex);
          std::cout << "Repair[ ";
          for (auto& failure : ctx.failed_blocks) {
            std::cout << failure << " ";
          }
          std::cout << "]: Decode/Cross/InnerNet/IO(agg): " << stripe_d
                    << " / " << stripe_c << " / " << stripe_inner << " / "
                    << stripe_io << std::endl;
        }
        {
          std::lock_guard<std::mutex> lk(timeline_mutex);
          int lane = 0;
          auto it_lane = stripe_lane_by_id.find(ctx.stripe_id);
          if (it_lane == stripe_lane_by_id.end()) {
            lane = static_cast<int>(stripe_lane_labels.size());
            stripe_lane_by_id[ctx.stripe_id] = lane;
            stripe_lane_labels.push_back("stripe " + std::to_string(ctx.stripe_id));
          } else {
            lane = it_lane->second;
          }
          stripe_events.push_back(
              {lane, ctx.stripe_begin_sec, ctx.stripe_end_sec, "#4F8EF7",
               "stripe " + std::to_string(ctx.stripe_id), stripe_d, stripe_c,
               stripe_inner, stripe_io, flow_stripe_meta[k]});
        }
      }
    }

    finalize_flow_timing_metrics();   // 综合各个步骤的最高耗时计算总指标
    finish_flow_stripe_phase_wall();  // 停止大秒表，获取实际流逝的墙钟时间
    if (IF_DEBUG && unordered_concurrency_main_repairs) {
      const std::string out_dir = repair_output_dir();
      const std::string mode =
          schedule_cross_rack_links
              ? (schedule_join_per_round ? "join_ordered" : "ordered")
              : "unordered";
      {
        const std::string stripe_svg =
            out_dir + "/repair_timeline_stripe_" + mode + ".svg";
        write_timeline_svg(stripe_svg,
                           "Stripe timeline (" + mode + ")",
                           stripe_lane_labels, stripe_events);
        std::cout << "[DEBUG] Stripe timeline written to: " << stripe_svg << std::endl;
      }
      const std::string block_svg =
          out_dir + "/repair_timeline_block_" + mode + ".svg";
      write_timeline_svg(block_svg,
                         "Block timeline (" + mode + ")",
                         block_lane_labels, block_events, schedule_notes);
      std::cout << "[DEBUG] Block timeline written to: " << block_svg << std::endl;
    }
    gettimeofday(&e2e_end_time, NULL);
    repair_time = (e2e_end_time.tv_sec - e2e_start_time.tv_sec) +
        (e2e_end_time.tv_usec - e2e_start_time.tv_usec) * 1.0 / 1000000;
    assign_flow_resp(true);
  }

  void Coordinator::do_flow_repair(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response)
  {
    do_flow_repair_common(failed_ids, stripe_id, response, false, false, false, false);
  }

  void Coordinator::do_flow_unordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response)
  {
    // 条带内 MainRepairPlan 无序分批并发 + 条带间分批并行；同一 proxy 上 main 与 help
    // 分通道（main_proxies_/help_proxies_ + 各自 mutex）隔离，不同端点仍可并行。
    do_flow_repair_common(failed_ids, stripe_id, response, true, true, false, false);
  }

  void Coordinator::do_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response)
  {
    // 新方案：在无序并发的基础上，对跨机架 help_repair 链路做匹配分轮次调度
    do_flow_repair_common(failed_ids, stripe_id, response, true, true, true, false);
  }

  void Coordinator::do_join_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_ids, int stripe_id,
          RepairResp& response)
  {
    // 新方案（join 版）：按调度轮次派发跨机架 help_repair，且每轮 join 同步。
    // 保持条带间并行，用于“并行 + 调度链路”实验。
    do_flow_repair_common(failed_ids, stripe_id, response, true, true, true, true);
  }

  // 为RS码、LRC码生成具体的修复执行计划
  bool Coordinator::concrete_flow_repair_plans(
          int stripe_id,
          std::vector<RepairPlan>& repair_plans,
          std::vector<MainRepairPlan>& main_repairs,
          std::vector<std::vector<HelpRepairPlan>>& help_repairs,
          unsigned int flow_main_cluster_id,
          int data_port)
  {
    Stripe& stripe = stripe_table_[stripe_id];
    std::vector<unsigned int> t_blocks2nodes(stripe.blocks2nodes.begin(),
        stripe.blocks2nodes.end());
    // for new locations, to optimize
    std::unordered_map<unsigned int, std::vector<unsigned int>> free_nodes_in_clusters;
    for (auto& repair_plan : repair_plans) {
      std::unordered_map<int, unsigned int> map2clusters;
      int cnt = 0;
      // 对修复方案中的可用块，统计每个集群中的可用块数量
      for (auto& help_block : repair_plan.help_blocks) {
        // 【新增检查】：如果 EC 算法返回了空的帮手块分组，直接用 -1 占位并跳过，防止 help_block[0] 越界崩溃！
        if (help_block.empty()) {
            map2clusters[cnt++] = -1; 
            continue;
        }
        unsigned int nid = t_blocks2nodes[help_block[0]];
        unsigned int cid = node_table_[nid].map2cluster;
        map2clusters[cnt++] = cid;
      }
      // repair_plan.failure_idxs：该条带的失效块逻辑ID
      // ----------------------------------------------------------
      // 根据传入的参数选择主修复集群
      unsigned int main_cid = flow_main_cluster_id;
      bool found = false;
      for (int i = 0; i < (int)map2clusters.size(); ++i) {
        if (map2clusters[i] == main_cid) {
          found = true;
          break;
        }
      }
      if (!found) {
        std::cerr << "[WARNING] Main cluster ID " << main_cid
                  << " not found in help block groups, using first group as fallback." << std::endl;
        main_cid = map2clusters[0];  // 回退到第一个分组
      }

      // ============================================================
      // 【修改一】提前完整初始化主集群空闲节点列表
      // 原逻辑在 failed_cluster_sets 循环内懒初始化，导致：
      //   1. 若失效块不在主集群则主集群节点列表为空
      //   2. 活跃块占用的节点未从空闲列表剔除
      // 现改为：在确定 main_cid 后立即初始化，并排除所有活跃块节点
      // ============================================================
      if (free_nodes_in_clusters.find(main_cid) == free_nodes_in_clusters.end()) {
        std::vector<unsigned int> free_nodes;
        Cluster& cluster = cluster_table_[main_cid];
        for (int i = 0; i < num_of_nodes_per_cluster_; i++) {
          free_nodes.push_back(cluster.nodes[i]);
        }
        free_nodes_in_clusters[main_cid] = free_nodes;
      }
      // 【修改一续】遍历所有帮助块，将主集群内活跃块占用的节点从空闲列表中移除
      for (auto& help_block_group : repair_plan.help_blocks) {
        for (auto block_idx : help_block_group) {
          unsigned int nid = t_blocks2nodes[block_idx];
          if (node_table_[nid].map2cluster == main_cid) {
            auto& fn = free_nodes_in_clusters[main_cid];
            auto it = std::find(fn.begin(), fn.end(), nid);
            if (it != fn.end()) fn.erase(it);
          }
        }
      }
      // ============================================================

      std::unordered_set<unsigned int> failed_cluster_sets;
      for (auto it = repair_plan.failure_idxs.begin();
           it != repair_plan.failure_idxs.end(); it++) {
        unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = node_table_[node_id].map2cluster;
        failed_cluster_sets.insert(cluster_id);
        // ============================================================
        // 【修改二】删除此处原有的懒初始化逻辑
        // 原代码在此处初始化 free_nodes_in_clusters[main_cid] 并仅剔除失效块节点，
        // 已由【修改一】提前完整处理，此处不再需要，予以删除。
        // 原代码（已删除）：
        //   if (free_nodes_in_clusters.find(main_cid) == free_nodes_in_clusters.end()) {
        //     std::vector<unsigned int> free_nodes;
        //     Cluster &cluster = cluster_table_[main_cid];
        //     for (int i = 0; i < num_of_nodes_per_cluster_; i++) {
        //       free_nodes.push_back(cluster.nodes[i]);
        //     }
        //     free_nodes_in_clusters[main_cid] = free_nodes;
        //   }
        //   auto iter = std::find(free_nodes_in_clusters[main_cid].begin(),
        //                         free_nodes_in_clusters[main_cid].end(), node_id);
        //   if (iter != free_nodes_in_clusters[main_cid].end()) {
        //     free_nodes_in_clusters[main_cid].erase(iter);
        //   }
        // ============================================================
      }
      MainRepairPlan main_plan;
      int clusters_num = repair_plan.help_blocks.size();
      CodingParameters cp;
      ec_schema_.ec->get_coding_parameters(cp);
      for (int i = 0; i < clusters_num; i++) {
        for (auto block_idx : repair_plan.help_blocks[i]) {
          main_plan.live_blocks_index.push_back(block_idx);
        }
        if (failed_cluster_sets.find(map2clusters[i]) != failed_cluster_sets.end()) {
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            main_plan.inner_cluster_help_blocks_info.push_back(
                std::make_pair(block_idx, std::make_pair(node_ip, node_port)));
            main_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            // for new locations
            unsigned int cluster_id = node_table_[node_id].map2cluster;
            auto iter = std::find(free_nodes_in_clusters[cluster_id].begin(), 
                                  free_nodes_in_clusters[cluster_id].end(), node_id);
            if (iter != free_nodes_in_clusters[cluster_id].end()) {
              free_nodes_in_clusters[cluster_id].erase(iter);
            }
          }
        }
      }
      main_plan.ec_type = ec_schema_.ec_type;
      stripe.ec->get_coding_parameters(main_plan.cp);
      main_plan.cluster_id = main_cid;
      main_plan.cp.x = ec_schema_.x;
      main_plan.cp.seri_num = stripe_id % ec_schema_.x;
      main_plan.cp.local_or_column = repair_plan.local_or_column;
      main_plan.block_size = stripe.block_size;
      main_plan.partial_decoding = ec_schema_.partial_decoding;
      main_plan.partial_scheme = ec_schema_.partial_scheme;
      main_plan.data_port = data_port;
      // 添加故障块信息
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        main_plan.failed_blocks_index.push_back(*it);
        main_plan.failed_block_ids.push_back(stripe.block_ids[*it]);
      }
      for (auto block_idx : repair_plan.parity_idxs) {
        main_plan.parity_blocks_index.push_back(block_idx);
      }
      std::vector<HelpRepairPlan> help_plans;
      // 添加helper块信息
      for (int i = 0; i < clusters_num; i++) {
        // 【新增检查】：遇到空分组直接跳过，防止发送无效 RPC
        if (repair_plan.help_blocks[i].empty()) continue;
        if (map2clusters[i] != main_cid) {
          HelpRepairPlan help_plan;
          help_plan.ec_type = main_plan.ec_type;
          help_plan.cp = main_plan.cp;
          help_plan.cluster_id = map2clusters[i];
          help_plan.block_size = main_plan.block_size;
          help_plan.partial_decoding = main_plan.partial_decoding;
          help_plan.partial_scheme = main_plan.partial_scheme;
          help_plan.isvertical = main_plan.isvertical;
          if (ec_schema_.partial_scheme) {
            for(auto it = repair_plan.failure_idxs.begin();
              it != repair_plan.failure_idxs.end(); it++) {
              help_plan.failed_blocks_index.push_back(*it);
            }
          }
          for(auto it = main_plan.parity_blocks_index.begin(); 
              it != main_plan.parity_blocks_index.end(); it++) {
            help_plan.parity_blocks_index.push_back(*it);
          }
          for(auto it = main_plan.live_blocks_index.begin(); 
              it != main_plan.live_blocks_index.end(); it++) {
            help_plan.live_blocks_index.push_back(*it);
          }
          int num_of_help_blocks = 0;
          for (auto block_idx : repair_plan.help_blocks[i]) {
            unsigned int node_id = t_blocks2nodes[block_idx];
            std::string node_ip = node_table_[node_id].node_ip;
            int node_port = node_table_[node_id].node_port;
            help_plan.inner_cluster_help_blocks_info.push_back(
                std::make_pair(block_idx, std::make_pair(node_ip, node_port)));
            help_plan.inner_cluster_help_block_ids.push_back(
                stripe.block_ids[block_idx]);
            num_of_help_blocks++;
          }
          // 判断是否能够使用局部修复，减少数据传输
          if (ec_schema_.partial_scheme) {
            int failed_num = (int) help_plan.failed_blocks_index.size();
            if (num_of_help_blocks > failed_num) {
              help_plan.partial_less = true;
            }
          } else {
            int num_of_partial_blocks =
                ec_schema_.ec->num_of_partial_blocks_to_transfer(
                    repair_plan.help_blocks[i], help_plan.parity_blocks_index);
            if (num_of_partial_blocks < num_of_help_blocks) {
              help_plan.partial_less = true;
            }
          }
          main_plan.help_clusters_partial_less.push_back(help_plan.partial_less);
          main_plan.help_clusters_blocks_info.push_back(
              help_plan.inner_cluster_help_blocks_info);
          main_plan.help_clusters_block_ids.push_back(
              help_plan.inner_cluster_help_block_ids);
          help_plan.main_proxy_ip = cluster_table_[main_cid].proxy_ip;
          help_plan.main_proxy_port = data_port;
          help_plans.push_back(help_plan);
          std::cout << "[DEBUG concrete_flow] stripe=" << stripe_id
                    << " main_cid=" << main_cid
                    << " cluster_proxy_ip=" << cluster_table_[main_cid].proxy_ip
                    << " cluster_proxy_port=" << cluster_table_[main_cid].proxy_port
                    << " help_plan.main_proxy_port=" << help_plan.main_proxy_port
                    << " helper_cluster=" << map2clusters[i] << std::endl;
        }
      }
      // 选择新存储位置
      // ============================================================
      // 【修改三】new_locations 统一从主集群（main_cid）选取空闲节点
      // 原逻辑中 cluster_id 取自失效块原所在集群，导致修复后新块留在原集群。
      // 现直接使用 main_cid，确保所有修复块都写入主集群（des_rack）。
      // ============================================================
      for(auto it = repair_plan.failure_idxs.begin();
          it != repair_plan.failure_idxs.end(); it++) {
        // 【修改三】cluster_id 固定为 main_cid，不再取失效块原所在集群
        // unsigned int node_id = t_blocks2nodes[*it];
        unsigned int cluster_id = main_cid;
        std::vector<unsigned int> &free_nodes = free_nodes_in_clusters[cluster_id];
        if (free_nodes.empty()) {
          std::cerr << "[ERROR] No free nodes in main cluster " << main_cid << std::endl;
          return false;
        }
        // 从该cluster的可用节点中随机选择一个新节点
        int ran_node_idx = random_index(free_nodes.size());
        unsigned int new_node_id = free_nodes[ran_node_idx];
        auto iter = std::find(free_nodes.begin(), free_nodes.end(), new_node_id);
        if (iter != free_nodes.end()) {
          free_nodes.erase(iter);
        }
        t_blocks2nodes[*it] = new_node_id;
        std::string node_ip = node_table_[new_node_id].node_ip;
        int node_port = node_table_[new_node_id].node_port;
        main_plan.new_locations.push_back(
            std::make_pair(new_node_id, std::make_pair(node_ip, node_port)));
      }
      main_repairs.push_back(main_plan);
      help_repairs.push_back(help_plans);
    }
    return true;
  }
}
