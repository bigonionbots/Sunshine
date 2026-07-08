/**
 * @file src/platform/android/misc.cpp
 * @brief Android platform plumbing: process, networking, threads, and host info.
 * @details Most functions here are portable POSIX and are adapted from the Linux backend.
 *          The Android-specific pieces (appdata, host name, url opening) defer to the JNI
 *          bridge and fall back to sane defaults so the binary can boot headless during the
 *          bring-up spike, before any host app/service exists.
 */
// standard includes
#include <cstring>
#include <thread>

// platform includes
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/process/v1.hpp>

// local includes
#include "src/platform/android/jni_bridge.h"
#include "src/platform/android/misc.h"
#include "src/platform/common.h"
#include "src/logging.h"

namespace bp = boost::process::v1;

using namespace std::literals;

namespace platf {

  namespace {
    /**
     * @brief Convert a Boost.Asio address and port into a native sockaddr.
     *
     * @param addr Source Boost.Asio IP address.
     * @param port Destination UDP port in host byte order.
     * @param out Storage populated with the native socket address.
     * @return Length in bytes of the populated socket address.
     */
    socklen_t to_native(const boost::asio::ip::address &addr, uint16_t port, sockaddr_storage &out) {
      std::memset(&out, 0, sizeof(out));
      if (addr.is_v6()) {
        auto *sa = reinterpret_cast<sockaddr_in6 *>(&out);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(port);
        auto bytes = addr.to_v6().to_bytes();
        std::memcpy(&sa->sin6_addr, bytes.data(), bytes.size());
        return sizeof(sockaddr_in6);
      }
      auto *sa = reinterpret_cast<sockaddr_in *>(&out);
      sa->sin_family = AF_INET;
      sa->sin_port = htons(port);
      auto bytes = addr.to_v4().to_bytes();
      std::memcpy(&sa->sin_addr, bytes.data(), bytes.size());
      return sizeof(sockaddr_in);
    }
  }  // namespace

  std::filesystem::path appdata() {
    // A host app would pass its Context.getFilesDir() through the JNI bridge. Until then,
    // allow an env override and fall back to a root-writable scratch location.
    if (auto env = std::getenv("SUNSHINE_APPDATA")) {
      return std::filesystem::path {env};
    }
    return std::filesystem::path {"/data/local/tmp/sunshine"};
  }

  std::string from_sockaddr(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};
    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    }
    return std::string {data};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};
    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }
    return {port, std::string {data}};
  }

  std::string get_mac_address(const std::string_view &address) {
    // Modern Android restricts hardware MAC access for unprivileged apps. Advertising an
    // empty MAC simply disables Wake-on-LAN, which is acceptable for a streaming host.
    return {};
  }

  std::string get_host_name() {
    if (auto name = jni::device_name()) {
      return *name;
    }
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') {
      return std::string {buffer};
    }
    return "Sunshine-Android"s;
  }

  bool send(send_info_t &send_info) {
    sockaddr_storage dst {};
    socklen_t dst_len = to_native(send_info.target_address, send_info.target_port, dst);

    iovec iov[2];
    int iov_count = 0;
    if (send_info.header && send_info.header_size) {
      iov[iov_count++] = {const_cast<char *>(send_info.header), send_info.header_size};
    }
    iov[iov_count++] = {const_cast<char *>(send_info.payload), send_info.payload_size};

    msghdr msg {};
    msg.msg_name = &dst;
    msg.msg_namelen = dst_len;
    msg.msg_iov = iov;
    msg.msg_iovlen = iov_count;

    return sendmsg(static_cast<int>(send_info.native_socket), &msg, 0) >= 0;
  }

  bool send_batch(batched_send_info_t &send_info) {
    // Minimal, portable fallback: emit one datagram per block. The Linux backend uses
    // sendmmsg/GSO for throughput; that optimization can be ported once the path works.
    sockaddr_storage dst {};
    socklen_t dst_len = to_native(send_info.target_address, send_info.target_port, dst);

    for (size_t i = 0; i < send_info.block_count; ++i) {
      size_t block = send_info.block_offset + i;
      auto payload = send_info.buffer_for_payload_offset(static_cast<ptrdiff_t>(block * send_info.payload_size));

      iovec iov[2];
      int iov_count = 0;
      if (send_info.headers && send_info.header_size) {
        iov[iov_count++] = {const_cast<char *>(send_info.headers + block * send_info.header_size), send_info.header_size};
      }
      iov[iov_count++] = {const_cast<char *>(payload.buffer), send_info.payload_size};

      msghdr msg {};
      msg.msg_name = &dst;
      msg.msg_namelen = dst_len;
      msg.msg_iov = iov;
      msg.msg_iovlen = iov_count;

      if (sendmsg(static_cast<int>(send_info.native_socket), &msg, 0) < 0) {
        return false;
      }
    }
    return true;
  }

  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    // DSCP/SO_PRIORITY tagging is frequently blocked on Android; treat as a no-op for now.
    return nullptr;
  }

  bp::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    // Android has no general "launch an application binary" model — apps are Activities.
    // Prep/detached commands need a host-app brokered launch (Intent) instead. Report
    // unsupported so process.cpp fails gracefully rather than crashing.
    ec = std::make_error_code(std::errc::not_supported);
    return {};
  }

  void open_url(const std::string &url) {
    jni::open_url(url);
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    return false;
  }

  bool process_group_running(std::uintptr_t native_handle) {
    return false;
  }

  void adjust_thread_priority(thread_priority_e priority) {
    int nice_value = 0;
    switch (priority) {
      case thread_priority_e::low:
        nice_value = 5;
        break;
      case thread_priority_e::normal:
        nice_value = 0;
        break;
      case thread_priority_e::high:
        nice_value = -5;
        break;
      case thread_priority_e::critical:
        nice_value = -10;
        break;
    }
    setpriority(PRIO_PROCESS, 0, nice_value);
  }

  void set_thread_name(const std::string &name) {
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
  }

  void enable_mouse_keys() {}

  void streaming_will_start() {}

  void streaming_will_stop() {}

  void restart() {}

  bool needs_encoder_reenumeration() {
    return false;
  }

  std::string resolve_render_device() {
    // No /dev/dri render node model on Android; hardware encode is brokered via MediaCodec.
    return {};
  }

  platform_caps::caps_t get_capabilities() {
    // Touch is the natural first-class input on Android; pen support can follow.
    return platform_caps::pen_touch;
  }

  bool has_elevated_privileges(bool all_caps) {
    return geteuid() == 0;
  }

  void drop_elevated_privileges(bool all_caps) {
    // Deliberately a no-op: the rooted-device model keeps privileges for uinput/capture.
  }

  /**
   * @brief Android high-precision timer backed by the standard library sleep.
   */
  class android_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<android_high_precision_timer>();
  }

  std::unique_ptr<deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

}  // namespace platf
