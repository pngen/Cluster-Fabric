// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real process death: a rack agent is an independent operating-system process.
// Killing it must fence its incarnation, invalidate the evidence it owned, and
// refuse every later publication under the dead boot. A fresh process for the
// same rack must be able to republish current evidence, while the dead
// incarnation stays fenced forever.
//
// No timeout is used as a success criterion anywhere in this test: every wait
// is a bounded poll that FAILS when the awaited condition never becomes true.

#include "harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#error "test_process_death requires the Windows CreateProcess implementation"
#endif

using namespace cluster_fabric;

namespace {

/// Bounded poll parameters. The poll never decides success by time: it fails
/// when the condition is still false after the last attempt.
constexpr std::size_t kPollAttempts = 200;
constexpr DWORD kPollIntervalMillis = 50;
constexpr std::int64_t kBaseMillis = 1'700'000'000'000;

void expect_true(bool condition, const std::string& detail) {
  if (!condition) {
    CF_FAIL(detail);
  }
}

void expect_accepted(const MutationResult& result, const std::string& what) {
  if (result.accepted()) {
    return;
  }
  if (result.outcome == MutationOutcome::Rejected && result.reason == RejectionReason::None) {
    CF_FAIL(what + ": the coordinator returned the untyped sentinel rejection "
                   "(outcome=REJECTED reason=NONE error=" +
            result.error.describe() + "), which means an internal validation helper returned a "
            "default MutationResult instead of a success result");
  }
  CF_FAIL(what + ": expected acceptance, got outcome=" +
          std::string(to_string(result.outcome)) + " reason=" +
          std::string(to_string(result.reason)) + " error=" + result.error.describe());
}

[[nodiscard]] std::string read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return "<unreadable: " + path + ">";
  }
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return text;
}

[[nodiscard]] std::string quote_argument(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

/// One spawned child process. The job object below kills every child when the
/// test process exits, so a failing test can never leave an orphan agent.
struct ChildProcess {
  PROCESS_INFORMATION info{};
  HANDLE log_handle = INVALID_HANDLE_VALUE;
  std::string log_path;
  bool running = false;
};

class JobObject {
 public:
  JobObject() {
    job_ = ::CreateJobObjectA(nullptr, nullptr);
    expect_true(job_ != nullptr, "CreateJobObject failed: " + std::to_string(::GetLastError()));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    expect_true(::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                          static_cast<DWORD>(sizeof(limits))) != 0,
                "SetInformationJobObject failed: " + std::to_string(::GetLastError()));
  }

  ~JobObject() {
    if (job_ != nullptr) {
      ::CloseHandle(job_);
    }
  }

  JobObject(const JobObject&) = delete;
  JobObject& operator=(const JobObject&) = delete;

  void assign(const ChildProcess& child) const {
    expect_true(::AssignProcessToJobObject(job_, child.info.hProcess) != 0,
                "AssignProcessToJobObject failed: " + std::to_string(::GetLastError()));
  }

  [[nodiscard]] DWORD active_processes() const {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (::QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting,
                                    static_cast<DWORD>(sizeof(accounting)), nullptr) == 0) {
      CF_FAIL("QueryInformationJobObject failed: " + std::to_string(::GetLastError()));
    }
    return accounting.ActiveProcesses;
  }

 private:
  HANDLE job_ = nullptr;
};

/// Spawns one child process with CREATE_NO_WINDOW and no visible console.
/// stdout and stderr are redirected to a file so a failure can report exactly
/// what the agent observed.
[[nodiscard]] ChildProcess spawn_agent(const std::string& executable,
                                       const std::string& command_line,
                                       const std::string& log_path) {
  ChildProcess child;
  child.log_path = log_path;

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = static_cast<DWORD>(sizeof(attributes));
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  child.log_handle = ::CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  expect_true(child.log_handle != INVALID_HANDLE_VALUE,
              "could not create the agent log " + log_path + ": " +
                  std::to_string(::GetLastError()));

  const HANDLE null_input = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
  expect_true(null_input != INVALID_HANDLE_VALUE,
              "could not open NUL for the child stdin: " + std::to_string(::GetLastError()));

  STARTUPINFOA startup{};
  startup.cb = static_cast<DWORD>(sizeof(startup));
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input;
  startup.hStdOutput = child.log_handle;
  startup.hStdError = child.log_handle;

  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  const BOOL created = ::CreateProcessA(executable.c_str(), mutable_line.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &child.info);
  const DWORD create_error = ::GetLastError();
  ::CloseHandle(null_input);
  expect_true(created != 0, "CreateProcess failed for " + command_line + ": " +
                                std::to_string(create_error));
  child.running = true;
  return child;
}

