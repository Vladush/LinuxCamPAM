#include "constants.hpp"
#include "../common/socket_client.hpp"
#include "../common/ipc_protocol.hpp"
#include "kde_lockscreen.hpp"
#include "pam_config.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int run_check_kde(bool brief) {
#ifdef PAM_CONFIG_PATH
  const char *config_path = PAM_CONFIG_PATH;
#else
  const char *config_path = linuxcampam::CONFIG_PATH;
#endif

  if (access(config_path, F_OK) == 0 && access(config_path, R_OK) != 0) {
    std::cerr << "Error: Configuration file " << config_path << " is unreadable." << '\n';
    return 2;
  }

  PamConfig pam_config;
  try {
    pam_config = load_pam_config(config_path);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  }

  linuxcampam::kde::Scan etc_scan = linuxcampam::kde::scan_file(linuxcampam::kde::ETC_STACK);
  linuxcampam::kde::Scan vendor_scan = linuxcampam::kde::scan_file(linuxcampam::kde::VENDOR_STACK);
  linuxcampam::kde::StackInfo eff = linuxcampam::kde::scan_effective_stack();

  if (etc_scan == linuxcampam::kde::Scan::Missing && vendor_scan == linuxcampam::kde::Scan::Missing) {
    const bool alternative = access("/etc/pam.d/kscreenlocker", F_OK) == 0;
    std::cout << (brief ? "KDE lock screen: " : "KDE lock screen\n  Status: ")
              << (alternative ? "Non-kde service name (/etc/pam.d/kscreenlocker exists), not supported yet"
                              : "KDE not installed") << '\n';
    return alternative ? 1 : 0;
  }

  linuxcampam::kde::Decision dec = linuxcampam::kde::decide(pam_config, eff);

  auto scan_str = [](linuxcampam::kde::Scan s) -> const char * {
    switch (s) {
    case linuxcampam::kde::Scan::KwalletAuth: return "KWallet auth rule found";
    case linuxcampam::kde::Scan::NoKwalletAuth: return "no KWallet auth rule";
    case linuxcampam::kde::Scan::OtherKwalletAuth: return "unsupported KWallet module";
    case linuxcampam::kde::Scan::Missing: return "file not found";
    case linuxcampam::kde::Scan::Unreadable: return "file unreadable";
    case linuxcampam::kde::Scan::TooLarge: return "file too large";
    case linuxcampam::kde::Scan::Unsupported: return "unsupported PAM syntax";
    }
    return "unknown";
  };

  std::string mode_str = to_string(pam_config.kde_lockscreen);
  std::string eff_str = dec.single_enter ? "single_enter" : "legacy";
  std::string summary_mode;
  if (pam_config.kde_lockscreen == KdeLockscreenMode::Auto) {
    summary_mode = "auto -> " + eff_str + " (" + dec.reason + ")";
  } else {
    summary_mode = mode_str + " (" + dec.reason + ")";
  }

  int exit_code = 0;
  std::string hint;

  if (eff.scan == linuxcampam::kde::Scan::OtherKwalletAuth ||
      eff.scan == linuxcampam::kde::Scan::Unsupported ||
      eff.scan == linuxcampam::kde::Scan::TooLarge ||
      eff.scan == linuxcampam::kde::Scan::Unreadable) {
    hint = "Unsupported stack: " + std::string(eff.path) + " (" + scan_str(eff.scan) + ")";
    exit_code = 1;
  } else if (pam_config.kde_lockscreen_invalid) {
    hint = "Invalid kde_lockscreen value: using legacy behavior";
    exit_code = 1;
  } else if (!pam_config.require_confirmation) {
    hint = "require_confirmation = false: lock screen scans immediately";
    exit_code = 1;
  } else if (dec.single_enter && pam_config.exempt_services_explicit &&
             std::find(pam_config.confirmation_exempt_services.begin(),
                       pam_config.confirmation_exempt_services.end(),
                       "kde") != pam_config.confirmation_exempt_services.end()) {
    hint = "Explicit confirmation_exempt_services contains 'kde' while mode is single_enter. Remove 'kde' from the list.";
    exit_code = 1;
  }

  bool has_override_note = false;
  if (exit_code == 0 && pam_config.kde_lockscreen == KdeLockscreenMode::Auto &&
      etc_scan == linuxcampam::kde::Scan::NoKwalletAuth &&
      vendor_scan == linuxcampam::kde::Scan::KwalletAuth) {
    has_override_note = true;
  }

  if (brief) {
    std::cout << "kde_lockscreen: " << summary_mode << '\n';
    if (!hint.empty()) {
      std::cout << "Hint: " << hint << '\n';
    }
    if (has_override_note) {
      std::cout << "Note: local override " << linuxcampam::kde::ETC_STACK << " is no longer needed for single-Enter unlock.\n";
    }
  } else {
    std::cout << "KDE lock screen\n";
    std::cout << "  Config:          " << config_path << '\n';
    std::cout << "  kde_lockscreen:  " << summary_mode << '\n';
    std::cout << "  Confirmation:    "
              << (pam_config.require_confirmation ? "required" : "not required")
              << (pam_config.exempt_services_explicit ? " (explicit confirmation_exempt_services)" : " (default)")
              << '\n';
    std::cout << "  Empty token:     " << (dec.single_enter ? "on" : "off") << '\n';
    std::cout << "  Vendor stack:    " << linuxcampam::kde::VENDOR_STACK << " (" << scan_str(vendor_scan) << ")\n";
    if (etc_scan != linuxcampam::kde::Scan::Missing) {
      std::cout << "  Local override:  " << linuxcampam::kde::ETC_STACK << " (" << scan_str(etc_scan) << ")\n";
    }
    if (!hint.empty()) {
      std::cout << "Hint: " << hint << '\n';
    }
    if (has_override_note) {
      std::cout << "Note: the local override is no longer needed for single-Enter unlock.\n";
      std::cout << "  Compare it with the vendor file before removing it:\n";
      std::cout << "  diff -u " << linuxcampam::kde::VENDOR_STACK << " " << linuxcampam::kde::ETC_STACK << '\n';
    }
  }

  return exit_code;
}


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

