#include "client.h"
#include <unistd.h>

namespace ECProject
{
  Client::Client(std::string ip, int port, std::string coordinator_ip,
                 int coordinator_port)
    : ip_(ip), port_(port), coordinator_ip_(coordinator_ip),
      coordinator_port_(coordinator_port),
      acceptor_(io_context_,
        asio::ip::tcp::endpoint(asio::ip::address::from_string(ip.c_str()), port_)) 
  {
    easylog::set_min_severity(easylog::Severity::ERROR);
    rpc_coordinator_ = std::make_unique<coro_rpc::coro_rpc_client>();
    async_simple::coro::syncAwait(
        rpc_coordinator_->connect(coordinator_ip_, std::to_string(coordinator_port_)));
  }

  Client::~Client() { acceptor_.close(); }

  void Client::set_ec_parameters(ParametersInfo parameters)
  {
    async_simple::coro::syncAwait(
        rpc_coordinator_->call<&Coordinator::set_erasure_coding_parameters>(parameters));
  }

  double Client::set(std::string key, std::string value)
  {
    std::vector<std::pair<std::string, size_t>> objects;
    objects.push_back({key, value.size()});

    // *******************************
    auto res = async_simple::coro::syncAwait(rpc_coordinator_->call_for<&Coordinator::request_set>(std::chrono::seconds{500}, objects));
    if (!res) {
      std::cout << "[RPC ERROR] RPC to Coordinator failed: " << res.error().msg << std::endl;
      return 0.0;
    }
    auto response = res.value();


    // auto response =
    //     async_simple::coro::syncAwait(
    //         rpc_coordinator_->call_for<&Coordinator::request_set>(
    //             std::chrono::seconds{0}, objects)).value();
        
    std::cout << "[SET] Send " << key << " to proxy_address:" 
              << response.proxy_ip << ":" << response.proxy_port 
              << std::endl;

    double encoding_time = 0;
    if (!IF_SIMULATION) {
      asio::ip::tcp::socket socket_(io_context_);
      asio::ip::tcp::endpoint endpoint(asio::ip::make_address(response.proxy_ip),
                                       response.proxy_port);
      socket_.connect(endpoint);

      std::vector<unsigned char> key_size_buf = int_to_bytes((int)key.size());
      asio::write(socket_, asio::buffer(key_size_buf, key_size_buf.size()));
      std::vector<unsigned char> value_size_buf = int_to_bytes((int)value.size());
      asio::write(socket_, asio::buffer(value_size_buf, value_size_buf.size()));

      asio::write(socket_, asio::buffer(key, key.size()));
      asio::write(socket_, asio::buffer(value, value.size()));

      std::vector<unsigned char> finish_buf(sizeof(int));
      asio::read(socket_, asio::buffer(finish_buf,
                                       finish_buf.size()));
      int finish = bytes_to_int(finish_buf);

      std::vector<unsigned char> encoding_time_buf(sizeof(double));
      asio::read(socket_, asio::buffer(encoding_time_buf,
                                       encoding_time_buf.size()));
      encoding_time = bytes_to_double(encoding_time_buf);

      asio::error_code ignore_ec;
      socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
      socket_.close(ignore_ec);

      std::vector<std::string> keys;
      for (auto& pair : objects) {
        keys.push_back(pair.first);
      }
      async_simple::coro::syncAwait(
          rpc_coordinator_->call<&Coordinator::commit_object>(keys, (bool)finish));
    }

    return encoding_time;
  }

  std::string Client::get(std::string key)
  {
    size_t value_len = 
        async_simple::coro::syncAwait(
            rpc_coordinator_->call<&Coordinator::request_get>(key, ip_, port_)).value();

    std::string key_buf(key.size(), 0);
    std::string value_buf(value_len, 0);

    if (!IF_SIMULATION) {
      asio::ip::tcp::socket socket_(io_context_);
      acceptor_.accept(socket_);

      std::vector<unsigned char> size_buf(sizeof(int));
      asio::read(socket_, asio::buffer(size_buf, size_buf.size()));
      int key_size = bytes_to_int(size_buf);
      asio::read(socket_, asio::buffer(size_buf, size_buf.size()));
      int value_size = bytes_to_int(size_buf);
      if (value_size > 0) {
        size_t read_len_of_key = 
            asio::read(socket_, asio::buffer(key_buf.data(), key_buf.size()));
        my_assert(read_len_of_key == key.size() && key_buf == key);

        size_t read_len_of_value =
            asio::read(socket_, asio::buffer(value_buf.data(), value_buf.size()));
        my_assert(read_len_of_value == value_len);

        asio::error_code ignore_ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        socket_.close(ignore_ec);

        std::cout << "[GET] get key: " << key_buf.data()
                  << ", valuesize: " << value_len << std::endl;
      } else {
        std::cout << "[GET] can not get value of " << key_buf.data() << std::endl;
      }
    }

    return value_buf;
  }

