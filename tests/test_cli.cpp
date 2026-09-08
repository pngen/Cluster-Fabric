// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// cluster_fabric_inspect is the read-only operator entry point. Every case
// here runs the real executable as a hidden child process with its output
// captured to a file, and asserts the exit code, the printed fields and the
// fact that nothing it inspects is modified.

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

#if !defined(_WIN32)
#error "test_cli requires the Windows CreateProcess contract used by this suite"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace cluster_fabric;

namespace {

// ---------------------------------------------------------------------------
// Child process execution
// ---------------------------------------------------------------------------

struct ChildResult {
  std::uint32_t exit_code = 0;
  std::string output;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  CF_EXPECT(stream.is_open());
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  CF_EXPECT(stream.is_open());
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  CF_EXPECT(stream.good());
}

/// Runs cluster_fabric_inspect with no visible console: CREATE_NO_WINDOW, a
/// hidden window, stdout and stderr redirected into p capture_path, and stdin
/// bound to NUL. The wait is bounded and fails the test rather than passing.
ChildResult run_inspect(const std::vector<std::string>& arguments,
                        const std::filesystem::path& capture_path) {
  std::string command_line = std::string("\"") + CLUSTER_FABRIC_TEST_INSPECT_PATH + "\"";
  for (const std::string& argument : arguments) {
    command_line += " \"";
    command_line += argument;
    command_line += "\"";
  }
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  const HANDLE output =
      CreateFileA(capture_path.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    CF_FAIL("CreateFile for the capture file failed with error " +
            std::to_string(GetLastError()) + ": " + capture_path.string());
  }
  const HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    CloseHandle(output);
    CF_FAIL("CreateFile for NUL failed with error " + std::to_string(GetLastError()));
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = input;
  startup.hStdOutput = output;
  startup.hStdError = output;

  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  const DWORD create_error = created != 0 ? 0 : GetLastError();
  CloseHandle(input);
  CloseHandle(output);
  if (created == 0) {
    CF_FAIL("CreateProcess failed with error " + std::to_string(create_error) + ": " +
            command_line);
  }

  const DWORD waited = WaitForSingleObject(process.hProcess, 30'000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 30'000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CF_FAIL("cluster_fabric_inspect did not exit within the bounded wait: " + command_line);
  }
  DWORD exit_code = 0;
  const BOOL read_exit = GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (read_exit == 0) {
    CF_FAIL("GetExitCodeProcess failed with error " + std::to_string(GetLastError()));
  }

  ChildResult result;
  result.exit_code = static_cast<std::uint32_t>(exit_code);
  result.output = read_file(capture_path);
  return result;
}

void expect_exit(const ChildResult& result, std::uint32_t expected, std::string_view context) {
  if (result.exit_code != expected) {
    CF_FAIL(std::string(context) + ": cluster_fabric_inspect exited with " +
            std::to_string(result.exit_code) + ", expected " + std::to_string(expected) +
            "\n--- captured output ---\n" + result.output + "--- end output ---");
  }
}

void expect_contains(const ChildResult& result, std::string_view needle, std::string_view context) {
  if (result.output.find(needle) == std::string::npos) {
    CF_FAIL(std::string(context) + ": output does not contain \"" + std::string(needle) +
            "\"\n--- captured output ---\n" + result.output + "--- end output ---");
  }
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

template <class Id>
Id make_id(std::string_view text) {
  const std::optional<Id> parsed = Id::parse(text);
  CF_EXPECT(parsed.has_value());
  return *parsed;
}

EvidenceStamp stamp(std::int64_t millis) {
  EvidenceStamp value;
  value.provenance = EvidenceProvenance::Measured;
  value.observed_at = Timestamp::from_unix_millis(millis);
  value.freshness = Freshness::Fresh;
  value.ttl_millis = 60'000;
  return value;
}

PersistedState::DurableRack durable_rack(const char* rack, const char* boot) {
  PersistedState::DurableRack record;
  record.rack = make_id<RackId>(rack);
  record.generation = RackGeneration::from_raw(7);
  record.membership = RackMembershipState::Active;
  record.publisher = make_id<RackPublisherId>("publisher-a");
  record.last_accepted_publication = PublicationGeneration::from_raw(2);
  record.last_boot = make_id<RackAgentBootId>(boot);
  record.rack_lifecycle = RackLifecycleState::Ready;
  record.membership_generation = MembershipGeneration::from_raw(2);
  record.origin_label = "rack-fabric:1.0.0";
  record.declared_at = Timestamp::from_unix_millis(1'000'000);
  return record;
}

/// A structurally valid durable state with two racks.
PersistedState valid_state() {
  PersistedState state;
  state.id = make_id<ClusterId>("cluster-a");
  state.epoch = ClusterEpoch::from_raw(1);
  state.last_coordinator_epoch = CoordinatorEpoch::from_raw(1);
  state.generation = ClusterGeneration::from_raw(3);
  state.membership_generation = MembershipGeneration::from_raw(2);
  state.topology_epoch = TopologyEpoch::from_raw(1);
  state.topology_generation = TopologyGeneration::from_raw(1);
  state.connectivity_generation = ConnectivityGeneration::from_raw(1);
  state.health_generation = HealthGeneration::from_raw(1);
  state.constraint_generation = ConstraintGeneration::from_raw(1);
  state.domain_generations.placement = DomainGeneration::from_raw(1);
  state.domain_generations.capacity = DomainGeneration::from_raw(1);
  state.domain_generations.failure = DomainGeneration::from_raw(1);
  state.domain_generations.network = DomainGeneration::from_raw(1);
  state.domain_generations.storage = DomainGeneration::from_raw(1);
  state.domain_generations.power = DomainGeneration::from_raw(1);
  state.domain_generations.cooling = DomainGeneration::from_raw(1);
  state.domain_generations.link = DomainGeneration::from_raw(1);
  state.snapshot_generation = SnapshotGeneration::from_raw(1);
  state.publication_generation = PublicationGeneration::from_raw(1);
  state.lifecycle = ClusterLifecycle::Ready;
  state.readiness_contract = ReadinessContract::permissive();
  state.topology_record.epoch = TopologyEpoch::from_raw(1);
  state.topology_record.generation = TopologyGeneration::from_raw(1);
  state.topology_record.established_at = Timestamp::from_unix_millis(1'000'000);
  state.topology_record.evidence = stamp(1'000'000);
  state.topology_record.reason = "cluster_declared";
  state.declared_at = Timestamp::from_unix_millis(1'000'000);
  state.last_mutation_at = Timestamp::from_unix_millis(1'000'000);
  state.racks = {durable_rack("rack-01", "boot-01"), durable_rack("rack-02", "boot-02")};
  return state;
}

std::filesystem::path fresh_directory() {
  std::error_code code;
  const std::filesystem::path base = std::filesystem::temp_directory_path(code);
  CF_EXPECT(!code);
  const std::filesystem::path directory = base / "cf-cli-test";
  std::filesystem::remove_all(directory, code);
  CF_EXPECT(!code);
  std::filesystem::create_directories(directory, code);
  CF_EXPECT(!code);
  return directory;
}

void remove_directory(const std::filesystem::path& directory) {
  std::error_code code;
  std::filesystem::remove_all(directory, code);
  CF_EXPECT(!code);
}

/// Writes a durable container with the product's own file store.
std::filesystem::path write_container(const std::filesystem::path& path,
                                      const PersistedState& state) {
  FilePersistenceStore store(path.string());
  const PersistenceStore::SaveOutcome saved = store.save(state);
  if (saved.status != PersistenceStatus::Ok) {
    CF_FAIL("FilePersistenceStore::save reported " + std::string(to_string(saved.status)) + ": " +
            saved.error.message);
  }
  CF_EXPECT(saved.ok);
  CF_EXPECT(std::filesystem::exists(path));
  return path;
}

}  // namespace

// ---------------------------------------------------------------------------
// --file
// ---------------------------------------------------------------------------

CF_TEST(inspect_file_reports_a_valid_container) {
  const std::filesystem::path directory = fresh_directory();
  const std::filesystem::path container = write_container(directory / "cluster.cf", valid_state());
  const ChildResult result = run_inspect({"--file", container.string()}, directory / "out.txt");

  expect_exit(result, 0, "--file on a container written by FilePersistenceStore");
  expect_contains(result, "container         : OK", "--file container status");
  expect_contains(result, "structural check  : OK", "--file structural check");
  expect_contains(result, "cluster           : cluster-a", "--file cluster identity");
  expect_contains(result, "racks             : 2", "--file rack count");
  expect_contains(result, "links             : 0", "--file link count");
  remove_directory(directory);
}

CF_TEST(inspect_file_json_reports_the_cluster_and_rack_count) {
  const std::filesystem::path directory = fresh_directory();
  const std::filesystem::path container = write_container(directory / "cluster.cf", valid_state());
  const ChildResult result =
      run_inspect({"--file", container.string(), "--json"}, directory / "out.json");

  expect_exit(result, 0, "--file --json on a valid container");
  expect_contains(result, "\"tool\": \"cluster_fabric_inspect\"", "--json tool field");
  expect_contains(result, "\"source\": \"file\"", "--json source field");
  expect_contains(result, "\"container_ok\": true", "--json container flag");
  expect_contains(result, "\"cluster\": \"cluster-a\"", "--json cluster field");
  expect_contains(result, "\"racks\": 2", "--json rack count");
  expect_contains(result, "\"rack_records\": [", "--json rack records");
  expect_contains(result, "{\"rack\": \"rack-01\"", "--json first rack record");
  remove_directory(directory);
}

CF_TEST(inspect_file_rejects_corrupted_and_truncated_containers) {
  const std::filesystem::path directory = fresh_directory();
  const std::filesystem::path container = write_container(directory / "cluster.cf", valid_state());
  const std::string bytes = read_file(container);
  CF_EXPECT(bytes.size() > 40);

  // A single flipped payload bit is a checksum failure, never a partial read.
  std::string corrupted = bytes;
  corrupted[24] = static_cast<char>(static_cast<unsigned char>(corrupted[24]) ^ 0x01u);
  const std::filesystem::path corrupted_path = directory / "corrupted.cf";
  write_file(corrupted_path, corrupted);
  const ChildResult checksum = run_inspect({"--file", corrupted_path.string()},
                                           directory / "corrupted.txt");
  expect_exit(checksum, 3, "--file on a corrupted container");
  expect_contains(checksum, "container         : CHECKSUM_MISMATCH",
                  "--file corrupted container status");

  // A file shorter than the header is rejected before any parsing.
  const std::filesystem::path short_path = directory / "short.cf";
  write_file(short_path, std::string(4, '\x00'));
  const ChildResult truncated = run_inspect({"--file", short_path.string()},
                                            directory / "short.txt");
  expect_exit(truncated, 3, "--file on a truncated container");
  expect_contains(truncated, "container         : TRUNCATED_HEADER",
                  "--file truncated container status");

  // A missing file is reported as missing, still with exit 3.
  const ChildResult missing = run_inspect({"--file", (directory / "absent.cf").string()},
                                          directory / "missing.txt");
  expect_exit(missing, 3, "--file on a missing container");
  expect_contains(missing, "container         : MISSING_FILE", "--file missing container status");
  remove_directory(directory);
}

CF_TEST(inspect_file_leaves_the_container_byte_identical) {
  const std::filesystem::path directory = fresh_directory();
  const std::filesystem::path container = write_container(directory / "cluster.cf", valid_state());
  const std::string before = read_file(container);

  const ChildResult first = run_inspect({"--file", container.string()}, directory / "first.txt");
  const std::string after_first = read_file(container);
  if (before != after_first) {
    CF_FAIL("--file modified the container: " + std::to_string(before.size()) + " bytes before, " +
            std::to_string(after_first.size()) + " after");
  }

  const ChildResult second = run_inspect({"--file", container.string()}, directory / "second.txt");
  CF_EXPECT_EQ(read_file(container), before);
  if (first.output != second.output) {
    CF_FAIL("two identical --file runs produced different output:\n--- first ---\n" +
            first.output + "--- second ---\n" + second.output);
  }

  // The verbose path is read-only too.
  const ChildResult verbose = run_inspect({"--file", container.string(), "--verbose"},
                                          directory / "verbose.txt");
  expect_exit(verbose, 0, "--file --verbose");
  expect_contains(verbose, "  rack rack-01 generation 7", "--verbose rack detail");
  if (read_file(container) != before) {
    CF_FAIL("--file --verbose modified the container");
  }
  remove_directory(directory);
}

// ---------------------------------------------------------------------------
// Argument handling
// ---------------------------------------------------------------------------

CF_TEST(inspect_without_arguments_exits_with_2) {
  const std::filesystem::path directory = fresh_directory();
  const ChildResult result = run_inspect({}, directory / "none.txt");
  expect_exit(result, 2, "cluster_fabric_inspect with no arguments");
  expect_contains(result, "cluster_fabric_inspect [options]", "usage text");
  expect_contains(result, "--file <path>", "usage file option");
  expect_contains(result, "--coordinator <host:port>", "usage coordinator option");

  // Both sources at once is also a usage error.
  const std::filesystem::path container = write_container(directory / "cluster.cf", valid_state());
  const ChildResult both = run_inspect({"--file", container.string(), "--coordinator",
                                        "127.0.0.1:1"},
                                       directory / "both.txt");
  expect_exit(both, 2, "--file together with --coordinator");
  remove_directory(directory);
}

// ---------------------------------------------------------------------------
// --coordinator
// ---------------------------------------------------------------------------

CF_TEST(inspect_coordinator_prints_the_semantic_digest) {
  const std::filesystem::path directory = fresh_directory();
  CoordinatorConfig config;
  config.cluster = make_id<ClusterId>("cluster-fabric-inspect");
  config.port = 0;
  ManualClock clock;
  ClusterCoordinator coordinator(config, nullptr, &clock);
  const CoordinatorStartOutcome started = coordinator.start();
  if (!started.ok) {
    CF_FAIL("ClusterCoordinator::start reported " + std::string(to_string(started.status)) + ": " +
            started.error.message);
  }
  CF_EXPECT(started.port != 0);
  const std::string endpoint = "127.0.0.1:" + std::to_string(started.port);
  const std::string digest(coordinator.snapshot().semantic_digest());

  const ChildResult result =
      run_inspect({"--coordinator", endpoint}, directory / "coordinator.txt");
  coordinator.stop();

  expect_exit(result, 0, "--coordinator against a live coordinator");
  expect_contains(result, "source            : coordinator " + endpoint, "--coordinator source");
  expect_contains(result, "cluster           : cluster-fabric-inspect", "--coordinator cluster");
  expect_contains(result, "semantic digest   : " + digest, "--coordinator semantic digest");
  remove_directory(directory);
}

CF_TEST(inspect_coordinator_does_not_advance_the_coordinator) {
  const std::filesystem::path directory = fresh_directory();
  CoordinatorConfig config;
  config.cluster = make_id<ClusterId>("cluster-fabric-inspect");
  config.port = 0;
  ManualClock clock;
  ClusterCoordinator coordinator(config, nullptr, &clock);
  const CoordinatorStartOutcome started = coordinator.start();
  if (!started.ok) {
    CF_FAIL("ClusterCoordinator::start reported " + std::string(to_string(started.status)) + ": " +
            started.error.message);
  }

  const ClusterSnapshot before = coordinator.snapshot();
  const CoordinatorStats stats_before = coordinator.stats();
  const std::string endpoint = "127.0.0.1:" + std::to_string(started.port);

  const ChildResult result =
      run_inspect({"--coordinator", endpoint, "--json"}, directory / "json.txt");
  const ClusterSnapshot after = coordinator.snapshot();
  const CoordinatorStats stats_after = coordinator.stats();
  coordinator.stop();

  expect_exit(result, 0, "--coordinator --json");
  expect_contains(result, "\"source\": \"coordinator\"", "--coordinator json source");
  expect_contains(result, "\"cluster\": \"cluster-fabric-inspect\"", "--coordinator json cluster");
  expect_contains(result, "\"semantic_digest\": \"" + before.semantic_digest() + "\"",
                  "--coordinator json digest");

  CF_EXPECT_EQ(after.semantic_digest(), before.semantic_digest());
  CF_EXPECT_EQ(after.cluster_generation(), before.cluster_generation());
  CF_EXPECT_EQ(after.cluster_epoch(), before.cluster_epoch());
  CF_EXPECT_EQ(after.coordinator_epoch(), before.coordinator_epoch());
  CF_EXPECT_EQ(after.membership_generation(), before.membership_generation());
  CF_EXPECT_EQ(after.snapshot_generation(), before.snapshot_generation());
  CF_EXPECT_EQ(stats_after.mutations_accepted, stats_before.mutations_accepted);
  CF_EXPECT_EQ(stats_after.mutations_rejected, stats_before.mutations_rejected);
  CF_EXPECT_EQ(stats_after.commits, stats_before.commits);
  CF_EXPECT_EQ(stats_after.persistence_saves, stats_before.persistence_saves);
  remove_directory(directory);
}

int main() { return cf_test::run("test_cli"); }
