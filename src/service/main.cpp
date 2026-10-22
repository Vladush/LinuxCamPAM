
#include "auth_engine.hpp"
#include "deadline_worker.hpp"
#include "presence_actions.hpp"
#include "constants.hpp"
#include "ipc_protocol.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "HardwareManager.hpp"
#include "SensorFactory.hpp"
#include "PresenceTripwire.hpp"
#include "VirtualKeyboard.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <sys/prctl.h>
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

constexpr int SOCKET_PERMS = 0666; // File permissions for the UNIX socket (world-readable/writable)
constexpr int BACKLOG = 5; // Maximum length of the pending connections queue for the socket
constexpr int kClientIpcTimeoutMs = 2000;
constexpr int kWorkerPollIntervalMs = 1000;
} // namespace

void signal_handler(int signum) {
  log_info("Received signal " + std::to_string(signum) + ", stopping...");
  g_running = false;
}

void handle_client(int fd, AuthEngine &engine) {
  linuxcampam::FileDescriptor client_fd(fd);
  std::string request;
  if (!linuxcampam::protocol::recv_message(client_fd.get(), request, kClientIpcTimeoutMs)) {
    return;
  }

  log_debug("Received Request: " + request);

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
      if (!check_permission(req.args.front())) {
        // Enforce SO_PEERCRED validation. Unprivileged cross-user requests are dropped
        // to prevent local DoS attacks from tying up the camera hardware.
        response = "AUTH_FAIL";
        break;
      }

      if (engine.getConfig().proximity_enforce && !g_proximity_present.load()) {
        response = "AUTH_FAIL: Proximity sensor reports no human present";
        log_warn("Authentication rejected by proximity sensor enforcement.");
        break;
      }

      bool success = engine.verifyUser(req.args.front());
      response = success ? "AUTH_SUCCESS" : "AUTH_FAIL";
      break;
    }
    case Command::ADD_USER: {
      if (req.args.empty())
        break;
      if (!check_permission(req.args.front())) {
        response = "ERROR Permission Denied";
        break;
      }
      auto Result = engine.enrollUser(req.args.front());
      response =
          Result.first ? "ENROLL_SUCCESS" : ("ENROLL_FAIL " + Result.second);
      break;
    }
    case Command::TRAIN_USER: {
      if (req.args.empty())
        break;
      if (!check_permission(req.args.front())) {
        response = "ERROR Permission Denied";
        break;
      }
      std::string label = (req.args.size() > 1) ? req.args.at(1) : "default";
      bool success = engine.trainUser(req.args.front(), label, false);
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
      std::string user = req.args.empty() ? "" : req.args.front();
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
        if (!check_permission(req.args.front())) {
          response = "ERROR Permission Denied";
          break;
        }
        bool success = engine.setLabel(req.args.front(), req.args.at(1));
        response = success ? "LABEL_SET" : "LABEL_FAIL";
      }
      break;
    }
    case Command::TRAIN_NEW: {
      if (req.args.empty()) {
        response = "ERROR Missing user";
      } else {
        if (!check_permission(req.args.front())) {
          response = "ERROR Permission Denied";
          break;
        }
        std::string label = (req.args.size() > 1) ? req.args.at(1) : "default";
        bool success = engine.trainUser(req.args.front(), label, true);
        response = success ? "TRAIN_SUCCESS" : "TRAIN_FAIL";
      }
      break;
    }

    case Command::LIST_EMBEDDINGS: {
      if (req.args.empty()) {
        response = "ERROR Missing user";
      } else {
        if (!check_permission(req.args.front())) {
          response = "ERROR Permission Denied";
          break;
        }
        auto labels = engine.listEmbeddings(req.args.front());
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
        if (!check_permission(req.args.front())) {
          response = "ERROR Permission Denied";
          break;
        }
        bool success = engine.removeEmbedding(req.args.front(), req.args.at(1));
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
      std::string levelStr = req.args.front();
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

  linuxcampam::protocol::send_message(client_fd.get(), response, kClientIpcTimeoutMs);
}

static void configure_logging(const Configuration &config) {
  if (config.log_level == "debug") {
    Logger::setLevel(LogLevel::DEBUG);
  } else if (config.log_level == "info") {
    Logger::setLevel(LogLevel::INFO);
  } else if (config.log_level == "warn" || config.log_level == "warning") {
    Logger::setLevel(LogLevel::WARN);
  } else if (config.log_level == "error") {
    Logger::setLevel(LogLevel::ERROR);
  }

  if (!config.log_file.empty()) {
    Logger::setLogFile(config.log_file);
  }

  // Syslog is enabled early to capture hardware initialization logs
  Logger::enableSyslog("linuxcampamd");
}

// Private worker entry point. Exec, rather than fork-and-run, avoids inherited
// OpenCV/HID thread locks. The supervisor owns the authentication deadline.
static int verification_worker(const char *config_path, std::string_view supervisor) {
  if (std::to_string(getppid()) != supervisor || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
      std::to_string(getppid()) != supervisor) return 1;
  if (fcntl(DeadlineWorker::CHANNEL_FD, F_SETFD, FD_CLOEXEC) < 0) return 1;
  AuthEngine engine;
  if (!engine.init(config_path, false)) return 1;
  configure_logging(engine.getConfig());
  for (;;) {
    pollfd pfd{DeadlineWorker::CHANNEL_FD, POLLIN, 0};
    int ready = poll(&pfd, 1, kWorkerPollIntervalMs);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) return 1;
    if (ready == 0) { (void)engine.performMaintenance(); continue; }
    std::string request;
    if (!linuxcampam::protocol::recv_message(DeadlineWorker::CHANNEL_FD, request, kClientIpcTimeoutMs)) return 0;
    try {
      auto data = json::parse(request);
      const auto user = data.at("user").get<std::string>();
      const bool diagnostic = data.at("diagnostic").get<bool>();
      Logger::setLevel(static_cast<LogLevel>(data.at("log_level").get<int>()));
      AuthResult result;
      // The supervisor also tracks failures, so a restart cannot reset lockout.
      if (diagnostic) result = engine.verifyUserWithDetails(user);
      else { result.success = engine.verifyUser(user); }
      json response = {{"success", result.success}, {"reason", result.reason}, {"score", result.best_score}};
      if (!linuxcampam::protocol::send_message(DeadlineWorker::CHANNEL_FD, response.dump(), kClientIpcTimeoutMs)) return 0;
    } catch (const std::exception &error) {
      log_error(std::string("Verification worker failed: ") + error.what());
      return 1;
    }
  }
}

