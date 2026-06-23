#include "datanode.h"

namespace ECProject
{
  Datanode::Datanode(std::string ip, int port)
      : ip_(ip), port_(port), 
        acceptor_(io_context_, 
                  asio::ip::tcp::endpoint(asio::ip::address::from_string(ip.c_str()), 
                  port + SOCKET_PORT_OFFSET))
  {
    easylog::set_min_severity(easylog::Severity::ERROR);
    // port is for rpc 
    rpc_server_ = std::make_unique<coro_rpc::coro_rpc_server>(1, port_);
    rpc_server_->register_handler<&Datanode::checkalive>(this);
    rpc_server_->register_handler<&Datanode::handle_set>(this);
    rpc_server_->register_handler<&Datanode::handle_get>(this);
    rpc_server_->register_handler<&Datanode::handle_delete>(this);
    rpc_server_->register_handler<&Datanode::handle_transfer>(this);
    // port + SOCKET_PORT_OFFSET is for socket
    port_for_transfer_data_ = port + SOCKET_PORT_OFFSET;

    #ifdef IN_MEMORY
      // port + STORAGE_SERVER_OFFSET is for storage server
      #ifdef MEMCACHED
        memcached_return rc;
        memcached_ = memcached_create(NULL);
        memcached_server_st *servers;
        servers = memcached_server_list_append(NULL, ip.c_str(), port + STORAGE_SERVER_OFFSET, &rc);
        rc = memcached_server_push(memcached_, servers);
        memcached_server_free(servers);
        memcached_behavior_set(memcached_, MEMCACHED_BEHAVIOR_DISTRIBUTION, MEMCACHED_DISTRIBUTION_CONSISTENT);
        memcached_behavior_set(memcached_, MEMCACHED_BEHAVIOR_RETRY_TIMEOUT, 20);
        memcached_behavior_set(memcached_, MEMCACHED_BEHAVIOR_SERVER_FAILURE_LIMIT, 5);
        memcached_behavior_set(memcached_, MEMCACHED_BEHAVIOR_AUTO_EJECT_HOSTS, true);
      #endif
      #ifdef REDIS
        std::string url = "tcp://" + ip_ + ":" + std::to_string(port_ + STORAGE_SERVER_OFFSET);
        redis_ = std::make_unique<sw::redis::Redis>(url);
      #endif
    #else
      std::string targetdir = "./storage/";
      if (access(targetdir.c_str(), 0) == -1) {
        mkdir(targetdir.c_str(), S_IRWXU);
      }
    #endif
  }

  Datanode::~Datanode()
  {
    acceptor_.close();
    rpc_server_->stop();
  }

  void Datanode::run()
  {
    auto err = rpc_server_->start();
  }

  bool Datanode::checkalive()
  {
    return true;
  }

  bool Datanode::store_data(std::string& key_buf, std::string& value_buf, size_t value_size, double *disk_io_sec)
  {
    struct timeval io_s{}, io_e{};
    gettimeofday(&io_s, NULL);
    #ifdef IN_MEMORY
      #ifdef MEMCACHED
        memcached_return_t rc = memcached_set(memcached_, 
                                               (const char *)key_buf.data(),
                                               key_buf.size(),
                                               (const char *)value_buf.data(),
                                               value_buf.size(),
                                               (time_t)0, (uint32_t)0);
        if (rc != MEMCACHED_SUCCESS) {
          std::cout << "[Datanode" << port_ << "][Memcached] failed to set"
                    << key_buf << "! "
                    << memcached_strerror(memcached_, rc) << std::endl;
          return false;
        }
      #endif
      #ifdef REDIS
        auto status = redis_->set(key_buf, value_buf);
        if (!status) {
          std::cout << "[Datanode" << port_ << "][Redis] failed to set"
                    << key_buf << "!\n";
          return false;
        }
      #endif
      #ifndef MEMCACHED
        #ifndef REDIS
          auto ret = kvstore_.insert(std::make_pair(key_buf, value_buf));
          if (!ret.second) {
            std::cout << "[Datanode" << port_ << "][kvstore] failed to set"
                      << key_buf << "!\n";
            return false;
          }
        #endif
      #endif
    #else
      std::string targetdir = "./storage/" + std::to_string(port_) + "/";
      std::string writepath = targetdir + key_buf;
      if (access(targetdir.c_str(), 0) == -1) {
        mkdir(targetdir.c_str(), S_IRWXU);
      }
      std::ofstream ofs(writepath, std::ios::binary | std::ios::out | std::ios::trunc);
      ofs.write(value_buf.data(), value_size);
      ofs.flush();
      ofs.close();
      if (!ofs) {
        std::cout << "[Datanode" << port_ << "][Disk] failed to set!\n";
        return false;
      }
    #endif
    gettimeofday(&io_e, NULL);
    if (disk_io_sec) {
      *disk_io_sec = (io_e.tv_sec - io_s.tv_sec) +
                     (io_e.tv_usec - io_s.tv_usec) * 1.0 / 1000000;
    }
    return true;
  }

