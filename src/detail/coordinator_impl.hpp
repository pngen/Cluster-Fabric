// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal coordinator implementation. Not installed and not part of the
// public contract.

#ifndef CLUSTER_FABRIC_DETAIL_COORDINATOR_IMPL_HPP
#define CLUSTER_FABRIC_DETAIL_COORDINATOR_IMPL_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cluster_fabric/coordinator.hpp"
#include "cluster_fabric/invariant.hpp"
#include "cluster_fabric/persistence.hpp"
#include "cluster_fabric/protocol.hpp"
#include "cluster_fabric/snapshot.hpp"

namespace cluster_fabric {
namespace detail {

#if defined(_WIN32)
using socket_handle = std::uintptr_t;
inline constexpr socket_handle kInvalidSocket =
    static_cast<socket_handle>(~static_cast<std::uintptr_t>(0));
#else
using socket_handle = int;
inline constexpr socket_handle kInvalidSocket = -1;
#endif

/// Initializes the platform socket library exactly once.
[[nodiscard]] bool socket_library_init() noexcept;
[[nodiscard]] std::string socket_error_text() noexcept;
void close_socket(socket_handle handle) noexcept;
void shutdown_socket(socket_handle handle) noexcept;
/// Binds and listens on the loopback address. Returns kInvalidSocket on
/// failure and 0.0.0.0 is never used: this build is validated as a single-host
/// reference deployment.
[[nodiscard]] socket_handle open_listener(const std::string& bind_address, std::uint16_t port,
                                          std::size_t backlog) noexcept;
/// The port the listener is actually bound to.
[[nodiscard]] std::uint16_t listener_port(socket_handle handle) noexcept;

/// Canonical state -> durable container. Dynamic observation is not included.
[[nodiscard]] PersistedState to_persisted_state(const ClusterState& state);
/// Durable container -> canonical state. Every dynamic observation is cleared
/// and every recovered rack requires revalidation.
[[nodiscard]] ClusterState from_persisted_state(const PersistedState& persisted,
                                               CoordinatorEpoch coordinator_epoch, Timestamp now,
                                               RecoveryReport& report);

/// One in-flight mutation with a completion slot.
struct RequestSlot {
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  MutationResult result;
};

struct QueueItem {
  enum class Kind { Mutation, Fence, Stop } kind = Kind::Mutation;
  MutationRequest request;
  std::shared_ptr<RequestSlot> slot;
  RackAgentBootId fence_boot;
  std::string fence_reason;
};

/// One accepted network session.
struct Session {
  std::uint64_t id = 0;
  socket_handle handle = kInvalidSocket;
  std::optional<RackAgentBootId> boot;
  std::vector<RackId> racks;
  std::atomic<bool> stop{false};
  std::mutex write_mutex;
  std::thread thread;
};

/// Undo journal used by the commit path. Records the prior value of every
/// record the candidate mutation touches, so a failed invariant check or a
/// failed persistence commit restores canonical state exactly.
class StateJournal {
 public:
  explicit StateJournal(ClusterState& state) : state_(state) {}

  /// Records the current value of one canonical map entry so it can be
  /// restored exactly.
  template <class Key, class Value>
  void record(std::map<Key, Value> ClusterState::* member, const Key& key) {
    std::map<Key, Value>& map = state_.*member;
    const auto it = map.find(key);
    if (it == map.end()) {
      undo_.push_back([&map, key]() { map.erase(key); });
      return;
    }
    const Value previous = it->second;
    undo_.push_back([&map, key, previous]() { map.insert_or_assign(key, previous); });
  }

  /// Records one rack record so it can be restored exactly.
  void record_rack(const RackId& rack) { record(&ClusterState::racks, rack); }

  /// Records every scalar field, counter and identity list.
  void record_scalars();

  void rollback();
  [[nodiscard]] bool empty() const noexcept { return undo_.empty(); }

 private:
  ClusterState& state_;
  std::vector<std::function<void()>> undo_;
};

}  // namespace detail

/// Coordinator implementation.
///
/// Locking contract (audited):
///   state_mutex    guards canonical state, derived indexes and state_version.
///                  It is never held across socket I/O, persistence I/O,
///                  condition-variable waits, hook callbacks or thread joins.
///   queue_mutex    guards the request queue only. It is released before the
///                  commit thread touches canonical state.
///   sessions_mutex guards the session registry only. Sessions are joined
///                  after the registry lock is released.
///   write_mutex    per session, guards that session's socket writes only.
/// Lock order where more than one is held: state_mutex is always taken last
/// and never while queue_mutex or sessions_mutex is held.
struct ClusterCoordinator::Impl {
  Impl(CoordinatorConfig config, PersistenceStore* store, const Clock* clock);

