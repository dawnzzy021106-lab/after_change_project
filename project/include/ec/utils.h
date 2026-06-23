#pragma once

#include <stdio.h>
#include <iostream>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sys/time.h>
#include <source_location>

#define my_assert(condition) exit_when((condition), std::source_location::current())

namespace ECProject {
  // generate a random index in [0,len)
  int random_index(size_t len);
  // generate a random integer in [min,max]
  int random_range(int min, int max);
  // generate n random number in [min,max]
  void random_n_num(int min, int max, int n, std::vector<int>& random_numbers);
  // randomly select n element in the array
  void random_n_element(int n, std::vector<int> array,
                        std::vector<int>& selected_num);

  std::string generate_random_string(int length);

  void generate_unique_random_strings(int key_length, int value_length, int n,
          std::unordered_map<std::string, std::string> &key_value);

  void generate_unique_random_keys(int key_length, int n,
          std::unordered_set<std::string> &keys);

  void exit_when(bool condition, const std::source_location &location);

  int bytes_to_int(std::vector<unsigned char> &bytes);

  std::vector<unsigned char> int_to_bytes(int integer);

  double bytes_to_double(std::vector<unsigned char> &bytes);

  std::vector<unsigned char> double_to_bytes(double doubler);

  bool cmp_descending(std::pair<int, int> &a,std::pair<int, int> &b);
}
