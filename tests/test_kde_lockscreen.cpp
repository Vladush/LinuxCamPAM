#include "pam/kde_lockscreen.hpp"
#include <gtest/gtest.h>

namespace kde = linuxcampam::kde;

TEST(KdeStack, VendorKubuntuStackHasKwalletAuth) {
  constexpr std::string_view stack = R"(
auth    requisite       pam_nologin.so
auth    required        pam_succeed_if.so user != root quiet_success
@include common-auth
auth    optional        pam_kwallet5.so
@include common-account
session optional        pam_kwallet5.so auto_start
)";
  EXPECT_EQ(kde::scan_text(stack), kde::Scan::KwalletAuth);
}

TEST(KdeStack, Plasma6StackHasKwalletAuth) {
  EXPECT_EQ(kde::scan_text("auth optional pam_kwallet6.so\n"), kde::Scan::KwalletAuth);
}

TEST(KdeStack, SessionRuleAloneDoesNotCount) {
  EXPECT_EQ(kde::scan_text("session optional pam_kwallet5.so auto_start\n"),
            kde::Scan::NoKwalletAuth);
}

TEST(KdeDecision, ExplicitListKeepsKdeExempt) {
  PamConfig config;
  config.exempt_services_explicit = true;
  kde::Decision single{true, "test"};
  EXPECT_TRUE(kde::is_confirmation_exempt(config, "kde", single));
}

TEST(KdeConfig, KdeLockscreenParsesAllValues) {
  EXPECT_EQ(parse_kde_lockscreen("auto"), KdeLockscreenMode::Auto);
  EXPECT_EQ(parse_kde_lockscreen("single_enter"), KdeLockscreenMode::SingleEnter);
  EXPECT_EQ(parse_kde_lockscreen("legacy"), KdeLockscreenMode::Legacy);
  EXPECT_EQ(parse_kde_lockscreen("  auto  "), KdeLockscreenMode::Auto);
  EXPECT_EQ(parse_kde_lockscreen("\"legacy\""), KdeLockscreenMode::Legacy);
  EXPECT_EQ(parse_kde_lockscreen("invalid"), std::nullopt);
}