std::string send_cmd(const std::string &cmd, int timeout_ms = 5000) {
  auto sock = linuxcampam::connectUnixSocket(linuxcampam::SOCKET_PATH, timeout_ms);
  if (!sock.isValid()) {
    std::cerr << "Could not connect to service at " << linuxcampam::SOCKET_PATH
              << ". Is linuxcampamd running?\n";
    return {};
  }
  if (!linuxcampam::protocol::send_message(sock.get(), cmd, timeout_ms)) return {};
  std::string response;
  if (!linuxcampam::protocol::recv_message(sock.get(), response, timeout_ms)) return {};
  return response;
}

bool may_access_user(const std::string &user) {
  if (getuid() == 0 || user == get_current_user()) return true;
  std::cerr << "Error: Accessing another user's face data requires root privileges (sudo).\n";
  return false;
}

void print_response(const std::string &resp) {
  if (!resp.empty()) {
    std::cout << "Response: " << resp << '\n';
  } else {
    std::cerr << "Error: Connection closed by service (empty response)."
              << '\n';
  }
}

void print_help() {
#ifdef LINUXCAMPAM_VERSION
  std::cout << "LinuxCamPAM CLI Tool v" << LINUXCAMPAM_VERSION << "\n";
#else
  std::cout << "LinuxCamPAM CLI Tool vUnknown\n";
#endif
  std::cout
      << "Usage:\n"
      << "  linuxcampam add <username>              Enroll a new user\n"
      << "  linuxcampam train [username] [options]  Train/refine model\n"
      << "    --label <name>                        Refine specific label\n"
      << "    --new                                 Add new embedding\n"
      << "  linuxcampam test [username]             Test camera & auth\n"
      << "  linuxcampam list <username>             Show embedding labels\n"
      << "  linuxcampam remove <user> --label <X>   Remove specific embedding\n"
      << "  linuxcampam show-config                 Show active config\n"
      << "  linuxcampam debug [on|off]              Toggle debug logging\n"
      << "  linuxcampam check-kde [--brief]         Check KDE lock screen setup\n"
      << "  linuxcampam version                     Show version info\n"
      << "  linuxcampam help                        Show this help\n";
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout
        << "Usage: linuxcampam "
           "<add|train|test|list|remove|show-config|debug|check-kde|version|help> [args]"
        << '\n';
    return 1;
  }

  std::string_view op = argv[1];

  if (op == "add") {
    if (getuid() != 0) {
      std::cerr << "Error: Adding a user requires root privileges (sudo)." << '\n';
      return 1;
    }
    if (argc < 3) {
      std::cout << "Usage: linuxcampam add <username>" << '\n';
      return 1;
    }
    std::string user = argv[2];

    // First enroll the user
    std::string resp = send_cmd("ADD_USER " + user, 60000);
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
          std::cout << "Cancelled. Embedding discarded." << '\n';
          return 0;
        }
      }

      std::string label_resp = send_cmd("SET_LABEL " + user + " " + label);
      if (!label_resp.empty() &&
          label_resp.find("ERROR") == std::string::npos) {
        std::cout << "Embedding saved with label: " << label << '\n';
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
                  << '\n';
        return 1;
      }
    }

    if (!may_access_user(user)) return 1;

    if (is_new) {
      // Prompt for new label
      std::cout << "New label: ";
      std::getline(std::cin, label);
      if (label.empty()) {
        label = "trained_" + std::to_string(std::time(nullptr));
      }
      print_response(send_cmd("TRAIN_NEW " + user + " " + label, 60000));
    } else {
      if (label.empty()) {
        label = "default";
      }
      print_response(send_cmd("TRAIN_USER " + user + " " + label, 60000));
    }

  } else if (op == "test") {
    std::string current_user = get_current_user();
    std::string user;

    if (argc >= 3) {
      user = argv[2];
      // Security: require sudo to test other users
      if (!may_access_user(user)) return 1;
    } else {
      user = current_user;
    }

    if (!user.empty()) {
      print_response(send_cmd("TEST_AUTH " + user, linuxcampam::MAX_AUTH_TIMEOUT_MS + linuxcampam::AUTH_IPC_MARGIN_MS));
    } else {
      print_response(send_cmd("TEST_AUTH", 60000));
    }

  } else if (op == "list") {
    if (argc < 3) {
      std::cout << "Usage: linuxcampam list <username>" << '\n';
      return 1;
    }
    std::string user = argv[2];
    if (!may_access_user(user)) return 1;
    print_response(send_cmd("LIST_EMBEDDINGS " + user));

  } else if (op == "remove") {
    if (argc < 4) {
      std::cout << "Usage: linuxcampam remove <username> --label <label>"
                << '\n';
      return 1;
    }
    std::string user = argv[2];
    if (!may_access_user(user)) return 1;
    std::string label;
    for (int i = 3; i < argc; i++) {
      if (std::string(argv[i]) == "--label" && i + 1 < argc) {
        label = argv[++i];
        break;
      }
    }
    if (label.empty()) {
      std::cout << "Error: --label is required" << '\n';
      return 1;
    }
    print_response(send_cmd("REMOVE_EMBEDDING " + user + " " + label));

  } else if (op == "show-config") {
    std::string cfg = send_cmd("GET_CONFIG");
    if (!cfg.empty()) {
      std::cout << cfg << '\n';
    } else {
      std::cerr << "Error: Could not get config from service." << '\n';
    }

  } else if (op == "debug") {
    if (argc < 3) {
      // No arg: get current level
      std::string lvl = send_cmd("GET_LOG_LEVEL");
      if (!lvl.empty()) {
        std::cout << "Current Log Level: " << lvl << '\n';
      } else {
        std::cerr << "Error: Could not query log level." << '\n';
      }
    } else {
      std::string arg = argv[2];
      if (arg == "on") {
        print_response(send_cmd("SET_LOG_LEVEL DEBUG"));
      } else if (arg == "off") {
        print_response(send_cmd("SET_LOG_LEVEL INFO"));
      } else {
        std::cout << "Usage: linuxcampam debug [on|off]" << '\n';
      }
    }

  } else if (op == "version" || op == "--version" || op == "-v") {
#ifdef LINUXCAMPAM_VERSION
    std::cout << "Client Version: " << LINUXCAMPAM_VERSION << '\n';
#else
    std::cout << "Client Version: Unknown" << '\n';
#endif
    std::string daemon_ver = send_cmd("GET_VERSION");
    if (daemon_ver.empty()) {
      std::cout << "Daemon Version: Not running or unreachable" << '\n';
    } else {
      std::cout << "Daemon Version: " << daemon_ver << '\n';
    }

  } else if (op == "check-kde") {
    bool brief = argc > 2 && std::string_view{argv[2]} == "--brief";
    return run_check_kde(brief);

  } else if (op == "help" || op == "--help" || op == "-h") {
    print_help();
  } else {
    std::cout << "Unknown command. Try 'linuxcampam help'." << '\n';
    return 1;
  }

  return 0;
}
