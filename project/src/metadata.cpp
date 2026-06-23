#include "metadata.h"
#include <fstream>
#include <sstream>

namespace ECProject
{
  static const std::unordered_map<std::string, MultiStripePR> mspr_map = {
    {"RAND", RAND},
    {"DISPERSED", DISPERSED},
    {"AGGREGATED", AGGREGATED},
    {"HORIZONTAL", HORIZONTAL},
    {"VERTICAL", VERTICAL}
  };

  static const std::unordered_map<std::string, PlacementRule> pr_map = {
    {"FLAT", FLAT},
    {"RANDOM", RANDOM},
    {"OPTIMAL", OPTIMAL}
  };

  static const std::unordered_map<std::string, ECTYPE> ec_map = {
    {"RS", RS},
    {"AZURE_LRC", AZURE_LRC},
    {"AZURE_LRC_1", AZURE_LRC_1},
    {"OPTIMAL_LRC", OPTIMAL_LRC},
    {"OPTIMAL_CAUCHY_LRC", OPTIMAL_CAUCHY_LRC},
    {"UNIFORM_CAUCHY_LRC", UNIFORM_CAUCHY_LRC},
    {"PC", PC},
    {"HV_PC", HV_PC}
  };

  ECFAMILY check_ec_family(ECTYPE ec_type)
  {
    if (ec_type == AZURE_LRC || ec_type == AZURE_LRC_1 ||
        ec_type == OPTIMAL_LRC || ec_type == OPTIMAL_CAUCHY_LRC ||
        ec_type == UNIFORM_CAUCHY_LRC) {
      return LRCs;
    } else if (ec_type == PC || ec_type == HV_PC) {
      return PCs;
    } else {
      return RSCodes;
    }
  }

  ErasureCode* ec_factory(ECTYPE ec_type, CodingParameters cp)
  {
    ErasureCode *ec;
    if (ec_type == RS) {
      ec = new RSCode(cp.k, cp.m);
    } else if (ec_type == AZURE_LRC) {
      ec = new Azu_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == AZURE_LRC_1) {
      ec = new Azu_LRC_1(cp.k, cp.l, cp.g);
    } else if (ec_type == OPTIMAL_LRC) {
      ec = new Opt_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == OPTIMAL_CAUCHY_LRC) {
      ec = new Opt_Cau_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == UNIFORM_CAUCHY_LRC) {
      ec = new Uni_Cau_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == PC) {
      ec = new ProductCode(cp.k1, cp.m1, cp.k2, cp.m2);
    } else if (ec_type == HV_PC) {
      ec = new HVPC(cp.k1, cp.m1, cp.k2, cp.m2);
    } else {
      ec = nullptr;
    }
    return ec;
  }

  RSCode* rs_factory(ECTYPE ec_type, CodingParameters cp)
  {
    RSCode *rs;
    if (ec_type == RS) {
      rs = new RSCode(cp.k, cp.m);
    } else {
      rs = nullptr;
    }
    return rs;
  }

  LocallyRepairableCode* lrc_factory(ECTYPE ec_type, CodingParameters cp)
  {
    LocallyRepairableCode *lrc;
    if (ec_type == AZURE_LRC) {
      lrc = new Azu_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == AZURE_LRC_1) {
      lrc = new Azu_LRC_1(cp.k, cp.l, cp.g);
    } else if (ec_type == OPTIMAL_LRC) {
      lrc = new Opt_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == OPTIMAL_CAUCHY_LRC) {
      lrc = new Opt_Cau_LRC(cp.k, cp.l, cp.g);
    } else if (ec_type == UNIFORM_CAUCHY_LRC) {
      lrc = new Uni_Cau_LRC(cp.k, cp.l, cp.g);
    } else {
      lrc = nullptr;
    }
    return lrc;
  }

  ProductCode* pc_factory(ECTYPE ec_type, CodingParameters cp)
  {
    ProductCode *pc;
    if (ec_type == PC) {
      pc = new ProductCode(cp.k1, cp.m1, cp.k2, cp.m2);
    } else if (ec_type == HV_PC) {
      pc = new HVPC(cp.k1, cp.m1, cp.k2, cp.m2);
    } else {
      pc = nullptr;
    }
    return pc;
  }

  ErasureCode* clone_ec(ECTYPE ec_type, ErasureCode* ec)
  {
    CodingParameters cp;
    ec->get_coding_parameters(cp);
    return ec_factory(ec_type, cp);
  }

