#pragma once

#include "metadata.h"
#include <map>
#include <asio.hpp>
#include <fstream>
#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>
#ifdef IN_MEMORY
  #ifdef MEMCACHED
    #include <libmemcached/memcached.h>
  #endif
  #ifdef REDIS
    #include <sw/redis++/redis++.h>
  #endif
#else
  #include <unistd.h>
#endif


namespace ECProject
{
  class Datanode
  {
  public:
    Datanode(std::string ip, int port);
    ~Datanode();
    void run();

    // rpc调用
    bool checkalive();
    // set
    void handle_set(std::string src_ip, int src_port, bool ispull);
    // get
    void handle_get(std::string key, size_t key_len, size_t value_len);
    // delete
    void handle_delete(std::string block_key);
    // simulate cross-cluster transfer
    void handle_transfer();

  private:
    bool store_data(std::string& key, std::string& value, size_t value_size, double *disk_io_sec = nullptr);
    bool access_data(std::string& key, std::string& value, size_t value_size, double *disk_io_sec = nullptr);
    std::unique_ptr<coro_rpc::coro_rpc_server> rpc_server_{nullptr};
    std::string ip_;
    int port_;
    int port_for_transfer_data_;
    asio::io_context io_context_{};
    asio::ip::tcp::acceptor acceptor_;
    #ifdef IN_MEMORY
      #ifdef MEMCACHED
        memcached_st *memcached_;
      #endif
      #ifdef REDIS
        std::unique_ptr<sw::redis::Redis> redis_{nullptr};
      #endif
      #ifndef MEMCACHED
        #ifndef REDIS
          std::map<std::string, std::string> kvstore_;
        #endif
      #endif
    #endif
  };
}
