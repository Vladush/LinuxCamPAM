
#include "auth_engine.hpp"
#include "constants.hpp"
#include "ipc_protocol.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running(true);
constexpr size_t BUFFER_SIZE = 1024;
constexpr int SOCKET_PERMS = 0666;
constexpr int BACKLOG = 5;
} // namespace

void signal_handler(int signum) {
  Logger::log(LogLevel::INFO,
              "Received signal " + std::to_string(signum) + ", stopping...");
  g_running = false;
}

struct Config {
  std::string socket_path = linuxcampam::SOCKET_PATH;
};

void handle_client(int fd, AuthEngine &engine) {
  linuxcampam::FileDescriptor client_fd(fd);
  std::array<char, BUFFER_SIZE> buffer = {};
  ssize_t valread = read(client_fd.get(), buffer.data(), buffer.size() - 1);
  if (valread <= 0) {
    return;
  }

  buffer[static_cast<size_t>(valread)] = '\0';
  std::string_view request(buffer.data(), static_cast<size_t>(valread));
  log_debug("Received Request: " + std::string(request));

  // Protocol: COMMAND argument via ipc_protocol.hpp
  // e.g. AUTH_REQUEST john | ADD_USER john | TRAIN_USER john default |
  // TEST_AUTH

  std::string response = "ERROR Unknown Command";
  using namespace linuxcampam::protocol;
  std::string cmd = "UNKNOWN";

  try {
    Request req = Request::deserialize(request);
    cmd = commandToString(req.cmd);
    log_debug("Command: " + commandToString(req.cmd));

    // Command Dispatch
    switch (req.cmd) {
    case Command::AUTH_REQUEST: {
      if (req.args.empty())
        break;
      bool success = engine.verifyUser(req.args[0]);
      response = success ? "AUTH_SUCCESS" : "AUTH_FAIL";
      break;
    }
    case Command::ADD_USER: {
      if (req.args.empty())
        break;
      auto Result = engine.enrollUser(req.args[0]);
      response =
          Result.first ? "ENROLL_SUCCESS" : ("ENROLL_FAIL " + Result.second);
      break;
    }
    case Command::TRAIN_USER: {
      if (req.args.empty())
        break;
      std::string label = (req.args.size() > 1) ? req.args[1] : "default";
      bool success = engine.trainUser(req.args[0], label, false);
      response = success ? "TRAIN_SUCCESS" : "TRAIN_FAIL";
      break;
    }
    case Command::GET_VERSION: {
      std::string ir_status = linuxcampam::getIREmitterVersion(
          engine.getConfig().ir_emitter_path.string());
      std::string ir_append =
          ir_status.empty() ? "" : " (IR Emitter: " + ir_status + ")";
#ifdef LINUXCAMPAM_VERSION
      response = std::string(LINUXCAMPAM_VERSION) + ir_append;
#else
      response = "Unknown" + ir_append;
#endif
      break;
    }
    case Command::TEST_AUTH: {
      std::string user = req.args.empty() ? "" : req.args[0];
      if (!user.empty()) {
        Logger::log(LogLevel::INFO, "Testing Auth for user: " + user);
        AuthResult result = engine.verifyUserWithDetails(user);
        std::string hw_status = "HW_OK";
        std::string auth_status =
            result.success ? "AUTH_SUCCESS" : ("AUTH_FAIL: " + result.reason);
        response = hw_status + " | " + auth_status;
      } else {
        bool hw_success = engine.testCameraAndAuth();
        response = hw_success ? "HW_OK" : "HW_FAIL";
      }
      break;
    }
    case Command::SET_LABEL: {
      if (req.args.size() < 2) {
        response = "ERROR Missing user or label";
      } else {
        bool success = engine.setLabel(req.args[0], req.args[1]);
        response = success ? "LABEL_SET" : "LABEL_FAIL";
      }
      break;
    }
    case Command::TRAIN_NEW: {
      if (req.args.empty()) {
        response = "ERROR Missing user";
      } else {
        std::string label = (req.args.size() > 1) ? req.args[1] : "default";
        bool success = engine.trainUser(req.args[0], label, true);
        response = success ? "TRAIN_SUCCESS" : "TRAIN_FAIL";
      }
      break;
    }

    case Command::LIST_EMBEDDINGS: {
      if (req.args.empty()) {
        response = "ERROR Missing user";
      } else {
        auto labels = engine.listEmbeddings(req.args[0]);
        if (labels.empty()) {
          response = "No embeddings found";
        } else {
          response = "Labels:";
          for (const auto &l : labels) {
            response += " " + l;
          }
        }
      }
      break;
    }
    case Command::REMOVE_EMBEDDING: {
      if (req.args.size() < 2) {
        response = "ERROR Missing user or label";
      } else {
        bool success = engine.removeEmbedding(req.args[0], req.args[1]);
        response = success ? "REMOVED" : "REMOVE_FAIL";
      }
      break;
    }
    case Command::GET_CONFIG: {
      response = engine.getConfigString();
      break;
    }
    case Command::SET_LOG_LEVEL: {
      if (req.args.empty())
        break;
      std::string levelStr = req.args[0];
      if (levelStr == "DEBUG") {
        Logger::setLevel(LogLevel::DEBUG);
        response = "LOG_LEVEL_DEBUG";
        log_info("Log level set to DEBUG via socket.");
      } else if (levelStr == "INFO") {
        Logger::setLevel(LogLevel::INFO);
        response = "LOG_LEVEL_INFO";
        log_info("Log level set to INFO via socket.");
      } else {
        response = "ERROR Invalid Log Level";
      }
      break;
    }
    case Command::GET_LOG_LEVEL: {
      auto lvl = Logger::getLevel();
      if (lvl == LogLevel::DEBUG)
        response = "DEBUG";
      else if (lvl == LogLevel::INFO)
        response = "INFO";
      else if (lvl == LogLevel::WARN)
        response = "WARN";
      else
        response = "ERROR";
      break;
    }
    default: {
      // response already set to unknown
      break;
    }
    }
  } catch (const std::exception &e) {
    Logger::log(LogLevel::ERROR, "Exception handling " + cmd + ": " + e.what());
    response = "ERROR Exception";
  } catch (...) {
    Logger::log(LogLevel::ERROR, "Unknown exception handling " + cmd);
    response = "ERROR Unknown Exception";
  }

  send(client_fd.get(), response.c_str(), response.length(), 0);
}

