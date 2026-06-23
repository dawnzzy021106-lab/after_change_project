#include "coordinator.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ECProject
{
  namespace
  {
    class TemporaryRandomRepairSolver
    {
    public:
      explicit TemporaryRandomRepairSolver(int ec_k)
        : solver_path_("/home/hadoop/zzy/ec_prototype-master/flow_repair/complete_min_cost_max_flow"),
          backup_path_(solver_path_ + ".mcmf_backup"),
          rr_path_("/home/hadoop/zzy/ec_prototype-master/flow_repair/rr.py"),
          restored_(false)
      {
        if (::access(rr_path_.c_str(), R_OK) != 0) {
          throw std::runtime_error("rr.py is not readable: " + rr_path_);
        }

        const bool solver_exists = (::access(solver_path_.c_str(), F_OK) == 0);
        if (solver_exists) {
          if (::access(backup_path_.c_str(), F_OK) == 0) {
            // 上一次异常退出时可能留下了备份，先恢复为真实 MCMF solver。
            ::remove(solver_path_.c_str());
            if (::rename(backup_path_.c_str(), solver_path_.c_str()) != 0) {
              perror("restore stale min_cost_max_flow backup failed");
              throw std::runtime_error("cannot restore stale min_cost_max_flow backup");
            }
          }
          if (::rename(solver_path_.c_str(), backup_path_.c_str()) != 0) {
            perror("backup min_cost_max_flow failed");
            throw std::runtime_error("cannot backup min_cost_max_flow solver");
          }
          backed_up_ = true;
        }

        std::ofstream wrapper(solver_path_);
        if (!wrapper.is_open()) {
          restore();
          throw std::runtime_error("cannot create random repair solver wrapper: " + solver_path_);
        }
        wrapper << "#!/bin/sh\n";
        wrapper << "exec python3 " << rr_path_ << " --ec-k " << ec_k << " \"$@\"\n";
        wrapper.close();
        if (::chmod(solver_path_.c_str(), 0755) != 0) {
          perror("chmod random repair solver wrapper failed");
          restore();
          throw std::runtime_error("cannot chmod random repair solver wrapper");
        }
      }

      ~TemporaryRandomRepairSolver()
      {
        restore();
      }

      TemporaryRandomRepairSolver(const TemporaryRandomRepairSolver&) = delete;
      TemporaryRandomRepairSolver& operator=(const TemporaryRandomRepairSolver&) = delete;

    private:
      void restore()
      {
        if (restored_) return;
        ::remove(solver_path_.c_str());
        if (backed_up_) {
          if (::rename(backup_path_.c_str(), solver_path_.c_str()) != 0) {
            perror("restore min_cost_max_flow failed");
          }
        }
        restored_ = true;
      }

      std::string solver_path_;
      std::string backup_path_;
      std::string rr_path_;
      bool backed_up_ = false;
      bool restored_ = false;
    };
  }

  void Coordinator::do_random_repair(std::vector<unsigned int> failed_ids, int stripe_id,
                                     RepairResp& response)
  {
    try {
      const int ec_k = ec_schema_.ec ? ec_schema_.ec->k : 0;
      if (ec_k <= 0) {
        std::cerr << "[ERROR] random repair cannot get a valid ec k" << std::endl;
        response.success = false;
        return;
      }

      // 复用 do_flow_repair_common 中的完整链路：
      // failures_map -> Available_matrix -> solver -> cluster_data.bin -> concrete_flow_repair_plans -> RPC修复。
      // 这里仅临时将 solver 入口替换为 rr.py，因此 1-4 的 MCMF 测试不受影响。
      TemporaryRandomRepairSolver random_solver(ec_k);
      do_flow_repair_common(failed_ids, stripe_id, response,
                            false,  // unordered_concurrency_main_repairs
                            false,  // parallel_stripes
                            false,  // schedule_cross_rack_links
                            false); // schedule_join_per_round
    } catch (const std::exception& e) {
      std::cerr << "[ERROR] random repair failed before executing repair flow: "
                << e.what() << std::endl;
      response.success = false;
    }
  }
}
