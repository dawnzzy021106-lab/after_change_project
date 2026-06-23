#include "pc.h"

using namespace ECProject;

void test_pc_decode(ProductCode *pc, char **stripe, int block_size);
void test_pc(ProductCode *pc, bool partial_scheme);
void test_pc_partition(ProductCode *pc);
void test_pc_repair_plan(ProductCode *pc, PlacementRule rule, bool partial_scheme);

int main(int argc, char const *argv[])
{
  bool partial_scheme = false;
  if (argc == 2) {
    partial_scheme = (std::string(argv[1]) == "true");
  }

  ProductCode *pc;

  // test ProductCode
  pc = new ProductCode(4, 2, 2, 1);
  test_pc(pc, partial_scheme);
  delete pc;

  // test HVPC
  pc = new HVPC(4, 2, 2, 1);
  test_pc(pc, partial_scheme);
  delete pc;

  return 0;
}

void test_pc(ProductCode *pc, bool partial_scheme)
{
  int k = pc->k;
  int m = pc->m;
  int block_size = 16;
  int val_len = k * block_size;

  std::vector<char *> v_stripe(k + m);
  char **stripe = (char **)v_stripe.data();
  std::vector<std::vector<char>> stripe_area(k + m, std::vector<char>(block_size));

  for (int i = 0; i < k; i++) {
    std::string tmp_str = generate_random_string(block_size);
    memcpy(stripe_area[i].data(), tmp_str.c_str(), block_size);
    stripe[i] = stripe_area[i].data();
  }
  for (int i = k; i < k + m; i++) {
    stripe[i] = stripe_area[i].data();
  }

  pc->encode(stripe, &stripe[k], block_size);

  test_pc_decode(pc, stripe, block_size);

  test_pc_partition(pc);

  test_pc_repair_plan(pc, FLAT, partial_scheme);
  test_pc_repair_plan(pc, RANDOM, partial_scheme);
  test_pc_repair_plan(pc, OPTIMAL, partial_scheme);
}

void test_pc_decode(ProductCode *pc, char **stripe, int block_size)
{
  std::cout << "[TEST_ProductCode_Decoding]"
            << pc->self_information() << std::endl;
  std::vector<std::string> before_lost;

  int failed_num = random_range(1, (pc->m1 + 1) * (pc->m2 + 1) - 1);
  std::vector<int> failure_idxs;
  random_n_num(0, pc->k + pc->m - 1, failed_num, failure_idxs);
  int *erasures = new int[failed_num + 1];
  erasures[failed_num] = -1;
  int index = 0;
  for (auto idx : failure_idxs) {
    erasures[index++] = idx;
    std::string str(stripe[idx], stripe[idx] + block_size);
    before_lost.push_back(str);
    memset(stripe[idx], 0, block_size);
  }

  std::cout << "Failure indexes : ";
  for (auto idx : failure_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  if (!pc->check_if_decodable(failure_idxs)) {
    std::cout << "Undecodable!" << std::endl;
    return;
  }

  pc->decode(stripe, &stripe[pc->k], block_size, erasures, failed_num);

  delete erasures;

  index = 0;
  for (auto idx : failure_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    if (str != before_lost[index++]) {
      std::cout << "Failed! Decode error!" << std::endl;
      return;
    }
  }
  std::cout << "\033[32mPassed!\033[0m" << std::endl;
}

void test_pc_partition(ProductCode *pc)
{
  pc->placement_rule = FLAT;
  pc->generate_partition();
  pc->print_info(pc->partition_plan, "partition");

  pc->placement_rule = RANDOM;
  pc->generate_partition();
  pc->print_info(pc->partition_plan, "partition");

  pc->placement_rule = OPTIMAL;
  pc->generate_partition();
  pc->print_info(pc->partition_plan, "partition");
}

void test_pc_repair_plan(ProductCode *pc, PlacementRule rule, bool partial_scheme)
{
  std::cout << "[TEST_ProductCode_Repair_Plan]"
            << pc->self_information() << std::endl;
  
  pc->placement_rule = rule;
  pc->generate_partition();
  pc->print_info(pc->partition_plan, "partition");
  pc->row_code.print_info(pc->row_code.partition_plan, "partition");

  int failed_num = random_range(1, (pc->m1 + 1) * (pc->m2 + 1) - 1);
  std::vector<int> failure_idxs;
  random_n_num(0, pc->k + pc->m - 1, failed_num, failure_idxs);

  std::cout << "Failure indexes : ";
  for (auto idx : failure_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  if (!pc->check_if_decodable(failure_idxs)) {
    std::cout << "Undecodable!" << std::endl;
    return;
  }

  std::vector<RepairPlan> repair_plans;
  pc->generate_repair_plan(failure_idxs, repair_plans, partial_scheme, false, false);

  int cnt = 0;
  for (auto& plan : repair_plans) {
    std::cout << "[Plan " << cnt++ << "]\n";
    std::cout << "Failures index: ";
    for (auto idx : plan.failure_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl;
    pc->print_info(plan.help_blocks, "Help blocks");
  }
}