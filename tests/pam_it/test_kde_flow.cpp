#include <gtest/gtest.h>
#include <security/pam_appl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pwd.h>
#include <thread>
#include <atomic>
#include <fstream>
#include <string>
#include <vector>

#ifndef PAM_IT_DIR
#error "PAM_IT_DIR must be defined"
#endif

// We define our own start_confdir for Linux-PAM 1.4+
extern "C" int pam_start_confdir(const char *service_name, const char *user,
                                 const struct pam_conv *pam_conversation,
                                 const char *confdir, pam_handle_t **pamh);

struct ScriptedConversation {
  std::vector<std::string> responses;
  size_t index = 0;
  int prompts = 0;

  static int conv(int num_msg, const struct pam_message **msg,
                  struct pam_response **resp, void *appdata_ptr) {
    auto *self = static_cast<ScriptedConversation *>(appdata_ptr);
    auto *responses = static_cast<struct pam_response *>(calloc(num_msg, sizeof(struct pam_response))); // NOLINT
    for (int i = 0; i < num_msg; ++i) {
      if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
        self->prompts++;
        if (self->index < self->responses.size()) {
          const auto &r = self->responses[self->index++];
          if (r == "ABORT") {
            free(responses); // NOLINT
            return PAM_CONV_ERR;
          }
          responses[i].resp = strdup(r.c_str());
        }
      }
    }
    *resp = responses;
    return PAM_SUCCESS;
  }
};

class FakeDaemon {
public:
  FakeDaemon() : sock(socket(AF_UNIX, SOCK_STREAM, 0)), running(false) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = std::string(PAM_IT_DIR) + "/socket";
    strncpy(static_cast<char*>(addr.sun_path), path.c_str(), sizeof(addr.sun_path) - 1);
    if (unlink(static_cast<const char*>(addr.sun_path)) < 0) {}
    if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {} // NOLINT
    constexpr int max_backlog = 5;
    if (listen(sock, max_backlog) < 0) {}
    running = true;
    t = std::thread(&FakeDaemon::run, this);
  }

  ~FakeDaemon() {
    running = false;
    shutdown(sock, SHUT_RDWR);
    close(sock);
    if (t.joinable()) t.join();
    std::string path = std::string(PAM_IT_DIR) + "/socket";
    if (unlink(path.c_str()) < 0) {}
  }

  FakeDaemon(const FakeDaemon&) = delete;
  FakeDaemon& operator=(const FakeDaemon&) = delete;
  FakeDaemon(FakeDaemon&&) = delete;
  FakeDaemon& operator=(FakeDaemon&&) = delete;

  void set_response(const std::string &r) { response = r; }
  int get_requests() const { return requests; }

private:
  void run() {
    while (running) {
      int client = accept(sock, nullptr, nullptr);
      if (client >= 0) {
        requests++;
        constexpr int buf_size = 128;
        std::array<char, buf_size> buf{};
        if (read(client, buf.data(), buf.size()) < 0) {}
        if (write(client, response.c_str(), response.length()) < 0) {}
        close(client);
      }
    }
  }

  int sock;
  std::thread t;
  std::atomic<bool> running;
  std::string response = "AUTH_SUCCESS";
  std::atomic<int> requests{0};
};

#include <filesystem>

class PamIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    std::filesystem::create_directories(std::string(PAM_IT_DIR) + "/etc");
    // Write fake stack to pam_it/etc/kde
    std::ofstream etc(std::string(PAM_IT_DIR) + "/etc/kde");
    etc << "auth [success=2 default=ignore] " << PAM_IT_DIR << "/modules/pam_linuxcampam_it.so no_welcome\n";
    etc << "auth [success=1 default=ignore] " << PAM_IT_DIR << "/modules/pam_fake_unix.so\n";
    etc << "auth requisite pam_deny.so\n";
    etc << "auth required  pam_permit.so\n";
    etc << "auth optional  " << PAM_IT_DIR << "/modules/pam_kwallet5.so\n";
    etc.close();
  }

  void write_config(const std::string &kde_lockscreen, const std::string &extra = "") {
    std::ofstream cfg(std::string(PAM_IT_DIR) + "/config.ini");
    cfg << "[Security]\nkde_lockscreen=" << kde_lockscreen << "\n" << extra;
  }

  void remove_kwallet_rule() {
    std::ofstream etc(std::string(PAM_IT_DIR) + "/etc/kde");
    etc << "auth [success=2 default=ignore] " << PAM_IT_DIR << "/modules/pam_linuxcampam_it.so no_welcome\n";
    etc << "auth [success=1 default=ignore] " << PAM_IT_DIR << "/modules/pam_fake_unix.so\n";
    etc << "auth requisite pam_deny.so\n";
    etc << "auth required  pam_permit.so\n";
  }

  int run_pam(ScriptedConversation &conv_data, const char *service = "kde") {
    pam_handle_t *pamh = nullptr;
    struct pam_conv conv = {ScriptedConversation::conv, &conv_data};
    std::string confdir = std::string(PAM_IT_DIR) + "/etc";
    const char *user = "testuser";
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      user = pw->pw_name;
    }
    
    if (pam_start_confdir(service, user, &conv, confdir.c_str(), &pamh) != PAM_SUCCESS) {
      return -1;
    }
    int ret = pam_authenticate(pamh, 0);
    const char *env = pam_getenv(pamh, "FAKE_KWALLET");
    last_kwallet_env = env ? env : "";
    pam_end(pamh, ret);
    return ret;
  }
  public:
  std::string last_kwallet_env;
};

