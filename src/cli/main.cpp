#include "constants.hpp"

#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

std::string get_current_user() {
  const char *sudo_user = std::getenv("SUDO_USER");
  if (sudo_user && strlen(sudo_user) > 0) {
    return sudo_user;
  }
  const char *user = std::getenv("USER");
  if (user && strlen(user) > 0) {
    return user;
  }
  return "";
}

std::string send_cmd(const std::string &cmd) {
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    std::cerr << "Error creating socket." << std::endl;
    return "";
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, linuxcampam::SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    std::cerr << "Could not connect to service at " << linuxcampam::SOCKET_PATH
              << ". Is linuxcampamd running?" << std::endl;
    close(sock);
    return "";
  }

  send(sock, cmd.c_str(), cmd.length(), 0);

  char buffer[4096] = {0};
  ssize_t bytes_read = read(sock, buffer, sizeof(buffer) - 1);
  close(sock);

  if (bytes_read > 0) {
    return std::string(buffer);
  }
  return "";
}

void print_response(const std::string &resp) {
  if (!resp.empty()) {
    std::cout << "Response: " << resp << std::endl;
  } else {
    std::cerr << "Error: Connection closed by service (empty response)."
              << std::endl;
  }
}

void print_help() {
  std::cout
#ifdef LINUXCAMPAM_VERSION
          std::cout
      << "LinuxCamPAM CLI Tool v" << LINUXCAMPAM_VERSION << "\n"
#else
          std::cout
      << "LinuxCamPAM CLI Tool vUnknown\n"
#endif
      << "Usage:\n"
      << "  linuxcampam add <username>              Enroll a new user\n"
      << "  linuxcampam train [username] [options]  Train/refine model\n"
      << "    --label <name>                        Refine specific label\n"
      << "    --new                                 Add new embedding\n"
      << "  linuxcampam test [username]             Test camera & auth\n"
      << "  linuxcampam list <username>             Show embedding labels\n"
      << "  linuxcampam remove <user> --label <X>   Remove specific embedding\n"
      << "  linuxcampam help                        Show this help\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout << "Usage: linuxcampam <add|train|test|list|remove|help> [args]"
              << std::endl;
    return 1;
  }

  std::string op = argv[1];

  if (op == "add") {
    if (argc < 3) {
      std::cout << "Usage: linuxcampam add <username>" << std::endl;
      return 1;
    }
    std::string user = argv[2];

    // First enroll the user
    std::string resp = send_cmd("ADD_USER " + user);
    print_response(resp);

    // If successful, prompt for label
    if (resp.find("ENROLL_SUCCESS") != std::string::npos) {
      // Get existing labels
      std::string existing = send_cmd("LIST_EMBEDDINGS " + user);

      std::cout << "Label (default): ";
      std::string label;
      std::getline(std::cin, label);
      if (label.empty()) {
        label = "default";
      }

      // Check if label exists and confirm overwrite
      if (existing.find(label) != std::string::npos) {
        std::cout << "Label '" << label
                  << "' already exists. Overwrite? [y/N]: ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") {
          std::cout << "Cancelled. Embedding discarded." << std::endl;
          return 0;
        }
      }

      std::string label_resp = send_cmd("SET_LABEL " + user + " " + label);
      if (!label_resp.empty() &&
          label_resp.find("ERROR") == std::string::npos) {
        std::cout << "Embedding saved with label: " << label << std::endl;
      }
    }

  } else if (op == "train") {
    std::string user;
    std::string label;
    bool is_new = false;

    // Parse arguments
    for (int i = 2; i < argc; i++) {
      std::string arg = argv[i];
      if (arg == "--label" && i + 1 < argc) {
        label = argv[++i];
      } else if (arg == "--new") {
        is_new = true;
      } else if (arg[0] != '-') {
        user = arg;
      }
    }

    // Default to current user if not specified
    if (user.empty()) {
      user = get_current_user();
      if (user.empty()) {
        std::cerr << "Could not determine username. Please specify explicitly."
                  << std::endl;
        return 1;
      }
    }

    if (is_new) {
      // Prompt for new label
      std::cout << "New label: ";
      std::getline(std::cin, label);
      if (label.empty()) {
        label = "trained_" + std::to_string(std::time(nullptr));
      }
      print_response(send_cmd("TRAIN_NEW " + user + " " + label));
    } else {
      if (label.empty()) {
        label = "default";
      }
      print_response(send_cmd("TRAIN_USER " + user + " " + label));
    }

  } else if (op == "test") {
    std::string current_user = get_current_user();
    std::string user;

    if (argc >= 3) {
      user = argv[2];
      // Security: require sudo to test other users
      if (user != current_user && getuid() != 0) {
        std::cerr << "Error: Testing other users requires sudo." << std::endl;
        return 1;
      }
    } else {
      user = current_user;
    }

    if (!user.empty()) {
      print_response(send_cmd("TEST_AUTH " + user));
    } else {
      print_response(send_cmd("TEST_AUTH"));
    }

  } else if (op == "list") {
    if (argc < 3) {
      std::cout << "Usage: linuxcampam list <username>" << std::endl;
      return 1;
    }
    print_response(send_cmd("LIST_EMBEDDINGS " + std::string(argv[2])));

  } else if (op == "remove") {
    if (argc < 4) {
      std::cout << "Usage: linuxcampam remove <username> --label <label>"
                << std::endl;
      return 1;
    }
    std::string user = argv[2];
    std::string label;
    for (int i = 3; i < argc; i++) {
      if (std::string(argv[i]) == "--label" && i + 1 < argc) {
        label = argv[++i];
        break;
      }
    }
    if (label.empty()) {
      std::cout << "Error: --label is required" << std::endl;
      return 1;
    }
    print_response(send_cmd("REMOVE_EMBEDDING " + user + " " + label));

  } else if (op == "version" || op == "--version" || op == "-v") {
#ifdef LINUXCAMPAM_VERSION
    std::cout << "Client Version: " << LINUXCAMPAM_VERSION << std::endl;
#else
    std::cout << "Client Version: Unknown" << std::endl;
#endif
    std::string daemon_ver = send_cmd("GET_VERSION");
    if (daemon_ver.empty()) {
      std::cout << "Daemon Version: Not running or unreachable" << std::endl;
    } else {
      std::cout << "Daemon Version: " << daemon_ver << std::endl;
    }

  } else if (op == "help" || op == "--help" || op == "-h") {
    print_help();
  } else {
    std::cout << "Unknown command. Try 'linuxcampam help'." << std::endl;
    return 1;
  }

  return 0;
}