  bool Datanode::access_data(std::string& key_buf, std::string& value_buf, size_t value_size, double *disk_io_sec)
  {
    struct timeval io_s{}, io_e{};
    gettimeofday(&io_s, NULL);
    #ifdef IN_MEMORY
      #ifdef MEMCACHED
        memcached_return_t rc;
        uint32_t flag;
        char *value = memcached_get(memcached_, (const char *)key_buf.data(),
                                    key_buf.size(), &value_size, &flag, &rc);
        if (value == nullptr) {
          std::cout << "[Datanode" << port_ << "][Memcached][Get] error!"
                    << memcached_strerror(memcached_, rc) << std::endl;
          return false;
        } else {
          value_buf.assign(value, value_size);
        }
      #endif
      #ifdef REDIS
        if(!redis_->exists(key_buf)) {
          std::cout << "[Datanode" << port_ << "][Redis][Get] key not found!" << std::endl;
          return false;
        } else {
          auto value_returned = redis_->get(key_buf);
          my_assert(value_returned.has_value());
          value_buf = value_returned.value();
        }
      #endif
      #ifndef MEMCACHED
        #ifndef REDIS
          auto it = kvstore_.find(key_buf);
          if (it == kvstore_.end()) {
            std::cout << "[Datanode" << port_ << "][kvstore][Get] key not found!" << std::endl;
            return false;
          } else {
            value_buf = it->second;
          }
        #endif
      #endif
    #else
      // on disk
      std::string targetdir = "./storage/" + std::to_string(port_) + "/";
      std::string readpath = targetdir + key_buf;
      if (access(readpath.c_str(), 0) == -1) {
        std::cout << "[Datanode" << port_ << "][Disk][Get] file does not exist!"
                  << readpath << std::endl;
        return false;
      } else {
        std::ifstream ifs(readpath);
        ifs.read(value_buf.data(), value_size);
        ifs.close();
      }
    #endif
    gettimeofday(&io_e, NULL);
    if (disk_io_sec) {
      *disk_io_sec = (io_e.tv_sec - io_s.tv_sec) +
                     (io_e.tv_usec - io_s.tv_usec) * 1.0 / 1000000;
    }
    return true;
  }