[[nodiscard]] bool process_exited(const ChildProcess& child) {
  return ::WaitForSingleObject(child.info.hProcess, 0) == WAIT_OBJECT_0;
}

[[nodiscard]] DWORD process_exit_code(const ChildProcess& child) {
  DWORD exit_code = 0;
  expect_true(::GetExitCodeProcess(child.info.hProcess, &exit_code) != 0,
              "GetExitCodeProcess failed: " + std::to_string(::GetLastError()));
  return exit_code;
}

/// Terminates the child and asserts the operating system reports it gone.
void terminate_and_reap(ChildProcess& child) {
  if (!child.running) {
    return;
  }
  if (::TerminateProcess(child.info.hProcess, 0) == 0) {
    expect_true(process_exited(child),
                "TerminateProcess failed and the process is still running: " +
                    std::to_string(::GetLastError()));
  }
  const DWORD waited = ::WaitForSingleObject(child.info.hProcess, INFINITE);
  expect_true(waited == WAIT_OBJECT_0,
              "WaitForSingleObject returned " + std::to_string(waited) + " for the terminated agent");
  expect_true(process_exited(child), "the terminated agent process is still running");
  expect_true(process_exit_code(child) != STILL_ACTIVE,
              "the terminated agent still reports STILL_ACTIVE");
  ::CloseHandle(child.info.hThread);
  ::CloseHandle(child.info.hProcess);
  child.info.hThread = nullptr;
  child.info.hProcess = nullptr;
  child.running = false;
  if (child.log_handle != INVALID_HANDLE_VALUE) {
    ::CloseHandle(child.log_handle);
    child.log_handle = INVALID_HANDLE_VALUE;
  }
}

[[nodiscard]] ClusterId test_cluster() {
  return *ClusterId::parse("cluster-fabric-process-death");
}