  void parse_args(ParametersInfo& paras, std::string config_file)
  {
    std::unordered_map<std::string, std::string> config;
    std::ifstream file(config_file);
    if (file.is_open()) {
      std::string line;
      while (std::getline(file, line)) {
        if (line[0] == '[' || line[0] == '#' || line.empty())
          continue;

        std::stringstream ss(line);
        std::string key, value;
        if (std::getline(ss, key, '=') && std::getline(ss, value)) {
          // remove space around '='
          auto new_key_end = std::remove_if(key.begin(), key.end(),
                  [](unsigned char ch) { return std::isspace(ch); });
          key.erase(new_key_end, key.end());
          auto new_val_end = std::remove_if(value.begin(), value.end(),
              [](unsigned char ch) { return std::isspace(ch); });
          value.erase(new_val_end, value.end());
          config[key] = value;
        }
      }
      file.close();
    } else {
      std::cerr << "Unable to open " << config_file << std::endl;
      return;
    }

    paras.partial_decoding = (config["partial_decoding"] == "true");
    paras.partial_scheme = (config["partial_scheme"] == "true");
    paras.repair_priority = (config["repair_priority"] == "true");
    paras.repair_method = (config["repair_method"] == "true");
    std::string temp = config["ec_type"];
    if (ec_map.find(temp) == ec_map.end()) {
      std::cerr << "Unknown ec_type: " << temp << std::endl;
      exit(0);
    } else {
      paras.ec_type = ec_map.at(temp);
    }
    temp = config["placement_rule"];
    if (pr_map.find(temp) == pr_map.end()) {
      std::cerr << "Unknown placement_rule: " << temp << std::endl;
      exit(0);
    } else {
      paras.placement_rule = pr_map.at(temp);
    }
    temp = config["multistripe_placement_rule"];
    if (mspr_map.find(temp) == mspr_map.end()) {
      std::cerr << "Unknown multistripe_placement_rule: " << temp << std::endl;
      exit(0);
    } else {
      paras.multistripe_placement_rule = mspr_map.at(temp);
    }
    paras.block_size = std::stoul(config["block_size"]) * 1024;
    paras.repair_stripe_num = std::stoi(config["repair_stripe_num"]);
    if (config.find("flow_repair_max_parallel") != config.end()) {
      paras.flow_repair_max_parallel = std::stoi(config["flow_repair_max_parallel"]);
    } else {
      paras.flow_repair_max_parallel = 0;
    }
    // 三种 flow repair 模式并行度：若未单独配置，则回退到 flow_repair_max_parallel（再回退默认）
    auto get_int_or = [&](const std::string& key, int fallback) -> int {
      auto it = config.find(key);
      if (it != config.end()) return std::stoi(it->second);
      return fallback;
    };
    paras.flow_repair_parallel_unordered =
        get_int_or("flow_repair_parallel_unordered",
                   paras.flow_repair_max_parallel > 0 ? paras.flow_repair_max_parallel : 25);
    paras.flow_repair_parallel_ordered =
        get_int_or("flow_repair_parallel_ordered",
                   paras.flow_repair_max_parallel > 0 ? paras.flow_repair_max_parallel : 8);
    paras.flow_repair_parallel_join_ordered =
        get_int_or("flow_repair_parallel_join_ordered",
                   paras.flow_repair_max_parallel > 0 ? paras.flow_repair_max_parallel : 4);
    paras.cp.x = std::stoi(config["x"]);
    paras.cp.k = std::stoi(config["k"]);
    paras.cp.m = std::stoi(config["m"]);
    paras.cp.l = std::stoi(config["l"]);
    paras.cp.g = std::stoi(config["g"]);
    paras.cp.k1 = std::stoi(config["k1"]);
    paras.cp.m1 = std::stoi(config["m1"]);
    paras.cp.k2 = std::stoi(config["k2"]);
    paras.cp.m2 = std::stoi(config["m2"]);

    if (check_ec_family(paras.ec_type) == LRCs) {
      paras.cp.m = paras.cp.l + paras.cp.g;
    } else if (check_ec_family(paras.ec_type) == PCs) {
      if (paras.ec_type == HV_PC) {
        paras.cp.m = paras.cp.k1 * paras.cp.m2 + paras.cp.k2 * paras.cp.m1;
      } else {
        paras.cp.m = paras.cp.k1 * paras.cp.m2 + paras.cp.k2 * paras.cp.m1
            + paras.cp.m1 * paras.cp.m2;
      }
    }

    std::cout << "*-*-*-*-*-*-*-EC-Info-*-*-*-*-*-*-*" << std::endl;
    std::cout << "Partial?:" << paras.partial_decoding
              << " P-S:" << paras.partial_scheme
              << " R-P:" << paras.repair_priority
              << " R-M:" << paras.repair_method << std::endl;
    std::cout << config["ec_type"] << " " << config["placement_rule"] << "(s) "
              << config["multistripe_placement_rule"] << "(m)" << std::endl;
    if (check_ec_family(paras.ec_type) == LRCs) {
      std::cout << "(" << paras.cp.k << "," << paras.cp.l
                << "," << paras.cp.g << ") ";
    } else if (check_ec_family(paras.ec_type) == PCs) {
      std::cout << "(" << paras.cp.k1 << "," << paras.cp.m1
                << "," << paras.cp.k2 << "," << paras.cp.m2 << ") ";
    } else {
      std::cout << "(" << paras.cp.k << "," << paras.cp.m << ") ";
    }
    std::cout << paras.cp.x << "(x) "
              << paras.block_size << "(bytes)" << paras.repair_stripe_num << "(stripes)"
              << " flow_repair_max_parallel=" << paras.flow_repair_max_parallel
              << " (<=0:auto)"
              << " flow_parallel(unordered/ordered/join)="
              << paras.flow_repair_parallel_unordered << "/"
              << paras.flow_repair_parallel_ordered << "/"
              << paras.flow_repair_parallel_join_ordered << "\n";
    std::cout << "*-*-*-*-*-*-*-EC-Info-*-*-*-*-*-*-*" << std::endl;
  }

  int stripe_wide_after_merge(ParametersInfo paras, int step_size)
  {
     ECFAMILY ec_family = check_ec_family(paras.ec_type);
    if (ec_family == RSCodes) {
      paras.cp.k *= step_size;
      return paras.cp.k + paras.cp.m;
    } else if (ec_family == LRCs) {
      paras.cp.k *= step_size;
      paras.cp.l *= step_size;
      return paras.cp.k + paras.cp.l + paras.cp.g;
    } else {
      if (paras.multistripe_placement_rule == VERTICAL) {
        paras.cp.k2 *= step_size;
      } else {
        paras.cp.k1 *= step_size;
      }
      if (paras.ec_type == HV_PC) {
        return (paras.cp.k1 + paras.cp.m1) * (paras.cp.k2 + paras.cp.m2)
            - paras.cp.m1 * paras.cp.m2;
      }
      return (paras.cp.k1 + paras.cp.m1) * (paras.cp.k2 * paras.cp.m2);
    }
  }
}
