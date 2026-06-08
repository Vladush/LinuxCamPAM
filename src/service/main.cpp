
#include "auth_engine.hpp"
#include "constants.hpp"
#include "ipc_protocol.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "HardwareManager.hpp"
#include "SensorFactory.hpp"
#include "PresenceTripwire.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <optional>
#include <string>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running(true);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_proximity_present(true); // Default to true so auth works if sensor is disabled
constexpr size_t BUFFER_SIZE = 1024; // Buffer size for incoming UNIX socket messages
constexpr int SOCKET_PERMS = 0666; // File permissions for the UNIX socket (world-readable/writable)
constexpr int BACKLOG = 5; // Maximum length of the pending connections queue for the socket
} // namespace

void signal_handler(int signum) {
  log_info("Received signal " + std::to_string(signum) + ", stopping...");
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

  buffer.at(static_cast<size_t>(valread)) = '\0';
  std::string_view request(buffer.data(), static_cast<size_t>(valread));
  log_debug("Received Request: " + std::string(request));

  struct ucred cred{};
  socklen_t len = sizeof(struct ucred);
  if (getsockopt(client_fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
    log_error("Failed to retrieve peer credentials.");
    return;
  }

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

    auto check_permission = [&](const std::string& target_user) -> bool {
      if (cred.uid == 0) return true; // root always allowed
      auto target_uid = linuxcampam::getUidForUsername(target_user);
      if (!target_uid) return false; // invalid user
      return cred.uid == *target_uid;
    };

    // Command Dispatch
    switch (req.cmd) {
    case Command::AUTH_REQUEST: {
      if (req.args.empty())
        break;
      
      if (engine.getConfig().proximity_enforce && !g_proximity_present.load()) {
        response = "AUTH_FAIL: Proximity sensor reports no human present";
        log_warn("Authentication rejected by proximity sensor enforcement.");
        break;
      }

      bool success = engine.verifyUser(req.args[0]);
      response = success ? "AUTH_SUCCESS" : "AUTH_FAIL";
      break;
    }
    case Command::ADD_USER: {
      if (req.args.empty())
        break;
      if (!check_permission(req.args[0])) {
        response = "ERROR Permission Denied";
        break;
      }
      auto Result = engine.enrollUser(req.args[0]);
      response =
          Result.first ? "ENROLL_SUCCESS" : ("ENROLL_FAIL " + Result.second);
      break;
    }
    case Command::TRAIN_USER: {
      if (req.args.empty())
        break;
      if (!check_permission(req.args[0])) {
        response = "ERROR Permission Denied";
        break;
      }
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
        if (!check_permission(user)) {
          response = "ERROR Permission Denied";
          break;
        }
        log_info("Testing Auth for user: " + user);
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
        if (!check_permission(req.args[0])) {
          response = "ERROR Permission Denied";
          break;
        }
        bool success = engine.setLabel(req.args[0], req.args[1]);
        response = success ? "LABEL_SET" : "LABEL_FAIL";
      }
      break;
    }
    case Command::TRAIN_NEW: {
      if (req.args.empty()) {
        response = "ERROR Missing user";
      } else {
        if (!check_permission(req.args[0])) {
          response = "ERROR Permission Denied";
          break;
        }
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
        if (!check_permission(req.args[0])) {
          response = "ERROR Permission Denied";
          break;
        }
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
        if (!check_permission(req.args[0])) {
          response = "ERROR Permission Denied";
          break;
        }
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
      if (cred.uid != 0) {
        response = "ERROR Permission Denied";
        break;
      }
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
    log_error("Exception handling " + cmd + ": " + e.what());
    response = "ERROR Exception";
  } catch (...) {
    log_error("Unknown exception handling " + cmd);
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

  log_info("Starting LinuxCamPAM Service...");
  log_info("Loading Config: " + config_path);

  // Wipe OpenCL cache - stale kernels cause hangs after Mesa updates
  std::filesystem::path home = "/root";
  if (auto user_home = linuxcampam::getHomeDir(getuid())) {
    home = *user_home;
  }
  fs::path opencv_cache = home / ".cache" / "opencv";
  if (fs::exists(opencv_cache)) {
    try {
      fs::remove_all(opencv_cache);
      log_debug("Cleared OpenCL cache");
    } catch (const std::exception &e) {
      log_debug("OpenCL cache cleanup failed: " + std::string(e.what()));
    }
  }

  AuthEngine engine;
  // Initialize Engine
  if (!engine.init(config_path)) {
    log_error("AuthEngine init failed, shutting down.");
    return 1;
  }

  // Apply log settings from config
  const auto& cfg = engine.getConfig();
  if (cfg.log_level == "debug") {
    Logger::setLevel(LogLevel::DEBUG);
  } else if (cfg.log_level == "info") {
    Logger::setLevel(LogLevel::INFO);
  } else if (cfg.log_level == "warn" || cfg.log_level == "warning") {
    Logger::setLevel(LogLevel::WARN);
  } else if (cfg.log_level == "error") {
    Logger::setLevel(LogLevel::ERROR);
  }

  if (!cfg.log_file.empty()) {
    Logger::setLogFile(cfg.log_file);
  }

  // Enable syslog early so we capture hardware init logs
  Logger::enableSyslog("linuxcampamd");

  // Proximity Sensor Integration Lifecycle
  std::optional<HardwareManager> hw_manager;
  SensorFactory sensor_factory;
  PresenceTripwire tripwire(sensor_factory);

  if (cfg.proximity_sensor == Configuration::ProximitySensorMode::AUTO || cfg.proximity_sensor == Configuration::ProximitySensorMode::ENABLED) {
    std::string hw_id = cfg.proximity_sensor_id;
    std::string i2c_addr;
    std::error_code ec;

    std::string expected_path = "/sys/bus/acpi/devices/" + hw_id + ":00/physical_node";
    if (fs::exists(expected_path, ec)) {
      auto target = fs::read_symlink(expected_path, ec);
      if (!ec) {
        i2c_addr = target.filename().string();
      }
    }

    if (i2c_addr.empty()) {
      for (const auto& entry : fs::directory_iterator("/sys/bus/acpi/devices/", ec)) {
        if (entry.path().filename().string().find(hw_id) != std::string::npos) {
          if (auto phys = entry.path() / "physical_node"; fs::exists(phys, ec)) {
            auto target = fs::read_symlink(phys, ec);
            if (!ec) {
              i2c_addr = target.filename().string();
              break;
            }
          }
        }
      }
    }

    if (!i2c_addr.empty()) {
      hw_manager.emplace(i2c_addr);
      if (hw_manager->seize_sensor()) {
        if (std::optional<std::string> hidraw_node = hw_manager->get_hidraw_node(); hidraw_node) {
          if (!tripwire.start(*hidraw_node, HardwareId(hw_id), [](bool present, int distance) {
            static bool last_state = false;
            static int last_distance = 0;
            g_proximity_present.store(present);
            if (present != last_state) {
              if (present) {
                log_info("Presence detected start at " + std::to_string(distance) + "cm");
              } else {
                log_info("Presence detected stop (last seen at " + std::to_string(last_distance) + "cm)");
              }
              last_state = present;
            }
            if (present) {
              last_distance = distance;
            }
          })) {
            log_warn("Failed to start PresenceTripwire.");
          } else {
            log_info("Proximity sensor started successfully on " + *hidraw_node);
            // If successfully started and enforcing, initialize state
            if (cfg.proximity_enforce) {
              g_proximity_present.store(false); 
            }
          }
        } else if (cfg.proximity_sensor == Configuration::ProximitySensorMode::ENABLED) {
          log_warn("Proximity sensor set to enabled, but hidraw node could not be resolved.");
        }
      } else {
        log_warn("Failed to seize hardware sensor.");
      }
    } else if (cfg.proximity_sensor == Configuration::ProximitySensorMode::ENABLED) {
      log_warn("Proximity sensor set to enabled, but hardware node was not found.");
    }
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

  // Syslog was enabled earlier

  // World-readable socket allows console users to trigger authentication
  chmod(socket_path.c_str(), SOCKET_PERMS);

  if (listen(server_fd.get(), BACKLOG) < 0) {
    perror("listen");
    return 1;
  }

  log_info("Listening on " + socket_path);

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

  tripwire.stop();
  // hw_manager will automatically release the sensor upon destruction

  (void)unlink(socket_path.c_str());
  log_info("Stopped.");
  return 0;
}