  void Datanode::handle_set(std::string src_ip, int src_port, bool ispull)
  {
    auto handler_push = [this]() mutable
    {
      try
      {
        asio::error_code ec;
        asio::ip::tcp::socket socket_(io_context_);
        acceptor_.accept(socket_);

        std::vector<unsigned char> size_buf(sizeof(int));
        asio::read(socket_, asio::buffer(size_buf, size_buf.size()), ec);
        int key_size = bytes_to_int(size_buf);
        asio::read(socket_, asio::buffer(size_buf, size_buf.size()), ec);
        int value_size = bytes_to_int(size_buf);

        if (value_size > 0) {
          std::string key_buf(key_size, 0);
          std::string value_buf(value_size, 0);
          asio::read(socket_, asio::buffer(key_buf.data(), key_buf.size()), ec);
          asio::read(socket_, asio::buffer(value_buf.data(), value_buf.size()), ec);

          double disk_io = 0;
          bool ret = store_data(key_buf, value_buf, value_size, &disk_io);

          if (ret) {  // response
            std::vector<unsigned char> finish = int_to_bytes(1);
            asio::write(socket_, asio::buffer(finish, finish.size()));
          } else {
            std::vector<unsigned char> finish = int_to_bytes(0);
            asio::write(socket_, asio::buffer(finish, finish.size()));
          }
          std::vector<unsigned char> io_buf = double_to_bytes(disk_io);
          asio::write(socket_, asio::buffer(io_buf, io_buf.size()));

          asio::error_code ignore_ec;
          socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
          socket_.close(ignore_ec);

          if (IF_DEBUG) {
            std::cout << "[Datanode" << port_ << "][Write] successfully write "
                      << key_buf << " with " << value_size << "bytes" << std::endl;
          }
        } else {
          std::cout << "[Datanode" << port_ << "][Write] no value recieved!" << std::endl;
        }         
      } 
      catch (const std::exception &e)
      {
        std::cerr << e.what() << '\n';
      }
    };
    auto handler_pull = [this, src_ip, src_port]() mutable
    {
      try
      {
        asio::ip::tcp::socket socket_(io_context_);
        asio::ip::tcp::resolver resolver(io_context_);
        asio::error_code con_error;
        asio::connect(socket_, resolver.resolve({std::string(src_ip), std::to_string(src_port)}), con_error);
        asio::error_code ec;
        if (!con_error && IF_DEBUG) {
          std::cout << "[Datanode" << port_ << "] Connect to " << src_ip
                    << ":" << src_port << " success!" << std::endl;
        }

        std::vector<unsigned char> size_buf(sizeof(int));
        asio::read(socket_, asio::buffer(size_buf, size_buf.size()), ec);
        int key_size = bytes_to_int(size_buf);
        asio::read(socket_, asio::buffer(size_buf, size_buf.size()), ec);
        int value_size = bytes_to_int(size_buf);

        if (IF_DEBUG) {
          std::cout << "[Datanode" << port_ << "] Read " << key_size
                    << " bytes key and " << value_size << " bytes value" << std::endl;
        }

        if (value_size > 0) {
          std::string key_buf(key_size, 0);
          std::string value_buf(value_size, 0);
          asio::read(socket_, asio::buffer(key_buf.data(), key_buf.size()), ec);
          asio::read(socket_, asio::buffer(value_buf.data(), value_buf.size()), ec);

          if (IF_DEBUG) {
            std::cout << "[Datanode" << port_ << "][Write] recieved "
                      << key_buf << " with " << value_size << "bytes" << std::endl;
          }

          double disk_io = 0;
          bool ret = store_data(key_buf, value_buf, value_size, &disk_io);

          if (ret) {  // response
            std::vector<unsigned char> finish = int_to_bytes(1);
            asio::write(socket_, asio::buffer(finish, finish.size()));
          } else {
            std::vector<unsigned char> finish = int_to_bytes(0);
            asio::write(socket_, asio::buffer(finish, finish.size()));
          }
          std::vector<unsigned char> io_buf = double_to_bytes(disk_io);
          asio::write(socket_, asio::buffer(io_buf, io_buf.size()));

          asio::error_code ignore_ec;
          socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
          socket_.close(ignore_ec);

          if (IF_DEBUG) {
            std::cout << "[Datanode" << port_ << "][Write] successfully write "
                      << key_buf << " with " << value_size << "bytes" << std::endl;
          }
        } else {
          std::cout << "[Datanode" << port_ << "][Write] no value recieved!" << std::endl;
        }
      }
      catch (const std::exception &e)
      {
        std::cerr << e.what() << '\n';
      }
    };
    try
    {
      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ << "][SET] ready to handle set!" << std::endl;
      }
      if (ispull) {
        std::thread my_thread(handler_pull);
        my_thread.join();
      } else {
        std::thread my_thread(handler_push);
        my_thread.detach();
      }
    }
    catch (std::exception &e)
    {
      std::cout << "exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
  }