TEST_F(PamIntegrationTest, AutoSingleEnterFaceSuccess) {
  write_config("auto");
  FakeDaemon d;
  ScriptedConversation c{{"", "secret"}}; // first prompt gets empty
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(c.prompts, 1);
  EXPECT_EQ(last_kwallet_env, "empty");
}

TEST_F(PamIntegrationTest, LegacyReproducesSecondPrompt) {
  write_config("legacy");
  FakeDaemon d;
  ScriptedConversation c{{"", "secret"}};
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(c.prompts, 1); // second prompt triggered by kwallet
  EXPECT_EQ(last_kwallet_env, "prompted");
}

TEST_F(PamIntegrationTest, AutoWithoutKwalletRuleIsLegacy) {
  write_config("auto");
  remove_kwallet_rule();
  FakeDaemon d;
  ScriptedConversation c{{"", "secret"}};
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(last_kwallet_env, ""); // No kwallet rule
  EXPECT_EQ(c.prompts, 0);

}

TEST_F(PamIntegrationTest, TypedPassword) {
  write_config("single_enter");
  FakeDaemon d;
  ScriptedConversation c{{"secret"}};
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(d.get_requests(), 0); // never queried daemon because we provided non-empty password
  EXPECT_EQ(last_kwallet_env, "password");
}

TEST_F(PamIntegrationTest, WrongPassword) {
  write_config("single_enter");
  FakeDaemon d;
  ScriptedConversation c{{"wrong"}};
  EXPECT_EQ(run_pam(c), PAM_AUTH_ERR);
  EXPECT_EQ(d.get_requests(), 0);
}

TEST_F(PamIntegrationTest, FaceFailFallsBackToPassword) {
  write_config("single_enter");
  FakeDaemon d;
  d.set_response("AUTH_FAILED");
  ScriptedConversation c{{"", "secret"}}; // first is empty to trigger face auth, it fails, so it falls back to unix prompt
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(last_kwallet_env, "password");
}

TEST_F(PamIntegrationTest, CancelledConfirmation) {
  write_config("single_enter");
  FakeDaemon d;
  ScriptedConversation c{{"ABORT"}};
  EXPECT_NE(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(d.get_requests(), 0);
}

TEST_F(PamIntegrationTest, NonKdeServiceUntouched) {
  write_config("single_enter");
  FakeDaemon d;
  // Write a sudo config for test
  std::ofstream etc(std::string(PAM_IT_DIR) + "/etc/sudo");
  etc << "auth [success=2 default=ignore] " << PAM_IT_DIR << "/modules/pam_linuxcampam_it.so no_welcome\n";
  etc << "auth [success=1 default=ignore] " << PAM_IT_DIR << "/modules/pam_fake_unix.so\n";
  etc << "auth requisite pam_deny.so\n";
  etc << "auth required  pam_permit.so\n";
  etc << "auth optional  " << PAM_IT_DIR << "/modules/pam_kwallet5.so\n";
  etc.close();

  // sudo is NOT exempt by default
  ScriptedConversation c{{"", "secret"}};
  EXPECT_EQ(run_pam(c, "sudo"), PAM_SUCCESS);
  // because single_enter only affects kde, sudo still behaves as legacy: face success but no empty token for kwallet
  EXPECT_EQ(last_kwallet_env, "prompted");
}

TEST_F(PamIntegrationTest, ExplicitListWithKde) {
  write_config("single_enter", "confirmation_exempt_services=kde\n");
  FakeDaemon d;
  // No confirmation prompt, daemon scanned immediately
  ScriptedConversation c;
  EXPECT_EQ(run_pam(c), PAM_SUCCESS);
  EXPECT_EQ(last_kwallet_env, "empty");
}
