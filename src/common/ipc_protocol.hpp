#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

namespace linuxcampam {
namespace protocol {

enum class Command : std::uint8_t {
  AUTH_REQUEST,
  ADD_USER,
  TRAIN_USER,
  TEST_AUTH,
  SET_LABEL,
  TRAIN_NEW,
  LIST_EMBEDDINGS,
  REMOVE_EMBEDDING,
  GET_CONFIG,
  SET_LOG_LEVEL,
  GET_LOG_LEVEL,
  GET_VERSION,
  UNKNOWN
};

inline std::string commandToString(Command cmd) {
  switch (cmd) {
  case Command::AUTH_REQUEST:
    return "AUTH_REQUEST";
  case Command::ADD_USER:
    return "ADD_USER";
  case Command::TRAIN_USER:
    return "TRAIN_USER";
  case Command::TEST_AUTH:
    return "TEST_AUTH";
  case Command::SET_LABEL:
    return "SET_LABEL";
  case Command::TRAIN_NEW:
    return "TRAIN_NEW";
  case Command::LIST_EMBEDDINGS:
    return "LIST_EMBEDDINGS";
  case Command::REMOVE_EMBEDDING:
    return "REMOVE_EMBEDDING";
  case Command::GET_CONFIG:
    return "GET_CONFIG";
  case Command::SET_LOG_LEVEL:
    return "SET_LOG_LEVEL";
  case Command::GET_LOG_LEVEL:
    return "GET_LOG_LEVEL";
  case Command::GET_VERSION:
    return "GET_VERSION";
  default:
    return "UNKNOWN";
  }
}

inline Command stringToCommand(std::string_view str) {
  if (str == "AUTH_REQUEST")
    return Command::AUTH_REQUEST;
  if (str == "ADD_USER")
    return Command::ADD_USER;
  if (str == "TRAIN_USER")
    return Command::TRAIN_USER;
  if (str == "TEST_AUTH")
    return Command::TEST_AUTH;
  if (str == "SET_LABEL")
    return Command::SET_LABEL;
  if (str == "TRAIN_NEW")
    return Command::TRAIN_NEW;
  if (str == "LIST_EMBEDDINGS")
    return Command::LIST_EMBEDDINGS;
  if (str == "REMOVE_EMBEDDING")
    return Command::REMOVE_EMBEDDING;
  if (str == "GET_CONFIG")
    return Command::GET_CONFIG;
  if (str == "SET_LOG_LEVEL")
    return Command::SET_LOG_LEVEL;
  if (str == "GET_LOG_LEVEL")
    return Command::GET_LOG_LEVEL;
  if (str == "GET_VERSION")
    return Command::GET_VERSION;
  return Command::UNKNOWN;
}

struct Request {
  Command cmd;
  std::vector<std::string> args;

  [[nodiscard]] std::string serialize() const {
    std::string out = commandToString(cmd);
    for (const auto &arg : args) {
      out += " " + arg;
    }
    return out;
  }

  [[nodiscard]] static Request deserialize(std::string_view data) {
    auto cmd_end = data.find(' ');
    auto cmd_sv = (cmd_end == std::string_view::npos) ? data : data.substr(0, cmd_end);
    Request req{stringToCommand(cmd_sv), {}};

    if (cmd_end != std::string_view::npos) {
      auto rest = data.substr(cmd_end + 1);
      size_t start = 0;
      while (start < rest.size()) {
        auto pos = rest.find(' ', start);
        auto token = rest.substr(start, (pos == std::string_view::npos) ? std::string_view::npos : pos - start);
        if (!token.empty()) {
          req.args.emplace_back(token);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 1;
      }
    }
    return req;
  }
};

constexpr uint8_t PROTOCOL_VERSION = 1;

inline bool send_message(int fd, const std::string& msg, int timeout_ms = 2000) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint8_t header[5];
    header[0] = PROTOCOL_VERSION;
    uint32_t len = msg.size();
    memcpy(header + 1, &len, sizeof(len));

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    size_t sent = 0;
    while (sent < sizeof(header)) {
        ssize_t n = send(fd, header + sent, sizeof(header) - sent, flags);
        if (n <= 0) return false;
        sent += n;
    }

    sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, msg.data() + sent, len - sent, flags);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

inline bool recv_message(int fd, std::string& out_msg, int timeout_ms = 2000) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t header[5];
    size_t received = 0;
    while (received < sizeof(header)) {
        ssize_t n = recv(fd, header + received, sizeof(header) - received, 0);
        if (n <= 0) return false;
        received += n;
    }

    if (header[0] != PROTOCOL_VERSION) return false;
    uint32_t len = 0;
    memcpy(&len, header + 1, sizeof(len));

    if (len > 1024 * 1024) return false; // 1MB sanity limit

    out_msg.resize(len);
    received = 0;
    while (received < len) {
        ssize_t n = recv(fd, out_msg.data() + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

} // namespace protocol
} // namespace linuxcampam
