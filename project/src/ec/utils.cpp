#include "utils.h"
#include <cstdlib>
#include <mutex>
#include <unistd.h>

const std::string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

namespace {
  unsigned int ec_random_seed()
  {
    const char *env = std::getenv("EC_RANDOM_SEED");
    if (env != nullptr && env[0] != '\0') {
      try {
        return static_cast<unsigned int>(std::stoul(env));
      } catch (...) {
        std::cerr << "[WARN] Invalid EC_RANDOM_SEED='" << env
                  << "', falling back to random_device." << std::endl;
      }
    }
    return std::random_device{}();
  }

  bool ec_random_seed_is_fixed()
  {
    const char *env = std::getenv("EC_RANDOM_SEED");
    return env != nullptr && env[0] != '\0';
  }

  std::mt19937& ec_rng()
  {
    static std::mt19937 rng(ec_random_seed());
    return rng;
  }

  std::mutex& ec_rng_mutex()
  {
    static std::mutex mtx;
    return mtx;
  }

  struct SeedLegacyRand {
    SeedLegacyRand()
    {
      unsigned int seed = ec_random_seed_is_fixed()
          ? ec_random_seed()
          : static_cast<unsigned int>(std::time(nullptr) ^ getpid());
      std::srand(seed);
    }
  } seed_legacy_rand;

  int random_int(int min, int max)
  {
    std::lock_guard<std::mutex> lock(ec_rng_mutex());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(ec_rng());
  }
}

// generate random index in range [0, len - 1]
int ECProject::random_index(size_t len)
{
  return random_int(0, static_cast<int>(len) - 1);
}

// generate random numbers in range [min, max]
int ECProject::random_range(int min, int max)
{
  return random_int(min, max);
}

// generate n random numbers in range [min, max]
void ECProject::random_n_num(int min, int max, int n, std::vector<int> &random_numbers)
{
  my_assert(n <= max - min + 1);

  int cnt = 0;
  int num = random_int(min, max);
  random_numbers.push_back(num);
  cnt++;
  while (cnt < n) {
    while (std::find(random_numbers.begin(), random_numbers.end(), num) != random_numbers.end()) {
      num = random_int(min, max);
    }
    random_numbers.push_back(num);
    cnt++;
  }
}

void ECProject::random_n_element(int n, std::vector<int> array,
                                 std::vector<int>& selected_num)
{
  my_assert(n <= int(array.size()));
  int cnt = 0;
  while (cnt < n) {
    int ran_idx = random_index(array.size());
    selected_num.push_back(array[ran_idx]);
    array.erase(array.begin() + ran_idx);
    cnt++;
  }
}

// generate random strings
std::string ECProject::generate_random_string(int length)
{
  std::string result;

  for (int i = 0; i < length; ++i) {
    int idx = random_int(0, static_cast<int>(charset.size()) - 1);
    result += charset[idx];
  }

  return result;
}

// generate n key-value pairs with distinct keys
void ECProject::generate_unique_random_strings(
        int key_length, int value_length, int n,
        std::unordered_map<std::string, std::string> &key_value)
{
  for (int i = 0; i < n; i++) {
    std::string key;

    do {
      key.clear();
      for (int i = 0; i < key_length; ++i) {
        int idx = random_int(0, static_cast<int>(charset.size()) - 1);
        key += charset[idx];
      }
    } while (key_value.find(key) != key_value.end());

    std::string value(value_length, key[0]);

    key_value[key] = value;
  }
}

void ECProject::generate_unique_random_keys(int key_length, int n, std::unordered_set<std::string> &keys)
{
  for (int i = 0; i < n; i++) {
    std::string key;
    do {
      key.clear();
      for (int i = 0; i < key_length; ++i) {
        int idx = random_int(0, static_cast<int>(charset.size()) - 1);
        key += charset[idx];
      }
    } while (keys.find(key) != keys.end());

    keys.insert(key);
  }
}

void ECProject::exit_when(bool condition, const std::source_location &location)
{
  if (!condition) {
    std::cerr << "Condition failed at " << location.file_name() << ":" << location.line()
              << " - " << location.function_name() << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

int ECProject::bytes_to_int(std::vector<unsigned char> &bytes)
{
  int integer;
  unsigned char *p = (unsigned char *)(&integer);
  for (int i = 0; i < int(bytes.size()); i++) {
    memcpy(p + i, &bytes[i], 1);
  }
  return integer;
}

std::vector<unsigned char> ECProject::int_to_bytes(int integer)
{
  std::vector<unsigned char> bytes(sizeof(int));
  unsigned char *p = (unsigned char *)(&integer);
  for (int i = 0; i < int(bytes.size()); i++) {
    memcpy(&bytes[i], p + i, 1);
  }
  return bytes;
}

double ECProject::bytes_to_double(std::vector<unsigned char> &bytes)
{
  double doubler;
  memcpy(&doubler, bytes.data(), sizeof(double));
  return doubler;
}

std::vector<unsigned char> ECProject::double_to_bytes(double doubler)
{
  std::vector<unsigned char> bytes(sizeof(double));
  memcpy(bytes.data(), &doubler, sizeof(double));
  return bytes;
}

bool ECProject::cmp_descending(std::pair<int, int> &a,std::pair<int, int> &b)
{
  return a.second > b.second;
}