[[nodiscard]] MutationRequest declare_request(const ClusterId& cluster,
                                              CoordinatorEpoch coordinator_epoch) {
  MutationRequest request;
  request.kind = MutationKind::DeclareCluster;
  request.cluster = cluster;
  request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  request.authority.coordinator_epoch = coordinator_epoch;
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract{};
  request.evidence = EvidenceStamp::make(EvidenceProvenance::Reported,
                                         Timestamp::from_unix_millis(kBaseMillis), 30'000);
  request.requested_at = Timestamp::from_unix_millis(kBaseMillis);
  request.reason = "test_declare_cluster";
  return request;
}

/// A rack publication authored by a process incarnation that is expected to be
/// fenced. Used to prove that the coordinator refuses it.
[[nodiscard]] MutationRequest dead_boot_publication(const ClusterId& cluster,
                                                    const ClusterState& state, const RackId& rack,
                                                    const RackPublisherId& publisher,
                                                    const RackAgentBootId& boot,
                                                    RackGeneration generation,
                                                    PublicationGeneration publication) {
  MutationRequest request;
  request.kind = MutationKind::AddRack;
  request.cluster = cluster;
  request.authority.cluster_epoch = state.epoch;
  request.authority.coordinator_epoch = state.coordinator_epoch;
  request.authority.boot = boot;
  request.authority.publisher = publisher;
  request.authority.rack = rack;
  request.authority.rack_generation = generation;
  request.authority.publication = publication;
  const EvidenceStamp stamp = EvidenceStamp::make(
      EvidenceProvenance::Reported, Timestamp::from_unix_millis(kBaseMillis), 30'000);
  request.evidence = stamp;
  request.requested_at = Timestamp::from_unix_millis(kBaseMillis);
  request.reason = "test_dead_boot_publication";
  request.rack_reference.rack = rack;
  request.rack_reference.generation = generation;
  request.rack_reference.rack_lifecycle = RackLifecycleState::Ready;
  request.rack_reference.composition.composition_label = "dead-boot-composition";
  request.rack_reference.composition.provenance = EvidenceProvenance::Reported;
  request.rack_reference.evidence = stamp;
  request.rack_reference.publisher = publisher;
  request.rack_reference.publication = publication;
  request.rack_reference.boot = boot;
  request.rack_reference.cluster_epoch = state.epoch;
  request.rack_reference.coordinator_epoch = state.coordinator_epoch;
  request.rack_reference.health = HealthState::Healthy;
  request.rack_reference.origin_label = "cluster-fabric-test:dead-boot";
  return request;
}

struct RackObservation {
  bool present = false;
  bool authoritative_current = false;
  RackMembershipState membership = RackMembershipState::Unknown;
  RackCurrentness currentness = RackCurrentness::Unknown;
  RackGeneration generation;
  RackAgentBootId boot;
  ClusterGeneration cluster_generation;
  CoordinatorEpoch coordinator_epoch;
  std::size_t fenced_count = 0;
};

[[nodiscard]] RackObservation observe(const ClusterCoordinator& coordinator, const RackId& rack) {
  RackObservation observation;
  const ClusterState state = coordinator.state_copy();
  observation.cluster_generation = state.generation;
  observation.coordinator_epoch = state.coordinator_epoch;
  observation.fenced_count = state.fenced_authorities.size();
  const auto it = state.racks.find(rack);
  if (it == state.racks.end()) {
    return observation;
  }
  observation.present = true;
  observation.authoritative_current = it->second.authoritative_current;
  observation.membership = it->second.membership;
  observation.currentness = it->second.reference.currentness;
  observation.generation = it->second.reference.generation;
  observation.boot = it->second.reference.boot;
  return observation;
}

[[nodiscard]] std::string describe_observation(const RackObservation& observation) {
  if (!observation.present) {
    return "rack=absent cluster_generation=" + observation.cluster_generation.str() +
           " coordinator_epoch=" + observation.coordinator_epoch.str();
  }
  return "cluster_generation=" + observation.cluster_generation.str() + " coordinator_epoch=" +
         observation.coordinator_epoch.str() + " rack_generation=" +
         observation.generation.str() + " boot=" + observation.boot.value() + " membership=" +
         std::string(to_string(observation.membership)) + " currentness=" +
         std::string(to_string(observation.currentness)) +
         " authoritative=" + (observation.authoritative_current ? "true" : "false") +
         " fenced=" + std::to_string(observation.fenced_count);
}

/// Bounded poll for an asynchronous condition. Prints every distinct
/// observation so a failure can be diagnosed from the output alone.
template <class Predicate>
[[nodiscard]] bool poll_until(const ClusterCoordinator& coordinator, const RackId& rack,
                              Predicate predicate, const std::string& what,
                              const ChildProcess* child, std::string& last_observation) {
  std::string previous;
  for (std::size_t attempt = 0; attempt < kPollAttempts; ++attempt) {
    const RackObservation observation = observe(coordinator, rack);
    const std::string rendered = describe_observation(observation);
    if (rendered != previous) {
      std::cout << "  [" << what << " poll " << attempt << "] " << rendered << "\n";
      previous = rendered;
    }
    if (predicate(observation)) {
      last_observation = rendered;
      return true;
    }
    if (child != nullptr && child->running && process_exited(*child)) {
      CF_FAIL(what + ": the rack agent process exited with code " +
              std::to_string(process_exit_code(*child)) + " before the condition held; last " +
              rendered + "; agent output: " + read_text_file(child->log_path));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMillis));
  }
  last_observation = previous;
  return false;
}

}  // namespace