int main(int argc, const char *const argv[]) {
  if (argc == 4 && std::string_view(argv[1]) == "--verification-worker")
    return verification_worker(argv[2], argv[3]);
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

  // Ensure run directory exists.
  std::string socket_path = linuxcampam::SOCKET_PATH;
  // Execution environments without root privileges might require a local temporary directory.
  // System service usage with elevated privileges is assumed.
  // Directory for socket is created if needed.
  fs::path p(socket_path);
  if (p.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
      // Abort early. Uncaught exceptions here trigger std::terminate.
      log_error("Failed to create socket directory '" +
                p.parent_path().string() + "': " + ec.message());
      return 1;
    }
  }

  // Config path
  std::string config_path = linuxcampam::CONFIG_PATH;
  // Fallback to local config for dev
  if (!fs::exists(config_path)) {
    config_path = "config.ini";
  }

  log_info("Starting LinuxCamPAM Service...");
  log_info("Loading Config: " + config_path);

  // OpenCL cache is wiped to prevent system hangs caused by stale kernels following Mesa updates.
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

  DeadlineWorker verification({fs::read_symlink("/proc/self/exe").string(),
                               "--verification-worker", fs::absolute(config_path).string(), std::to_string(getpid())});
  AuthEngine engine;
  // Initialize Engine
  if (!engine.init(config_path, false)) {
    log_error("AuthEngine init failed, shutting down.");
    return 1;
  }

  // Apply log settings from config
  const auto& cfg = engine.getConfig();
  int active_client = -1;
  engine.setVerificationRunner([&](std::string_view user, bool diagnostic) -> AuthResult {
    json request = {{"user", user}, {"diagnostic", diagnostic},
                    {"log_level", static_cast<int>(Logger::getLevel())}};
    auto response = verification.request(request.dump(), std::chrono::milliseconds(cfg.timeout_ms), active_client);
    if (!response) return {false, "Verification timeout or worker failure", 0.0f};
    auto result = json::parse(*response);
    // Recheck enforcement: a sensor failure during capture must revoke success too.
    if (cfg.proximity_enforce && !g_proximity_present.load())
      return {false, "Proximity sensor reports no human present", 0.0f};
    return {result.at("success").get<bool>(), result.at("reason").get<std::string>(),
            result.at("score").get<float>()};
  });
  configure_logging(cfg);

  // Fail-secure: if proximity is enforced, do not assume presence before sensor initializes
  if (cfg.proximity_enforce) {
    g_proximity_present.store(false);
  }

  // Proximity Sensor Integration Lifecycle
  // Declare VirtualKeyboard first so it outlives tripwire.
  // This ensures the polling thread is fully joined before the fd is closed.
  VirtualKeyboard vkb;
  PresenceActions presence_actions(cfg, [&] {
    if (!vkb.emit_wakeup()) log_warn("Failed to emit wake event via VirtualKeyboard");
  }, [&] {
    log_info("Walk-away deadline reached, locking screen...");
    const int status = linuxcampam::execute_command_spawn(cfg.lock_command);
    if (status != 0) log_warn("Lock command failed: " + std::to_string(status));
  });
  std::optional<HardwareManager> hw_manager;
  SensorFactory sensor_factory;
  PresenceTripwire tripwire(sensor_factory);
  ScopedWorker proximity_init_worker;

  if (cfg.proximity_sensor == Configuration::ProximitySensorMode::AUTO || cfg.proximity_sensor == Configuration::ProximitySensorMode::ENABLED) {
    proximity_init_worker = ScopedWorker([&cfg, &hw_manager, &tripwire, &presence_actions]() {
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
            if (!tripwire.start(*hidraw_node, HardwareId(hw_id), [&presence_actions](bool present, int confidence) {
              g_proximity_present.store(present);
              presence_actions.observe(present, confidence);
            })) {
              log_warn("Failed to start PresenceTripwire.");
            } else {
              log_info("Proximity sensor started successfully on " + *hidraw_node);
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
    });
  }

  // Socket Setup
  linuxcampam::FileDescriptor server_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
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

    if (g_running && activity > 0 && FD_ISSET(server_fd.get(), &readfds)) {
      int new_socket = 0;
      int addrlen = sizeof(address);
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      new_socket =
          accept4(server_fd.get(), reinterpret_cast<struct sockaddr *>(&address),
                  reinterpret_cast<socklen_t *>(&addrlen), SOCK_CLOEXEC);
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      if (new_socket >= 0) {
        // Blocking execution is utilized; the camera enforces single-access exclusively.
        active_client = new_socket;
        handle_client(new_socket, engine);
        active_client = -1;
      }
    }

    // Always evaluate maintenance and walk-away lock timeout regardless of socket activity
    (void)engine.performMaintenance();

  }

  proximity_init_worker.join();
  tripwire.stop();
  // hw_manager will automatically release the sensor upon destruction

  (void)unlink(socket_path.c_str());
  log_info("Stopped.");
  return 0;
}