int main(int argc, char *argv[]) {
  (void)signal(SIGINT, signal_handler);
  (void)signal(SIGTERM, signal_handler);

  // Parse args
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--debug" || arg == "-d") {
      Logger::setLevel(LogLevel::DEBUG);
      log_debug("Debug logging enabled via command line.");
    }
  }

  // Ensure run directory exists
  std::string socket_path = linuxcampam::SOCKET_PATH;
  // In dev mode, maybe use local tmp if not root?
  // For now assuming system service usage or sudo.
  // Create directory for socket if needed
  fs::path p(socket_path);
  if (p.has_parent_path()) {
    fs::create_directories(p.parent_path());
  }

  // Config path
  std::string config_path = linuxcampam::CONFIG_PATH;
  // Fallback to local config for dev
  if (!fs::exists(config_path)) {
    config_path = "config.ini";
  }

  Logger::log(LogLevel::INFO, "Starting LinuxCamPAM Service...");
  Logger::log(LogLevel::INFO, "Loading Config: " + config_path);

  // Wipe OpenCL cache - stale kernels cause hangs after Mesa updates
  const char *home = getenv("HOME");
  if (!home)
    home = "/root";
  fs::path opencv_cache = fs::path(home) / ".cache" / "opencv";
  if (fs::exists(opencv_cache)) {
    try {
      fs::remove_all(opencv_cache);
      Logger::log(LogLevel::DEBUG, "Cleared OpenCL cache");
    } catch (const std::exception &e) {
      Logger::log(LogLevel::DEBUG, "OpenCL cache cleanup failed: " + std::string(e.what()));
    }
  }

  AuthEngine engine;
  // Initialize Engine
  if (!engine.init(config_path)) {
    Logger::log(LogLevel::ERROR, "AuthEngine init failed, shutting down.");
    return 1;
  }

  // Socket Setup
  linuxcampam::FileDescriptor server_fd(socket(AF_UNIX, SOCK_STREAM, 0));
  struct sockaddr_un address = {};

  if (!server_fd.isValid()) {
    perror("socket failed");
    return 1;
  }

  address.sun_family = AF_UNIX;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  (void)std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                      socket_path.c_str());

  (void)unlink(socket_path.c_str()); // Remove old socket
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(server_fd.get(), reinterpret_cast<struct sockaddr *>(&address),
           sizeof(address)) < 0) {
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    perror("bind failed");
    return 1;
  }

  // Enable syslog for daemon logging
  Logger::enableSyslog("linuxcampamd");

  // World-readable socket allows console users to trigger authentication
  chmod(socket_path.c_str(), SOCKET_PERMS);

  if (listen(server_fd.get(), BACKLOG) < 0) {
    perror("listen");
    return 1;
  }

  Logger::log(LogLevel::INFO, "Listening on " + socket_path);

  while (g_running) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server_fd.get(), &readfds);

    // Timeout for select to allow checking g_running
    struct timeval timeout = {};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int activity = select(server_fd.get() + 1, &readfds, NULL, NULL, &timeout);

    if ((activity < 0) && (errno != EINTR)) {
      // Error
    } else if (activity == 0) {
      // Timeout: perform maintenance
      (void)engine.performMaintenance();
    }

    if (g_running && activity > 0 && FD_ISSET(server_fd.get(), &readfds)) {
      int new_socket = 0;
      int addrlen = sizeof(address);
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      new_socket =
          accept(server_fd.get(), reinterpret_cast<struct sockaddr *>(&address),
                 reinterpret_cast<socklen_t *>(&addrlen));
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      if (new_socket >= 0) {
        // Handle in thread or blocking? Blocking for now - camera is
        // single-access anyway
        handle_client(new_socket, engine);
      }
    }
  }

  (void)unlink(socket_path.c_str());
  Logger::log(LogLevel::INFO, "Stopped.");
  return 0;
}