  CoordinatorConfig config;
  PersistenceStore* store = nullptr;
  const Clock* clock = nullptr;

  /// Canonical state. Commit-thread writes, any-thread reads.
  mutable std::mutex state_mutex;
  ClusterState canonical;
  ClusterIndexes indexes;
  bool indexes_valid = false;
  std::uint64_t state_version = 0;
  std::uint64_t indexes_version = 0;

  /// Request queue.
  std::mutex queue_mutex;
  std::condition_variable queue_condition;
  std::deque<detail::QueueItem> queue;
  bool stopping = false;
  /// True while the commit thread is live. A coordinator that was never started
  /// (or has been stopped) must refuse a mutation instead of waiting forever for
  /// a result that no thread will ever produce.
  std::atomic<bool> running{false};
  std::thread commit_thread;

  /// Network.
  std::thread accept_thread;
  std::mutex sessions_mutex;
  std::map<std::uint64_t, std::shared_ptr<detail::Session>> sessions;
  std::atomic<std::uint64_t> next_session_id{1};
  std::atomic<bool> accepting{false};
  detail::socket_handle listener = detail::kInvalidSocket;
  std::uint16_t bound_port = 0;

  RecoveryReport recovery;
  std::atomic<CommitHook*> hook{nullptr};

  std::atomic<std::uint64_t> mutations_accepted{0};
  std::atomic<std::uint64_t> mutations_no_change{0};
  std::atomic<std::uint64_t> mutations_rejected{0};
  std::atomic<std::uint64_t> sessions_opened{0};
  std::atomic<std::uint64_t> sessions_closed{0};
  std::atomic<std::uint64_t> frames_in{0};
  std::atomic<std::uint64_t> frames_out{0};
  std::atomic<std::uint64_t> bytes_in{0};
  std::atomic<std::uint64_t> bytes_out{0};
  std::atomic<std::uint64_t> commits{0};
  std::atomic<std::uint64_t> persistence_saves{0};
  std::atomic<std::uint64_t> persistence_failures{0};
  std::atomic<std::uint64_t> fence_events{0};
  std::atomic<std::uint64_t> stale_replays_rejected{0};
  std::atomic<std::uint64_t> snapshots_built{0};
  std::atomic<std::uint64_t> validation_requests{0};

  [[nodiscard]] Timestamp now() const;
  [[nodiscard]] ClusterState state_copy();
  [[nodiscard]] ClusterSnapshot make_snapshot();

  /// Locks state_mutex, rebuilds the derived indexes when stale and invokes
  /// p body with canonical state and indexes. p body must not call back
  /// into the coordinator.
  template <class Body>
  decltype(auto) with_state(Body&& body) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!indexes_valid || indexes_version != state_version) {
      indexes.rebuild(canonical);
      indexes_version = state_version;
      indexes_valid = true;
    }
    return body(canonical, indexes);
  }

  /// Enqueues a mutation and waits for its typed result.
  [[nodiscard]] MutationResult submit(const MutationRequest& request);

  /// Commit path. Called only by the commit thread.
  void run_commit();
  [[nodiscard]] MutationResult apply_and_commit(const MutationRequest& request);
  void apply_fence(const RackAgentBootId& boot, std::string_view reason);

  void run_accept();
  void run_session(const std::shared_ptr<detail::Session>& session);
  /// Writes one frame. The result is deliberately not [[nodiscard]]: a
  /// session's read loop is driven by socket state, and a failed write ends
  /// the session on the next read.
  bool send_frame(detail::Session& session, MessageType type, std::uint64_t sequence,
                  const std::string& payload);
  void detach_session(std::uint64_t id);
  void close_all_sessions();

  [[nodiscard]] bool load_durable_state();
  [[nodiscard]] bool persist_now(MutationResult& result);
  void touch_state();

  /// Validates a compact snapshot view against current authority.
  [[nodiscard]] SnapshotValidation validate_view(const SnapshotView& view);

  /// Stops the coordinator. Defined in coordinator_network.cpp.
  void stop();
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_DETAIL_COORDINATOR_IMPL_HPP
