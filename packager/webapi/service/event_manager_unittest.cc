// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/webapi/service/event_manager.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace shaka {
namespace webapi {
namespace {

class EventManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    char dir_template[] = "/tmp/event_manager_test_XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(dir_template));
    temp_dir_ = dir_template;
  }
  void TearDown() override {
    std::string cmd = "rm -rf " + temp_dir_;
    system(cmd.c_str());
  }

  // Writes an executable shell script and returns its path.
  std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = temp_dir_ + "/" + name;
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
    out.close();
    chmod(path.c_str(), 0755);
    return path;
  }

  EventManager::Config MakeConfig(const std::string& bin) {
    EventManager::Config config;
    config.packager_bin = bin;
    config.log_dir = temp_dir_;
    config.metrics_port_min = 21100;
    config.metrics_port_max = 21102;  // pool of 3 for exhaustion tests
    config.readiness_timeout_ms = 200;
    return config;
  }

  // Polls GetEvent until |state| or 5s deadline.
  bool WaitForState(EventManager* manager, const std::string& id,
                    EventState state) {
    for (int i = 0; i < 100; ++i) {
      auto snap = manager->GetEvent(id);
      if (snap && snap->state == state)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  // Polls for |path| to exist, up to |deadline_ms|. Used so tests only
  // signal a fake child once it has installed its trap handler (the
  // manager's RUNNING transition is time-based, not trap-aware, so it does
  // not by itself guarantee the trap is installed yet).
  bool WaitForFile(const std::string& path, int deadline_ms) {
    const int iterations = deadline_ms / 20;
    for (int i = 0; i < iterations; ++i) {
      struct stat st;
      if (stat(path.c_str(), &st) == 0)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  std::string temp_dir_;
};

TEST_F(EventManagerTest, LifecycleRunningThenDrainStop) {
  // Child logs a line, handles TERM by exiting 0, sleeps forever. Touches
  // a sentinel file right after installing the trap so the test can wait
  // for the trap to actually be armed before sending TERM (the manager's
  // RUNNING transition is a readiness-timeout fallback, not proof the
  // trap is installed).
  const std::string bin = WriteScript(
      "child.sh", "echo hello-from-child >&2\ntrap 'exit 0' TERM\n"
                  "touch \"$0.ready\"\nwhile true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {"--fake_flag", "x"}, 5).ok());

  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kRunning));
  ASSERT_TRUE(WaitForFile(bin + ".ready", 5000));
  auto snap = manager.GetEvent("ev1");
  ASSERT_TRUE(snap.has_value());
  EXPECT_GT(snap->pid, 0);
  EXPECT_GE(snap->metrics_port, 21100);
  EXPECT_LE(snap->metrics_port, 21102);
  // argv_tail is preserved and --metrics_port appended.
  EXPECT_EQ("--fake_flag", snap->argv[1]);
  EXPECT_EQ("--metrics_port", snap->argv[3]);

  // stderr reached the log file.
  std::ifstream log(snap->log_path);
  std::string line;
  std::getline(log, line);
  EXPECT_EQ("hello-from-child", line);

  ASSERT_TRUE(manager.StopEvent("ev1", /*kill_now=*/false).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kStopped));
  EXPECT_EQ(0, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, KillEscalationWhenTermIgnored) {
  // Sentinel guards against sending TERM before the trap is armed: an
  // early TERM would kill the child outright (default disposition) instead
  // of being ignored, which would falsify the 137/SIGKILL assertion below.
  const std::string bin = WriteScript(
      "stubborn.sh",
      "trap '' TERM\ntouch \"$0.ready\"\nwhile true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {}, /*stop_timeout_seconds=*/1).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kRunning));
  ASSERT_TRUE(WaitForFile(bin + ".ready", 5000));

  ASSERT_TRUE(manager.StopEvent("ev1", /*kill_now=*/false).ok());
  // TERM ignored -> after 1s timeout the manager SIGKILLs.
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kStopped));
  // SIGKILL death is reported as 128+9.
  EXPECT_EQ(137, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, CrashBecomesFailedWithExitCode) {
  const std::string bin = WriteScript("crash.sh", "exit 7\n");
  EventManager manager(MakeConfig(bin));
  ASSERT_TRUE(manager.CreateEvent("ev1", {}, 5).ok());
  ASSERT_TRUE(WaitForState(&manager, "ev1", EventState::kFailed));
  EXPECT_EQ(7, manager.GetEvent("ev1")->exit_code.value());
}

TEST_F(EventManagerTest, DuplicateIdRejectedAndPortPoolExhausts) {
  const std::string bin = WriteScript(
      "child.sh", "trap 'exit 0' TERM\nwhile true; do sleep 0.1; done\n");
  EventManager manager(MakeConfig(bin));  // pool size 3
  ASSERT_TRUE(manager.CreateEvent("dup", {}, 5).ok());
  EXPECT_FALSE(manager.CreateEvent("dup", {}, 5).ok());

  ASSERT_TRUE(manager.CreateEvent("e2", {}, 5).ok());
  ASSERT_TRUE(manager.CreateEvent("e3", {}, 5).ok());
  // Pool of 3 exhausted (dup + e2 + e3).
  EXPECT_FALSE(manager.CreateEvent("e4", {}, 5).ok());

  // Stopping an event frees its port for reuse.
  ASSERT_TRUE(manager.StopEvent("e3", /*kill_now=*/true).ok());
  ASSERT_TRUE(WaitForState(&manager, "e3", EventState::kStopped));
  EXPECT_TRUE(manager.CreateEvent("e5", {}, 5).ok());
}

TEST_F(EventManagerTest, ShutdownStopsEverything) {
  const std::string bin = WriteScript(
      "child.sh", "trap 'exit 0' TERM\nwhile true; do sleep 0.1; done\n");
  auto manager = std::make_unique<EventManager>(MakeConfig(bin));
  ASSERT_TRUE(manager->CreateEvent("a", {}, 2).ok());
  ASSERT_TRUE(manager->CreateEvent("b", {}, 2).ok());
  ASSERT_TRUE(WaitForState(manager.get(), "a", EventState::kRunning));
  ASSERT_TRUE(WaitForState(manager.get(), "b", EventState::kRunning));
  manager->Shutdown();
  EXPECT_EQ(EventState::kStopped, manager->GetEvent("a")->state);
  EXPECT_EQ(EventState::kStopped, manager->GetEvent("b")->state);
}

TEST_F(EventManagerTest, EvictsOldestTerminalEventsBeyondCap) {
  const std::string bin = WriteScript(
      "child.sh", "trap 'exit 0' TERM\nwhile true; do sleep 0.1; done\n");
  EventManager::Config config = MakeConfig(bin);
  config.max_terminal_events = 2;
  EventManager manager(config);

  // Create, run, and drain-stop 3 events in order so their stopped_unix
  // values are strictly increasing (each waits for kStopped before the
  // next starts).
  const char* ids[] = {"e1", "e2", "e3"};
  for (const char* id : ids) {
    ASSERT_TRUE(manager.CreateEvent(id, {}, 5).ok());
    ASSERT_TRUE(WaitForState(&manager, id, EventState::kRunning));
    ASSERT_TRUE(manager.StopEvent(id, /*kill_now=*/false).ok());
    ASSERT_TRUE(WaitForState(&manager, id, EventState::kStopped));
  }

  // Creating a 4th, still-live event pushes the terminal count to 3,
  // which exceeds the cap of 2 and evicts the oldest stopped event (e1).
  ASSERT_TRUE(manager.CreateEvent("e4", {}, 5).ok());

  EXPECT_FALSE(manager.GetEvent("e1").has_value());
  EXPECT_TRUE(manager.GetEvent("e2").has_value());
  EXPECT_TRUE(manager.GetEvent("e3").has_value());
  EXPECT_TRUE(manager.GetEvent("e4").has_value());
  EXPECT_EQ(3u, manager.ListEvents().size());  // e2, e3 (terminal) + e4 (live)

  ASSERT_TRUE(manager.StopEvent("e4", /*kill_now=*/true).ok());
  ASSERT_TRUE(WaitForState(&manager, "e4", EventState::kStopped));
}

TEST(EventStateNameTest, MatchesApiStrings) {
  EXPECT_EQ("STARTING", EventStateName(EventState::kStarting));
  EXPECT_EQ("RUNNING", EventStateName(EventState::kRunning));
  EXPECT_EQ("STOPPING", EventStateName(EventState::kStopping));
  EXPECT_EQ("STOPPED", EventStateName(EventState::kStopped));
  EXPECT_EQ("FAILED", EventStateName(EventState::kFailed));
}

}  // namespace
}  // namespace webapi
}  // namespace shaka
