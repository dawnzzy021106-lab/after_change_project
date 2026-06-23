#include "lrc.h"

using namespace ECProject;

void test_lrc_decode(LocallyRepairableCode *lrc, char **stripe, int block_size);
void test_lrc_partial_decode(LocallyRepairableCode *lrc, char **stripe,
    int block_size, bool partial_scheme, bool repair_priority, bool repair_method);
void test_lrc_partial_encode(LocallyRepairableCode *lrc,
    char **stripe, int block_size);
void test_lrc_partial_encode_local(LocallyRepairableCode *lrc,
    char **stripe, int block_size);
void test_lrc(LocallyRepairableCode *lrc, bool partial_scheme,
    bool repair_priority, bool repair_method);
void test_lrc_partition(LocallyRepairableCode *lrc);
void test_lrc_repair_plan(LocallyRepairableCode *lrc, PlacementRule rule,
    bool partial_scheme, bool repair_priority, bool repair_method);

int main(int argc, char const *argv[])
{
  bool partial_scheme = false;
  if (argc == 2) {
    partial_scheme = (std::string(argv[1]) == "true");
  }
  bool repair_priority = false;
  if (argc == 3) {
    repair_priority = (std::string(argv[2]) == "true");
  }
  bool repair_method = false;
  if (argc == 4) {
    repair_method = (std::string(argv[3]) == "true");
  }

  LocallyRepairableCode *lrc;

  // test Azure_LRC
  lrc = new Azu_LRC(8, 2, 2);
  test_lrc(lrc, partial_scheme, repair_priority, repair_method);
  delete lrc;

  // test Azure_LRC+1
  lrc = new Azu_LRC_1(8, 3, 2);
  test_lrc(lrc, partial_scheme, repair_priority, repair_method);
  delete lrc;

  // test Optimal_LRC
  lrc = new Opt_LRC(8, 2, 2);
  test_lrc(lrc, partial_scheme, repair_priority, repair_method);
  delete lrc;

  // test Optimal_Cauchy_LRC
  lrc = new Opt_Cau_LRC(8, 2, 2);
  test_lrc(lrc, partial_scheme, repair_priority, repair_method);
  delete lrc;

  // test Uniform_Cauchy_LRC
  lrc = new Uni_Cau_LRC(8, 2, 2);
  test_lrc(lrc, partial_scheme, repair_priority, repair_method);
  delete lrc;

  return 0;
}

void test_lrc(LocallyRepairableCode *lrc, bool partial_scheme,
    bool repair_priority, bool repair_method)
{
  int k = lrc->k;
  int m = lrc->m;
  int block_size = 16;
  int val_len = k * block_size;

  std::vector<char *> v_stripe(k + m);
  char **stripe = (char **)v_stripe.data();
  std::vector<std::vector<char>> stripe_area(k + m, std::vector<char>(block_size));

  // std::unordered_map<std::string, std::string> key_value;
  // generate_unique_random_strings(5, block_size, k, key_value);
  // int idx = 0;
  // for (auto &kv : key_value) {
  //   memcpy(stripe_area[idx].data(), kv.second.c_str(), block_size);
  //   stripe[idx] = stripe_area[idx].data();
  //   idx++;
  // }
  for (int i = 0; i < k; i++) {
    std::string tmp_str = generate_random_string(block_size);
    memcpy(stripe_area[i].data(), tmp_str.c_str(), block_size);
    stripe[i] = stripe_area[i].data();
  }
  for (int i = k; i < k + m; i++) {
    stripe[i] = stripe_area[i].data();
  }

  lrc->encode(stripe, &stripe[k], block_size);

  // test_lrc_decode(lrc, stripe, block_size);

  test_lrc_partial_decode(lrc, stripe, block_size, partial_scheme, repair_priority, repair_method);

  // test_lrc_partial_encode(lrc, stripe, block_size);

  // test_lrc_partial_encode_local(lrc, stripe, block_size);

  // test_lrc_partition(lrc);

  // test_lrc_repair_plan(lrc, FLAT, partial_scheme, repair_priority, repair_method);
  // test_lrc_repair_plan(lrc, RANDOM, partial_scheme, repair_priority, repair_method);
  // test_lrc_repair_plan(lrc, OPTIMAL, partial_scheme, repair_priority, repair_method);
}

