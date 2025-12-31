#include "auth_engine.hpp"
#include "constants.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Global shutdown flag
std::atomic<bool> g_running(true);

void signal_handler(int signum) {
  Logger::log(LogLevel::INFO,
              "Received signal " + std::to_string(signum) + ", stopping...");
  g_running = false;
}

struct Config {
  std::string socket_path = linuxcampam::SOCKET_PATH;
  // Other config items loaded by AuthEngine directly or passed here
};

void handle_client(int client_fd, AuthEngine &engine) {
  char buffer[1024] = {0};
  ssize_t valread = read(client_fd, buffer, 1024);
  if (valread <= 0) {
    close(client_fd);
    return;
  }

  std::string request(buffer);
  Logger::log(LogLevel::DEBUG, "Received Request: " + request);

  // Protocol: COMMAND argument
  // e.g. "AUTH_REQUEST vlad"
  //      "ADD_USER vlad"
  //      "TRAIN_USER vlad"
  //      "TEST_AUTH"

  std::string response = "ERROR Unknown Command";

  std::istringstream iss(request);
  std::string cmd;
  iss >> cmd;

  try {
    if (cmd == "AUTH_REQUEST") {
      std::string user;
      iss >> user;
      bool success = engine.verifyUser(user);
      response = success ? "AUTH_SUCCESS" : "AUTH_FAIL";
    } else if (cmd == "ADD_USER") {
      std::string user;
      iss >> user;
      auto Result = engine.enrollUser(user);
      if (Result.first) {
        response = "ENROLL_SUCCESS";
      } else {
        response = "ENROLL_FAIL " + Result.second;
      }
    } else if (cmd == "TRAIN_USER") {
      std::string user, label;
      iss >> user >> label;
      if (label.empty())
        label = "default";
      bool success = engine.trainUser(user, label, false);
      response = success ? "TRAIN_SUCCESS" : "TRAIN_FAIL";
    } else if (cmd == "GET_VERSION") {
#ifdef LINUXCAMPAM_VERSION
      response = LINUXCAMPAM_VERSION;
#else
      response = "Unknown";
#endif
    } else if (cmd == "TEST_AUTH") {
      std::string user;
      iss >> user;

      if (!user.empty()) {
        // User verification test with detailed results
        Logger::log(LogLevel::INFO, "Testing Auth for user: " + user);
        AuthResult result = engine.verifyUserWithDetails(user);
        std::string hw_status = "HW_OK"; // Auth capture confirms HW works
        std::string auth_status =
            result.success ? "AUTH_SUCCESS" : ("AUTH_FAIL: " + result.reason);
        response = hw_status + " | " + auth_status;
      } else {
        // Hardware test only (no auth)
        bool hw_success = engine.testCameraAndAuth();
        response = hw_success ? "HW_OK" : "HW_FAIL";
      }
    } else if (cmd == "SET_LABEL") {
      std::string user, label;
      iss >> user >> label;
      if (user.empty() || label.empty()) {
        response = "ERROR Missing user or label";
      } else {
        bool success = engine.setLabel(user, label);
        response = success ? "LABEL_SET" : "LABEL_FAIL";
      }
    } else if (cmd == "TRAIN_NEW") {
      std::string user, label;
      iss >> user >> label;
      if (user.empty()) {
        response = "ERROR Missing user";
      } else {
        bool success = engine.trainUser(user, label, true);
        response = success ? "TRAIN_SUCCESS" : "TRAIN_FAIL";
      }
    } else if (cmd == "LIST_EMBEDDINGS") {
      std::string user;
      iss >> user;
      if (user.empty()) {
        response = "ERROR Missing user";
      } else {
        auto labels = engine.listEmbeddings(user);
        if (labels.empty()) {
          response = "No embeddings found";
        } else {
          response = "Labels:";
          for (const auto &l : labels) {
            response += " " + l;
          }
        }
      }
    } else if (cmd == "REMOVE_EMBEDDING") {
      std::string user, label;
      iss >> user >> label;
      if (user.empty() || label.empty()) {
        response = "ERROR Missing user or label";
      } else {
        bool success = engine.removeEmbedding(user, label);
        response = success ? "REMOVED" : "REMOVE_FAIL";
      }
    }
  } catch (const std::exception &e) {
    Logger::log(LogLevel::ERROR, "Exception handling " + cmd + ": " + e.what());
    response = "ERROR Exception";
  } catch (...) {
    Logger::log(LogLevel::ERROR, "Unknown exception handling " + cmd);
    response = "ERROR Unknown Exception";
  }

  send(client_fd, response.c_str(), response.length(), 0);
  close(client_fd);
}

int main(int argc, char *argv[]) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

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

  AuthEngine engine;
  // Initialize Engine
  if (!engine.init(config_path)) {
    std::cerr << "Failed to initialize AuthEngine. Exiting." << std::endl;
    return 1;
  }

  // Socket Setup
  int server_fd;
  struct sockaddr_un address;

  if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    return 1;
  }

  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);

  unlink(socket_path.c_str()); // Remove old socket
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    return 1;
  }

  // World-readable socket (0666) allows console users to trigger authentication
  chmod(socket_path.c_str(), 0666);

  if (listen(server_fd, 5) < 0) {
    perror("listen");
    return 1;
  }

  Logger::log(LogLevel::INFO, "Listening on " + socket_path);

  while (g_running) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);

    // Timeout for select to allow checking g_running
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int activity = select(server_fd + 1, &readfds, NULL, NULL, &timeout);

    if ((activity < 0) && (errno != EINTR)) {
      // Error
    } else if (activity == 0) {
      // Timeout: perform maintenance
      (void)engine.performMaintenance();
    }

    if (g_running && activity > 0 && FD_ISSET(server_fd, &readfds)) {
      int new_socket;
      int addrlen = sizeof(address);
      if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                               (socklen_t *)&addrlen)) >= 0) {
        // Handle in thread or blocking? Blocking for now - camera is
        // single-access anyway
        handle_client(new_socket, engine);
      }
    }
  }

  close(server_fd);
  unlink(socket_path.c_str());
  Logger::log(LogLevel::INFO, "Stopped.");
  return 0;
}
