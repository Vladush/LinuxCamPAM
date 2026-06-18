#include "../src/ScopedWorker.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

TEST(ScopedWorkerTest, ExecutesAndJoins) {
  std::atomic<bool> executed{false};
  {
    ScopedWorker worker([&]() { executed = true; });
    EXPECT_TRUE(worker.joinable());
  } // Destructor joins
  EXPECT_TRUE(executed.load());
}

TEST(ScopedWorkerTest, MoveConstructor) {
  std::atomic<bool> executed{false};
  ScopedWorker worker1([&]() { executed = true; });
  ScopedWorker worker2(std::move(worker1));
  
  EXPECT_FALSE(worker1.joinable());
  EXPECT_TRUE(worker2.joinable());
  
  worker2.join();
  EXPECT_FALSE(worker2.joinable());
  EXPECT_TRUE(executed.load());
}

TEST(ScopedWorkerTest, MoveAssignment) {
  std::atomic<bool> executed1{false};
  std::atomic<bool> executed2{false};
  constexpr auto kSleepDuration = std::chrono::milliseconds(10);
  
  ScopedWorker worker1([&]() {
    std::this_thread::sleep_for(kSleepDuration);
    executed1 = true; 
  });
  
  ScopedWorker worker2([&]() { executed2 = true; });
  
  // Assigning to worker1 should join worker1 first
  worker1 = std::move(worker2);
  
  EXPECT_TRUE(executed1.load());
  EXPECT_FALSE(worker2.joinable());
  EXPECT_TRUE(worker1.joinable());
  
  worker1.join();
  EXPECT_TRUE(executed2.load());
}