  void Datanode::handle_get(std::string key, size_t key_len, size_t value_len)
  {
    auto handler = [this, key, key_len, value_len]() mutable
    {
      asio::error_code ec;
      asio::ip::tcp::socket socket_(io_context_);
      acceptor_.accept(socket_);

      std::vector<unsigned char> key_size_buf = int_to_bytes(key_len);
      asio::write(socket_, asio::buffer(key_size_buf, key_size_buf.size()));

      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ << "][GET] read " << key 
                  << " and write to socket with port " << port_for_transfer_data_
                  << std::endl;
      }
      std::string value_buf(value_len, 0);
      double disk_io = 0;
      bool ret = access_data(key, value_buf, value_len, &disk_io);

      if (ret) {
        if (IF_DEBUG) {
          std::cout << "[Datanode" << port_ << "][GET] read " << key
                    << " with length of " << value_buf.length() << std::endl;
        }
        std::vector<unsigned char> value_size_buf = int_to_bytes(value_len);
        asio::write(socket_, asio::buffer(value_size_buf, value_size_buf.size()));

        asio::write(socket_, asio::buffer(key, key_len));
        asio::write(socket_, asio::buffer(value_buf.data(), value_buf.length()));

        std::vector<unsigned char> finish_buf(sizeof(int));
        asio::read(socket_, asio::buffer(finish_buf, finish_buf.size()));
        int finish = bytes_to_int(finish_buf);
        std::vector<unsigned char> io_buf = double_to_bytes(disk_io);
        asio::write(socket_, asio::buffer(io_buf, io_buf.size()));
        if (!finish && IF_DEBUG) {
          std::cout << "[Datanode" << port_ << "][GET] destination set failed!" << std::endl;
        }
      } else {
        std::vector<unsigned char> value_size_buf = int_to_bytes(0);
        asio::write(socket_, asio::buffer(value_size_buf, value_size_buf.size()));
        std::vector<unsigned char> io_buf = double_to_bytes(0.0);
        asio::write(socket_, asio::buffer(io_buf, io_buf.size()));
      }

      asio::error_code ignore_ec;
      socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
      socket_.close(ignore_ec);
      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ << "][GET] write to socket!" << std::endl;
      }
    };
    try
    {
      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ << "][GET] ready to handle get!" << std::endl;
      }
      std::thread my_thread(handler);
      my_thread.detach();
    }
    catch (std::exception &e)
    {
      std::cout << "exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
  }

  void Datanode::handle_delete(std::string block_key)
  {
    #ifdef IN_MEMORY
      #ifdef MEMCACHED
        memcached_return_t rc;
        rc = memcached_delete(memcached_, block_key.data(), block_key.size(), (time_t)0);
        if (rc != MEMCACHED_SUCCESS) {
          std::cout << "[DEL] delete error! "
                    << memcached_strerror(memcached_, rc) << std::endl;
        } else {
          if (IF_DEBUG) {
            std::cout << "[Datanode" << port_
                      << "][memcached][Delete] successfully delete "
                      << block_key << std::endl;
          }
        }
      #endif
      #ifdef REDIS
        if (!redis_->exists(block_key)) {
          std::cout << "[DEL] key not found!" << std::endl;
        } else {
          redis_->del(block_key);
          if (IF_DEBUG) {
              std::cout << "[Datanode" << port_
                        << "][redis][Delete] successfully delete "
                        << block_key << std::endl;
            }
        }
      #endif
      #ifndef MEMCACHED
        #ifndef REDIS
          auto it = kvstore_.find(block_key);
          if (it == kvstore_.end()) {
            std::cout << "[Datanode" << port_
                      << "][kvstore][Get] key not found!" << std::endl;
          } else {
            kvstore_.erase(it);
            if (IF_DEBUG) {
              std::cout << "[Datanode" << port_
                        << "][kvstore][Delete] successfully delete "
                        << block_key << std::endl;
            }
          }
        #endif
      #endif
    #else
      std::string file_path = "./storage/" + std::to_string(port_) + "/" + block_key;
      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ << "] File path:" << file_path << std::endl;
      }
      if (remove(file_path.c_str())) {
        std::cout << "[DEL] delete error!" << std::endl;
      }
    #endif
  }

  void Datanode::handle_transfer()
  {
    auto handler = [this]() mutable
    {
      asio::error_code ec;
      asio::ip::tcp::socket socket_(io_context_);
      acceptor_.accept(socket_);

      std::vector<unsigned char> value_size_buf(sizeof(int));
      asio::read(socket_, asio::buffer(value_size_buf, value_size_buf.size()), ec);
      int value_size = bytes_to_int(value_size_buf);

      std::string value_buf(value_size, 0);
      asio::read(socket_, asio::buffer(value_buf.data(), value_buf.size()), ec);

      // response
      std::vector<unsigned char> finish = int_to_bytes(1);
      asio::write(socket_, asio::buffer(finish, finish.size()));

      asio::error_code ignore_ec;
      socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
      socket_.close(ignore_ec);

      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_
                  << "][Transfer] successfully transfer with "
                  << value_size << "bytes." << std::endl;
      }
    };
    try
    {
      if (IF_DEBUG) {
        std::cout << "[Datanode" << port_ 
                  << "][Transfer] ready to handle cross-cluster transfer!"
                  << std::endl;
      }
      std::thread my_thread(handler);
      my_thread.detach();
    }
    catch (std::exception &e)
    {
      std::cout << "exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
  }
}