  void Client::delete_stripe(unsigned int stripe_id)
  {
    std::vector<unsigned int> stripe_ids;
    stripe_ids.push_back(stripe_id);
    async_simple::coro::syncAwait(
        rpc_coordinator_->call<&Coordinator::request_delete_by_stripe>(stripe_ids));
    std::cout << "[DEL] deleting Stripe " << stripe_id << std::endl;
  }

  void Client::delete_all_stripes()
  {
    auto stripe_ids =
        async_simple::coro::syncAwait(
            rpc_coordinator_->call<&Coordinator::list_stripes>()).value();
    async_simple::coro::syncAwait(
        rpc_coordinator_->call<&Coordinator::request_delete_by_stripe>(stripe_ids));
    for (auto it = stripe_ids.begin(); it != stripe_ids.end(); it++) {
      std::cout << "[DEL] deleting Stripe " << *it << std::endl;
    }
  }

  RepairResp Client::nodes_repair(std::vector<unsigned int> failed_node_ids)
  {
    return nodes_random_repair(failed_node_ids);
  }

  RepairResp Client::nodes_random_repair(std::vector<unsigned int> failed_node_ids)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<&Coordinator::request_random_repair>(
            std::chrono::seconds{500}, failed_node_ids, -1));
    if (!r) {
      std::cerr << "[RPC ERROR] request_random_repair(nodes) failed: "
                << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  void Client::set_flow_repair_max_parallel(int n)
  {
    async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<&Coordinator::set_flow_repair_max_parallel>(
            std::chrono::seconds{60}, n));
  }

  RepairResp Client::nodes_flow_repair(std::vector<unsigned int> failed_node_ids)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<&Coordinator::request_flow_repair>(
            std::chrono::seconds{500}, failed_node_ids, -1));
    if (!r) {
      std::cerr << "[RPC ERROR] request_flow_repair failed: " << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  RepairResp Client::nodes_flow_unordered_concurrency_repair(
          std::vector<unsigned int> failed_node_ids)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<
            &Coordinator::request_flow_unordered_concurrency_repair>(
            std::chrono::seconds{500}, failed_node_ids, -1));
    if (!r) {
      std::cerr << "[RPC ERROR] request_flow_unordered_concurrency_repair failed: "
                << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  RepairResp Client::nodes_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_node_ids)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<
            &Coordinator::request_flow_ordered_concurrency_repair>(
            std::chrono::seconds{500}, failed_node_ids, -1));
    if (!r) {
      std::cerr << "[RPC ERROR] request_flow_ordered_concurrency_repair failed: "
                << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  RepairResp Client::nodes_join_flow_ordered_concurrency_repair(
          std::vector<unsigned int> failed_node_ids)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<
            &Coordinator::request_join_flow_ordered_concurrency_repair>(
            std::chrono::seconds{500}, failed_node_ids, -1));
    if (!r) {
      std::cerr << "[RPC ERROR] request_join_flow_ordered_concurrency_repair failed: "
                << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  RepairResp Client::blocks_repair(std::vector<unsigned int> failed_block_ids, int stripe_id)
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call_for<&Coordinator::request_repair>(
            std::chrono::seconds{500}, failed_block_ids, stripe_id));
    if (!r) {
      std::cerr << "[RPC ERROR] request_repair(blocks) failed: "
                << r.error().msg << std::endl;
      return RepairResp{};
    }
    return r.value();
  }

  MergeResp Client::merge(int step_size)
  {
    auto response =
        async_simple::coro::syncAwait(
            rpc_coordinator_->call_for<&Coordinator::request_merge>(
                std::chrono::seconds{500}, step_size)).value();
    return response;
  }

  std::vector<unsigned int> Client::list_stripes()
  {
    auto response =
        async_simple::coro::syncAwait(
            rpc_coordinator_->call<&Coordinator::list_stripes>()).value();
    return response;
  }

  // 新增：让客户端可以向 Coordinator 发起快照和回滚的请求
  void Client::snapshot_metadata()
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call<&Coordinator::request_snapshot_metadata>());
    if (!r) {
      std::cerr << "[RPC ERROR] request_snapshot_metadata failed: "
                << r.error().msg << std::endl;
    }
  }

  void Client::revert_metadata()
  {
    auto r = async_simple::coro::syncAwait(
        rpc_coordinator_->call<&Coordinator::request_revert_metadata>());
    if (!r) {
      std::cerr << "[RPC ERROR] request_revert_metadata failed: "
                << r.error().msg << std::endl;
    }
  }
}