CF_TEST(rack_agent_process_death_fences_the_incarnation_and_a_fresh_process_recovers) {
  const std::string agent_path = CLUSTER_FABRIC_TEST_RACK_AGENT_PATH;
  {
    std::ifstream probe(agent_path, std::ios::binary);
    expect_true(probe.is_open(), "the rack agent executable is missing: " + agent_path);
  }

  const ClusterId cluster = test_cluster();
  const RackId rack = *RackId::parse("rack-001");
  const RackPublisherId publisher = *RackPublisherId::parse("rack-publisher-01");
  const RackGeneration generation = RackGeneration::from_raw(1);

  CoordinatorConfig config;
  config.cluster = cluster;
  config.readiness_contract = ReadinessContract{};
  config.bind_address = "127.0.0.1";
  config.port = 0;
  ClusterCoordinator coordinator(config, nullptr, nullptr);
  const CoordinatorStartOutcome started = coordinator.start();
  expect_true(started.ok, "the coordinator did not start: " + started.error.describe());
  const std::uint16_t port = coordinator.port();
  expect_true(port != 0, "the coordinator did not report a bound port");
  expect_accepted(coordinator.submit(
                      declare_request(cluster, coordinator.state_copy().coordinator_epoch)),
                  "declare cluster");

  const std::string endpoint = "127.0.0.1:" + std::to_string(port);
  const std::string common_flags =
      " --cluster " + cluster.value() + " --coordinator " + endpoint + " --rack " + rack.value() +
      " --publisher " + publisher.value() + " --generation " + generation.str() +
      " --heartbeat 100 --ttl 30000 --devices 4 --device-memory 16384";

  JobObject job;
  std::string log_directory = ".";
  {
    char buffer[MAX_PATH] = {};
    const DWORD length = ::GetTempPathA(MAX_PATH, buffer);
    if (length > 0 && length < MAX_PATH) {
      log_directory = std::string(buffer);
    }
  }
  const std::string first_log = log_directory + "cf-process-death-agent-1.log";

  ChildProcess first = spawn_agent(agent_path, quote_argument(agent_path) + common_flags, first_log);
  job.assign(first);

  std::string observation;
  const bool became_current = poll_until(
      coordinator, rack,
      [](const RackObservation& value) {
        return value.present && value.authoritative_current &&
               value.membership == RackMembershipState::Active &&
               value.currentness == RackCurrentness::Current;
      },
      "agent live", &first, observation);
  if (!became_current) {
    terminate_and_reap(first);
    CF_FAIL("the coordinator never observed rack/" + rack.value() +
            " as authoritative current under the live agent; last observation: " + observation +
            "; agent output: " + read_text_file(first_log));
  }

  const RackObservation live = observe(coordinator, rack);
  const RackAgentBootId dead_boot = live.boot;
  expect_true(dead_boot.known(), "the live agent published no boot identity");
  std::cout << "  live agent: " << describe_observation(live) << "\n";

  // --- real process death --------------------------------------------------
  terminate_and_reap(first);
  expect_true(!first.running, "the first agent process is still running");

  const bool fenced = poll_until(
      coordinator, rack,
      [](const RackObservation& value) {
        // The fenced incarnation keeps its boot identity as historical fact; the
        // fence counter and the loss of authority are what prove the death was
        // observed. A different boot appears only when the fresh process starts.
        return value.present && !value.authoritative_current && value.fenced_count > 0 &&
               value.currentness == RackCurrentness::RevalidationRequired;
      },
      "agent dead", nullptr, observation);
  expect_true(fenced, "rack/" + rack.value() +
                          " was not fenced after the agent process died; last observation: " +
                          observation);

  const ClusterState fenced_state = coordinator.state_copy();
  const auto fenced_record = fenced_state.racks.find(rack);
  expect_true(fenced_record != fenced_state.racks.end(),
              "rack/" + rack.value() + " vanished after the agent died");
  expect_true(!fenced_record->second.authoritative_current,
              "the dead agent's rack is still authoritative current");
  expect_true(fenced_record->second.reference.evidence.requires_revalidation(),
              "the dead agent's evidence does not require revalidation (freshness=" +
                  std::string(to_string(fenced_record->second.reference.evidence.freshness)) + ")");
  expect_true(fenced_record->second.reference.currentness == RackCurrentness::RevalidationRequired,
              "currentness after process death=" +
                  std::string(to_string(fenced_record->second.reference.currentness)));
  expect_true(fenced_state.is_boot_fenced(dead_boot),
              "the dead boot " + dead_boot.value() + " is not fenced");
  expect_true(fenced_state.lifecycle == ClusterLifecycle::RevalidationRequired,
              "lifecycle after process death=" +
                  std::string(to_string(fenced_state.lifecycle)));

  bool fence_reason_ok = false;
  for (const FencedAuthority& entry : fenced_state.fenced_authorities) {
    if (entry.boot == dead_boot) {
      fence_reason_ok = entry.reason == "session_closed";
      expect_true(fence_reason_ok, "fence reason for the dead boot is '" + entry.reason +
                                       "', expected 'session_closed'");
      expect_true(entry.rack == rack, "the fenced authority names rack " + entry.rack.value());
    }
  }
  expect_true(fence_reason_ok, "the dead boot has no fence entry");

  const CoordinatorStats fenced_stats = coordinator.stats();
  expect_true(fenced_stats.sessions_closed >= 1,
              "the coordinator closed no session: " + std::to_string(fenced_stats.sessions_closed));
  expect_true(fenced_stats.fence_events >= 1,
              "the coordinator recorded no fence event: " +
                  std::to_string(fenced_stats.fence_events));

  const MutationResult dead_publication =
      coordinator.submit(dead_boot_publication(cluster, fenced_state, rack, publisher, dead_boot,
                                               generation, PublicationGeneration::from_raw(50)));
  expect_true(dead_publication.outcome == MutationOutcome::Rejected &&
                  dead_publication.reason == RejectionReason::StaleRackBoot,
              "a publication under the dead boot was not rejected with REJECT_STALE_RACK_BOOT: "
              "outcome=" + std::string(to_string(dead_publication.outcome)) + " reason=" +
                  std::string(to_string(dead_publication.reason)) + " error=" +
                  dead_publication.error.describe());

  // --- a fresh process for the same rack -----------------------------------
  const std::string second_log = log_directory + "cf-process-death-agent-2.log";
  ChildProcess second =
      spawn_agent(agent_path, quote_argument(agent_path) + common_flags, second_log);
  job.assign(second);

  const bool recovered = poll_until(
      coordinator, rack,
      [&dead_boot](const RackObservation& value) {
        return value.present && value.authoritative_current && value.boot != dead_boot &&
               value.currentness == RackCurrentness::Current;
      },
      "fresh agent", &second, observation);
  if (!recovered) {
    terminate_and_reap(second);
    CF_FAIL("rack/" + rack.value() +
            " was not republished as current by a fresh agent process; last observation: " +
            observation + "; agent output: " + read_text_file(second_log));
  }

  const RackObservation fresh = observe(coordinator, rack);
  std::cout << "  fresh agent: " << describe_observation(fresh) << "\n";
  expect_true(fresh.boot != dead_boot, "the fresh agent reused the dead boot identity");

  const ClusterState recovered_state = coordinator.state_copy();
  expect_true(recovered_state.is_boot_fenced(dead_boot),
              "the dead boot " + dead_boot.value() + " is no longer fenced");
  expect_true(!recovered_state.is_boot_fenced(fresh.boot),
              "the fresh boot " + fresh.boot.value() + " is fenced");
  expect_true(recovered_state.lifecycle == ClusterLifecycle::Ready,
              "lifecycle after recovery=" + std::string(to_string(recovered_state.lifecycle)));

  const ClusterSnapshot snapshot = coordinator.snapshot();
  const SnapshotValidation validation = coordinator.validate(snapshot);
  expect_true(validation.current, "the recovered snapshot is not current: " +
                                      validation.describe());
  expect_true(validation.consumable, "the recovered snapshot is not consumable: " +
                                         validation.describe());
  expect_true(snapshot.semantic_digest() == semantic_digest_of(recovered_state),
              "the recovered snapshot digest " + snapshot.semantic_digest() +
                  " does not match current state digest " +
                  semantic_digest_of(recovered_state));

  terminate_and_reap(second);

  // --- no orphan agent process may remain ----------------------------------
  DWORD active = job.active_processes();
  for (std::size_t attempt = 0; attempt < kPollAttempts && active != 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMillis));
    active = job.active_processes();
  }
  expect_true(active == 0, "orphan rack agent processes remain in the job object: " +
                               std::to_string(active));

  coordinator.stop();
}

int main() { return cf_test::run("test_process_death"); }