void test_lrc_decode(LocallyRepairableCode *lrc, char **stripe, int block_size)
{
  std::cout << "[TEST_LRC_Decoding]"
            << lrc->self_information() << std::endl;
  std::vector<std::string> before_lost;

  int failed_num = random_range(1, lrc->g + 1);
  std::vector<int> failure_idxs;
  random_n_num(0, lrc->k + lrc->m - 1, failed_num, failure_idxs);
  // int failed_num = 3;
  // std::vector<int> failure_idxs = {0, 6, 9};
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

  if (!lrc->check_if_decodable(failure_idxs)) {
    std::cout << "Undecodable!" << std::endl;
    return;
  }

  lrc->decode(stripe, &stripe[lrc->k], block_size, erasures, failed_num);

  delete erasures;

  index = 0;
  for (auto idx : failure_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    if (str != before_lost[index++]) {
      std::cout << "\033[31mFailed! Decode error!\033[0m" << std::endl;
      return;
    }
  }

  std::cout << "\033[32mPassed!\033[0m" << std::endl;
}

void test_lrc_partial_decode(LocallyRepairableCode *lrc, char **stripe,
    int block_size, bool partial_scheme, bool repair_priority, bool repair_method)
{
  std::cout << "[TEST_LRC_Partial_Decoding]"
            << lrc->self_information() << std::endl;
  int failures_num = random_range(2, lrc->g + 1);
  std::vector<int> failures;
  random_n_num(0, lrc->k + lrc->m - 1, failures_num, failures);
  // int failures_num = 3;
  // std::vector<int> failures = {0, 6, 9};

  std::cout << "All failures : ";
  for (auto idx : failures) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  lrc->placement_rule = OPTIMAL;
  lrc->generate_partition();

  if (!lrc->check_if_decodable(failures)) {
    std::cout << "Undecodable!" << std::endl;
    return;
  }

  std::vector<RepairPlan> repair_plans;
  lrc->generate_repair_plan(failures, repair_plans, partial_scheme, repair_priority, repair_method);

  for (auto& rp : repair_plans) {
    lrc->local_or_column = rp.local_or_column;
    std::vector<int> survivor_idxs;
    for (auto& help_block : rp.help_blocks) {
      survivor_idxs.insert(survivor_idxs.end(),
          help_block.begin(), help_block.end());
    }
    std::vector<int> failure_idxs;
    failure_idxs.insert(failure_idxs.end(),
        rp.failure_idxs.begin(), rp.failure_idxs.end());

    auto parity_idxs = std::vector<int>(rp.parity_idxs.begin(),
        rp.parity_idxs.end());

    int failed_num = (int)failure_idxs.size();
    int survivors_num = (int)survivor_idxs.size();
    int l1si_num = random_range(failed_num, survivors_num - failed_num);
    std::vector<int> local1_survivor_idxs;
    random_n_element(l1si_num, survivor_idxs, local1_survivor_idxs);
    std::vector<int> local2_survivor_idxs(survivor_idxs.begin(), survivor_idxs.end());
    for (auto idx : local1_survivor_idxs) {
      local2_survivor_idxs.erase(std::remove(local2_survivor_idxs.begin(),
                                local2_survivor_idxs.end(), idx),
                                local2_survivor_idxs.end());
    }
    int l2si_num = (int)local2_survivor_idxs.size();

    my_assert(l1si_num + l2si_num == survivors_num);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(failure_idxs.begin(), failure_idxs.end(), gen);
    std::shuffle(survivor_idxs.begin(), survivor_idxs.end(), gen);
    std::shuffle(local1_survivor_idxs.begin(), local1_survivor_idxs.end(), gen);
    std::shuffle(local2_survivor_idxs.begin(), local2_survivor_idxs.end(), gen);
    std::shuffle(parity_idxs.begin(), parity_idxs.end(), gen);
    
    std::cout << "Failure indexes : ";
    for (auto idx : failure_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl << "Survivors indexes : ";
    for (auto idx : survivor_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl << "Local[1] Survivors indexes : ";
    for (auto idx : local1_survivor_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl << "Local[2] Survivors indexes : ";
    for (auto idx : local2_survivor_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl << "Parity indexes : ";
    for (auto idx : parity_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl;

    int parity_num = (int)parity_idxs.size();
    int partial_num = parity_num;
    if (partial_scheme) {
      partial_num = failed_num;
    }
    int partial_blocks_num = partial_num * 2;
    std::vector<char *> partial_coding(partial_blocks_num);
    char **partial_blocks = (char **)partial_coding.data();
    std::vector<std::vector<char>> partial_coding_area(partial_blocks_num,
                                                      std::vector<char>(block_size));
    for (int i = 0; i < partial_blocks_num; i++) {
      partial_blocks[i] = partial_coding_area[i].data();
    }

    // partial 1
    std::vector<char *> l1si_data(l1si_num);
    char **l1si = (char **)l1si_data.data();
    int index = 0;
    for (auto idx : local1_survivor_idxs) {
      l1si[index++] = stripe[idx];
    }
    auto parity_flag1 = std::vector<bool>(partial_num, true);
    lrc->encode_partial_blocks(l1si, partial_blocks, block_size,
                               local1_survivor_idxs, parity_idxs,
                               failure_idxs, survivor_idxs,
                               parity_flag1, partial_scheme);

    // partial 2
    std::vector<char *> l2si_data(l2si_num);
    char **l2si = (char **)l2si_data.data();
    index = 0;
    for (auto idx : local2_survivor_idxs) {
      l2si[index++] = stripe[idx];
    }
    auto parity_flag2 = std::vector<bool>(partial_num, true);
    lrc->encode_partial_blocks(l2si, &partial_blocks[failed_num], block_size,
                               local2_survivor_idxs, parity_idxs,
                               failure_idxs, survivor_idxs,
                               parity_flag2, partial_scheme);

    std::vector<int> partial_idxs;
    std::vector<char *> partial_data(partial_blocks_num);
    char **partial = (char **)partial_data.data();
    index = 0;
    for (int i = 0; i < partial_num; i++) {
      if (parity_flag1[i]) {
        partial[index++] = partial_blocks[i];
        if (partial_scheme) {
          partial_idxs.push_back(failure_idxs[i]);
        } else {
          partial_idxs.push_back(parity_idxs[i]);
        }
      }
    }
    for (int i = 0; i < partial_num; i++) {
      if (parity_flag2[i]) {
        partial[index++] = partial_blocks[i + partial_num];
        if (partial_scheme) {
          partial_idxs.push_back(failure_idxs[i]);
        } else {
          partial_idxs.push_back(parity_idxs[i]);
        }
      }
    }

    std::vector<char *> repaired_data(failed_num);
    char **repaired_blocks = (char **)repaired_data.data();
    index = 0;
    std::vector<std::string> before_lost;
    for (auto idx : failure_idxs) {
      std::string str(stripe[idx], stripe[idx] + block_size);
      before_lost.push_back(str);
      memset(stripe[idx], 0, block_size);
      repaired_blocks[index++] = stripe[idx];
    }

    std::vector<char *> v_coding(partial_num);
    char **partial2 = (char **)v_coding.data();
    std::vector<std::vector<char>> v_coding_area(partial_num,
                                                std::vector<char>(block_size));
    for (int i = 0; i < partial_num; i++) {
      partial2[i] = v_coding_area[i].data();
    }

    if (partial_scheme) {
      lrc->perform_addition(partial, repaired_blocks, block_size,
                            partial_idxs, failure_idxs);
    } else {
      lrc->perform_addition(partial, partial2, block_size,
                            partial_idxs, parity_idxs);
      lrc->decode_with_partial_blocks(repaired_blocks, partial2, block_size,
                                      failure_idxs, parity_idxs);
    }

    index = 0;
    for (auto idx : failure_idxs) {
      std::string str(stripe[idx], stripe[idx] + block_size);
      if (str != before_lost[index++]) {
        std::cout << "\033[31mFailed! Partially decode error!\033[0m" << std::endl;
        return;
      }
    }
  }
  
  std::cout << "\033[32mPassed!\033[0m" << std::endl;
}

void test_lrc_partial_encode(LocallyRepairableCode *lrc, char **stripe, int block_size)
{
  std::cout << "[TEST_LRC_Partial_Encoding]"
            << lrc->self_information() << std::endl;
  std::vector<int> data1_idxs;
  std::vector<int> data2_idxs;
  std::vector<int> parity_idxs;
  for (int i = 0; i < lrc->k; i++) {
    data1_idxs.push_back(i);
  }
  for (int i = lrc->k; i < lrc->k + lrc->g; i++) {
    parity_idxs.push_back(i);
  }

  int data2_num = random_range(1, lrc->k - 1);
  random_n_element(data2_num, data1_idxs, data2_idxs);
  for (auto idx : data2_idxs) {
    data1_idxs.erase(std::remove(data1_idxs.begin(), data1_idxs.end(), idx),
                     data1_idxs.end());
  }
  int data1_num = (int)data1_idxs.size();

  my_assert(data1_num + data2_num == lrc->k);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(data1_idxs.begin(), data1_idxs.end(), gen);
  std::shuffle(data2_idxs.begin(), data2_idxs.end(), gen);
  std::shuffle(parity_idxs.begin(), parity_idxs.end(), gen);

  std::cout << "Parity indexes : ";
  for (auto idx : parity_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl << "Data[1] indexes : ";
  for (auto idx : data1_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl << "Data[2] indexes : ";
  for (auto idx : data2_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  int parity_num = int(parity_idxs.size());
  int partial_blocks_num = parity_num * 2;
  std::vector<char *> partial_coding(partial_blocks_num);
  char **partial_blocks = (char **)partial_coding.data();
  std::vector<std::vector<char>> partial_coding_area(partial_blocks_num,
                                                     std::vector<char>(block_size));
  for (int i = 0; i < partial_blocks_num; i++) {
    partial_blocks[i] = partial_coding_area[i].data();
  }

  std::vector<int> nothing;

  // partial 1
  std::vector<char *> data1_data(data1_num);
  char **data1 = (char **)data1_data.data();
  int index = 0;
  for (auto idx : data1_idxs) {
    data1[index++] = stripe[idx];
  }
  auto parity_flag1 = std::vector<bool>(parity_num, true);
  lrc->encode_partial_blocks(data1, partial_blocks, block_size,
                             data1_idxs, parity_idxs, nothing,
                             nothing, parity_flag1, false);

  // partial 2
  std::vector<char *> data2_data(data2_num);
  char **data2 = (char **)data2_data.data();
  index = 0;
  for (auto idx : data2_idxs) {
    data2[index++] = stripe[idx];
  }
  auto parity_flag2 = std::vector<bool>(parity_num, true);
  lrc->encode_partial_blocks(data2, &partial_blocks[parity_num],
                             block_size, data2_idxs, parity_idxs,
                             nothing, nothing, parity_flag2, false);

  std::vector<int> partial_idxs;
  std::vector<char *> partial_data(partial_blocks_num);
  char **partial = (char **)partial_data.data();
  index = 0;
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_flag1[i]);
    partial[index++] = partial_blocks[i];
    partial_idxs.push_back(parity_idxs[i]);
  }
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_flag2[i]);
    partial[index++] = partial_blocks[i + parity_num];
    partial_idxs.push_back(parity_idxs[i]);
  }

  std::vector<char *> parity_data(parity_num);
  char **parity_blocks = (char **)parity_data.data();
  std::vector<std::string> parities;
  index = 0;
  for (auto& idx : parity_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    parities.push_back(str);
    memset(stripe[idx], 0, block_size);
    parity_blocks[index++] = stripe[idx];
  }

  // repair based on partail blocks
  lrc->perform_addition(partial, parity_blocks, block_size,
                        partial_idxs, parity_idxs);

  index = 0;
  for (auto& idx : parity_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    if (str != parities[index++]) {
      std::cout << "\033[31mFailed! Partially encode error!\033[0m" << std::endl;
      return;
    }
  }
  std::cout << "\033[32mPassed!\033[0m" << std::endl;
}

void test_lrc_partial_encode_local(LocallyRepairableCode *lrc, char **stripe,
                                   int block_size)
{
  std::cout << "[TEST_LRC_Partial_Encoding_in_Local_Group]"
            << lrc->self_information() << std::endl;
  std::vector<std::vector<int>> stripe_groups;
  lrc->grouping_information(stripe_groups);
  int group_id = random_index((size_t)lrc->l);
  int group_size = (int)stripe_groups[group_id].size() - 1;
  std::vector<int> data1_idxs;
  std::vector<int> data2_idxs;
  std::vector<int> parity_idxs;
  for (int i = 0; i < group_size; i++) {
    data1_idxs.push_back(stripe_groups[group_id][i]);
  }
  parity_idxs.push_back(stripe_groups[group_id][group_size]);

  if ((lrc->self_information()).substr(0, 14) == "Optimal_Cauchy") {
    for (int i = lrc->k; i < lrc->k + lrc->g; i++) {
      data1_idxs.push_back(i);
    }
    group_size += lrc->g;
  }

  int data2_num = random_range(1, group_size - 1);
  random_n_element(data2_num, data1_idxs, data2_idxs);
  for (auto idx : data2_idxs) {
    data1_idxs.erase(std::remove(data1_idxs.begin(), data1_idxs.end(), idx),
                     data1_idxs.end());
  }
  int data1_num = (int)data1_idxs.size();

  my_assert(data1_num + data2_num == group_size);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(data1_idxs.begin(), data1_idxs.end(), gen);
  std::shuffle(data2_idxs.begin(), data2_idxs.end(), gen);
  std::shuffle(parity_idxs.begin(), parity_idxs.end(), gen);

  std::cout << "Parity indexes : ";
  for (auto idx : parity_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl << "Data[1] indexes : ";
  for (auto idx : data1_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl << "Data[2] indexes : ";
  for (auto idx : data2_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  int parity_num = int(parity_idxs.size());
  int partial_num = 2;
  int partial_blocks_num = parity_num * partial_num;
  std::vector<char *> partial_coding(partial_blocks_num);
  char **partial_blocks = (char **)partial_coding.data();
  std::vector<std::vector<char>> partial_coding_area(partial_blocks_num,
                                                     std::vector<char>(block_size));
  for (int i = 0; i < partial_blocks_num; i++) {
    partial_blocks[i] = partial_coding_area[i].data();
  }

  std::vector<int> nothing;

  // partial 1
  std::vector<char *> data1_data(data1_num);
  char **data1 = (char **)data1_data.data();
  int index = 0;
  for (auto idx : data1_idxs) {
    data1[index++] = stripe[idx];
  }
  auto parity_flag1 = std::vector<bool>(parity_num, true);
  lrc->encode_partial_blocks(data1, partial_blocks, block_size,
                             data1_idxs, parity_idxs, nothing,
                             nothing, parity_flag1, false);

  // partial 2
  std::vector<char *> data2_data(data2_num);
  char **data2 = (char **)data2_data.data();
  index = 0;
  for (auto idx : data2_idxs) {
    data2[index++] = stripe[idx];
  }
  auto parity_flag2 = std::vector<bool>(parity_num, true);
  lrc->encode_partial_blocks(data2, &partial_blocks[parity_num],
                             block_size, data2_idxs, parity_idxs,
                             nothing, nothing, parity_flag2, false);

  std::vector<int> partial_idxs;
  std::vector<char *> partial_data(partial_blocks_num);
  char **partial = (char **)partial_data.data();
  index = 0;
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_flag1[i]);
    partial[index++] = partial_blocks[i];
    partial_idxs.push_back(parity_idxs[i]);
  }
  for (int i = 0; i < parity_num; i++) {
    my_assert(parity_flag2[i]);
    partial[index++] = partial_blocks[i + parity_num];
    partial_idxs.push_back(parity_idxs[i]);
  }

  std::vector<char *> parity_data(parity_num);
  char **parity_blocks = (char **)parity_data.data();
  std::vector<std::string> parities;
  index = 0;
  for (auto idx : parity_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    parities.push_back(str);
    memset(stripe[idx], 0, block_size);
    parity_blocks[index++] = stripe[idx];
  }

  // repair based on partail blocks
  lrc->perform_addition(partial, parity_blocks, block_size,
                        partial_idxs, parity_idxs);

  index = 0;
  for (auto idx : parity_idxs) {
    std::string str(stripe[idx], stripe[idx] + block_size);
    if (str != parities[index++]) {
      std::cout << "\033[31mFailed! Partially encode error!\033[0m" << std::endl;
      return;
    }
  }
  std::cout << "\033[32mPassed!\033[0m" << std::endl;
}

void test_lrc_partition(LocallyRepairableCode *lrc)
{
  // lrc->placement_rule = FLAT;
  // lrc->generate_partition();
  // lrc->print_info(lrc->partition_plan, "partition");

  // lrc->placement_rule = RANDOM;
  // lrc->generate_partition();
  // lrc->print_info(lrc->partition_plan, "partition");

  lrc->placement_rule = OPTIMAL;
  lrc->generate_partition();
  lrc->print_info(lrc->partition_plan, "partition");
}

void test_lrc_repair_plan(LocallyRepairableCode *lrc, PlacementRule rule,
    bool partial_scheme, bool repair_priority, bool repair_method)
{
  std::cout << "[TEST_LRC_Repair_Plan]"
            << lrc->self_information() << std::endl;

  
  std::vector<std::vector<int>> groups;
  lrc->grouping_information(groups);
  lrc->print_info(groups, "groups_information");
  
  lrc->placement_rule = rule;
  lrc->generate_partition();
  lrc->print_info(lrc->partition_plan, "partition");

  int failed_num = random_range(1, lrc->g + 1);
  std::vector<int> failure_idxs;
  random_n_num(0, lrc->k + lrc->m - 1, failed_num, failure_idxs);

  std::cout << "Failure indexes : ";
  for (auto idx : failure_idxs) {
    std::cout << idx << " ";
  }
  std::cout << std::endl;

  if (!lrc->check_if_decodable(failure_idxs)) {
    std::cout << "Undecodable!" << std::endl;
    return;
  }

  std::vector<RepairPlan> repair_plans;
  lrc->generate_repair_plan(failure_idxs, repair_plans, partial_scheme, repair_priority, repair_method);

  int cnt = 0;
  for (auto& plan : repair_plans) {
    std::cout << "[Plan " << cnt++ << "]\n";
    std::cout << "Failures index: ";
    for (auto idx : plan.failure_idxs) {
      std::cout << idx << " ";
    }
    std::cout << std::endl;
    lrc->print_info(plan.help_blocks, "Help blocks");
  }
}