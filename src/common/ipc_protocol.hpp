#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace linuxcampam {
namespace protocol {

enum class Command {
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

  [[nodiscard]] static Request deserialize(const std::string &data) {
    std::istringstream iss(data);
    std::string cmdStr;
    iss >> cmdStr;
    Request req{stringToCommand(cmdStr), {}};
    std::string arg;
    while (iss >> arg) {
      req.args.push_back(arg);
    }
    return req;
  }
};

} // namespace protocol
} // namespace linuxcampam
