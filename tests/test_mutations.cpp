// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Mutation acceptance, idempotence, rejection, authority ordering,
// transactional rollback and commit-hook ordering.
//
// Every MutationKind is exercised through the public ClusterCoordinator API:
//   (a) acceptance with the expected outcome and state change,
//   (b) an idempotent repeat (NO_CHANGE where the engine has such a path),
//   (c) the kind-specific rejection path.
// No test relies on wall-clock ordering: the coordinator is driven by a
// ManualClock and the commit path is observed only through synchronous results.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

[[nodiscard]] ClusterId make_cluster(const char* text) { return *ClusterId::parse(text); }
[[nodiscard]] RackId make_rack(const char* text) { return *RackId::parse(text); }
[[nodiscard]] RackPublisherId make_publisher(const char* text) {
  return *RackPublisherId::parse(text);
}
[[nodiscard]] RackAgentBootId make_boot(const char* text) {
  return *RackAgentBootId::parse(text);
}

/// A started coordinator with an injectable persistence store and clock.
class Engine {
 public:
  explicit Engine(std::size_t max_racks = kMaxRacksPerCluster,
                  PersistenceStore* store = nullptr,
                  ReadinessContract contract = ReadinessContract::permissive())
      : store_(store) {
    if (store_ == nullptr) {
      store_ = &own_store_;
    }
    config_.cluster = make_cluster("cluster-1");
    config_.readiness_contract = contract;
    config_.max_racks = max_racks;
    coordinator_ = std::make_unique<ClusterCoordinator>(config_, store_, &clock_);
    const CoordinatorStartOutcome started = coordinator_->start();
    if (!started.ok) {
      CF_FAIL("coordinator failed to start: status " +
              std::string(to_string(started.status)) + " message " + started.error.message);
    }
  }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine() = default;

  [[nodiscard]] ClusterCoordinator& coordinator() { return *coordinator_; }
  [[nodiscard]] const ClusterId& cluster() const { return config_.cluster; }
  [[nodiscard]] ClusterState state() { return coordinator_->state_copy(); }
  [[nodiscard]] Timestamp now() const { return clock_.now(); }

  [[nodiscard]] EvidenceStamp evidence(std::int64_t ttl_millis = 60'000) const {
    return EvidenceStamp::make(EvidenceProvenance::Measured, clock_.now(), ttl_millis);
  }

  void advance_millis(std::int64_t delta) { clock_.advance_millis(delta); }

  [[nodiscard]] MutationResult submit(const MutationRequest& request) {
    return coordinator_->submit(request);
  }

  /// A request carrying the authority this coordinator currently expects.
  [[nodiscard]] MutationRequest base(MutationKind kind) {
    MutationRequest request;
    request.kind = kind;
    request.cluster = cluster();
    const ClusterState current = state();
    request.authority.cluster_epoch = current.epoch;
    request.authority.coordinator_epoch = current.coordinator_epoch;
    request.authority.topology_epoch = current.topology_epoch;
    request.authority.topology_generation = current.topology_generation;
    request.requested_at = now();
    request.evidence = evidence();
    return request;
  }

 private:
  CoordinatorConfig config_;
  ManualClock clock_;
  MemoryPersistenceStore own_store_;
  PersistenceStore* store_ = nullptr;
  std::unique_ptr<ClusterCoordinator> coordinator_;
};

struct RackFixture {
  RackId rack;
  RackPublisherId publisher;
  RackAgentBootId boot;
  RackGeneration generation;
  PublicationGeneration publication;
};

void declare_cluster(Engine& engine) {
  MutationRequest request = engine.base(MutationKind::DeclareCluster);
  request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract::permissive();
  const MutationResult result = engine.submit(request);
  CF_EXPECT_EQ(result.outcome, MutationOutcome::Accepted);
}

[[nodiscard]] MutationRequest rack_scoped(Engine& engine, MutationKind kind,
                                          const RackFixture& fixture) {
  MutationRequest request = engine.base(kind);
  request.authority.rack = fixture.rack;
  request.authority.publisher = fixture.publisher;
  request.authority.boot = fixture.boot;
  request.authority.rack_generation = fixture.generation;
  request.authority.publication = fixture.publication;
  return request;
}

[[nodiscard]] RackReference make_reference(const RackFixture& fixture,
                                           const EvidenceStamp& stamp) {
  RackReference reference;
  reference.rack = fixture.rack;
  reference.generation = fixture.generation;
  reference.rack_lifecycle = RackLifecycleState::Ready;
  reference.currentness = RackCurrentness::Current;
  reference.evidence = stamp;
  reference.publisher = fixture.publisher;
  reference.publication = fixture.publication;
  reference.boot = fixture.boot;
  reference.health = HealthState::Healthy;
  reference.origin_label = "rack-fabric:test";
  return reference;
}

[[nodiscard]] MutationRequest make_add_rack_request(Engine& engine, const RackFixture& fixture) {
  MutationRequest request = rack_scoped(engine, MutationKind::AddRack, fixture);
  request.rack_reference = make_reference(fixture, request.evidence);
  return request;
}

/// Adds one rack through ADD_RACK and requires acceptance.
[[nodiscard]] RackFixture add_rack(Engine& engine, const char* rack_text,
                                   const char* publisher_text, const char* boot_text,
                                   std::uint64_t generation, std::uint64_t publication) {
  RackFixture fixture;
  fixture.rack = make_rack(rack_text);
  fixture.publisher = make_publisher(publisher_text);
  fixture.boot = make_boot(boot_text);
  fixture.generation = RackGeneration::from_raw(generation);
  fixture.publication = PublicationGeneration::from_raw(publication);
  const MutationResult result = engine.submit(make_add_rack_request(engine, fixture));
  CF_EXPECT_EQ(result.outcome, MutationOutcome::Accepted);
  return fixture;
}

[[nodiscard]] MutationRequest make_link_request(Engine& engine, const char* id,
                                                const RackId& source, const RackId& destination,
                                                std::uint64_t generation) {
  MutationRequest request = engine.base(MutationKind::PublishInterRackLink);
  InterRackLink link;
  link.id = *InterRackLinkId::parse(id);
  link.source = source;
  link.destination = destination;
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.header.generation = DomainGeneration::from_raw(generation);
  link.header.evidence = request.evidence;
  link.topology_epoch = request.authority.topology_epoch;
  request.link = link;
  return request;
}

[[nodiscard]] MutationRequest make_placement_request(Engine& engine, const char* id,
                                                     PlacementDomainClass klass,
                                                     const std::vector<RackId>& members,
                                                     std::uint64_t generation) {
  MutationRequest request = engine.base(MutationKind::PublishPlacementDomain);
  PlacementDomain domain;
  domain.id = *PlacementDomainId::parse(id);
  domain.klass = klass;
  domain.racks = members;
  domain.header.generation = DomainGeneration::from_raw(generation);
  domain.header.evidence = request.evidence;
  request.placement_domain = domain;
  return request;
}

[[nodiscard]] MutationRequest make_constraint_request(Engine& engine, const char* id,
                                                      ConstraintKind kind,
                                                      const std::vector<RackId>& members,
                                                      std::uint64_t generation) {
  MutationRequest request = engine.base(MutationKind::PublishConstraint);
  ClusterConstraint constraint;
  constraint.id = *ConstraintId::parse(id);
  constraint.kind = kind;
  constraint.racks = members;
  constraint.statement = "declared infrastructure requirement";
  constraint.header.generation = DomainGeneration::from_raw(generation);
  constraint.header.evidence = request.evidence;
  request.constraint = constraint;
  return request;
}

[[nodiscard]] MutationResult publish_placement_domain(Engine& engine, const char* id,
                                                      PlacementDomainClass klass,
                                                      const std::vector<RackId>& members,
                                                      std::uint64_t generation) {
  return engine.submit(make_placement_request(engine, id, klass, members, generation));
}

/// Composes a readable difference between two canonical states.
[[nodiscard]] std::string describe_difference(const ClusterState& actual,
                                              const ClusterState& expected) {
  std::string diff;
  const auto note = [&diff](const std::string& text) {
    if (!diff.empty()) {
      diff += "; ";
    }
    diff += text;
  };
  if (actual.racks != expected.racks) {
    note("racks differ (actual " + std::to_string(actual.racks.size()) + " entries, expected " +
         std::to_string(expected.racks.size()) + " entries)");
  }
  if (actual.links != expected.links) {
    note("links differ");
  }
  if (actual.placement_domains != expected.placement_domains) {
    note("placement_domains differ");
  }
  if (actual.capacity_domains != expected.capacity_domains) {
    note("capacity_domains differ");
  }
  if (actual.failure_domains != expected.failure_domains) {
    note("failure_domains differ");
  }
  if (actual.network_domains != expected.network_domains) {
    note("network_domains differ");
  }
  if (actual.storage_domains != expected.storage_domains) {
    note("storage_domains differ");
  }
  if (actual.power_domains != expected.power_domains) {
    note("power_domains differ");
  }
  if (actual.cooling_domains != expected.cooling_domains) {
    note("cooling_domains differ");
  }
  if (actual.link_domains != expected.link_domains) {
    note("link_domains differ");
  }
  if (actual.constraints != expected.constraints) {
    note("constraints differ");
  }
  if (actual.withdrawn_racks != expected.withdrawn_racks) {
    note("withdrawn_racks differ");
  }
  if (actual.retired_racks != expected.retired_racks) {
    note("retired_racks differ");
  }
  if (actual.fenced_authorities != expected.fenced_authorities) {
    note("fenced_authorities differ");
  }
  if (actual.epoch != expected.epoch) {
    note("cluster epoch " + actual.epoch.str() + " != " + expected.epoch.str());
  }
  if (actual.coordinator_epoch != expected.coordinator_epoch) {
    note("coordinator epoch " + actual.coordinator_epoch.str() + " != " +
         expected.coordinator_epoch.str());
  }
  if (actual.generation != expected.generation) {
    note("cluster generation " + actual.generation.str() + " != " + expected.generation.str());
  }
  if (actual.membership_generation != expected.membership_generation) {
    note("membership generation " + actual.membership_generation.str() + " != " +
         expected.membership_generation.str());
  }
  if (actual.topology_epoch != expected.topology_epoch) {
    note("topology epoch " + actual.topology_epoch.str() + " != " + expected.topology_epoch.str());
  }
  if (actual.topology_generation != expected.topology_generation) {
    note("topology generation " + actual.topology_generation.str() + " != " +
         expected.topology_generation.str());
  }
  if (actual.connectivity_generation != expected.connectivity_generation) {
    note("connectivity generation " + actual.connectivity_generation.str() + " != " +
         expected.connectivity_generation.str());
  }
  if (actual.health_generation != expected.health_generation) {
    note("health generation " + actual.health_generation.str() + " != " +
         expected.health_generation.str());
  }
  if (actual.constraint_generation != expected.constraint_generation) {
    note("constraint generation " + actual.constraint_generation.str() + " != " +
         expected.constraint_generation.str());
  }
  if (!(actual.domain_generations == expected.domain_generations)) {
    note("domain generations differ");
  }
  if (actual.snapshot_generation != expected.snapshot_generation) {
    note("snapshot generation " + actual.snapshot_generation.str() + " != " +
         expected.snapshot_generation.str());
  }
  if (actual.publication_generation != expected.publication_generation) {
    note("publication generation " + actual.publication_generation.str() + " != " +
         expected.publication_generation.str());
  }
  if (actual.lifecycle != expected.lifecycle) {
    note("lifecycle " + std::string(to_string(actual.lifecycle)) + " != " +
         std::string(to_string(expected.lifecycle)));
  }
  if (actual.readiness_contract != expected.readiness_contract) {
    note("readiness contract differs");
  }
  if (!(actual.topology_record == expected.topology_record)) {
    note("topology record differs");
  }
  if (actual.last_mutation_at != expected.last_mutation_at) {
    note("last_mutation_at " + std::to_string(actual.last_mutation_at.millis()) + " != " +
         std::to_string(expected.last_mutation_at.millis()));
  }
  if (actual.declared_at != expected.declared_at) {
    note("declared_at differs");
  }
  return diff;
}

// ---------------------------------------------------------------------------
// Persistence and hook doubles
// ---------------------------------------------------------------------------

/// Records every save and can be made to fail from a chosen call onwards.
class FailingStore final : public PersistenceStore {
 public:
  [[nodiscard]] SaveOutcome save(const PersistedState& state) override {
    ++save_calls;
    if (fail_from != 0 && save_calls >= fail_from) {
      SaveOutcome outcome;
      outcome.ok = false;
      outcome.status = PersistenceStatus::IoError;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                            "injected_io_error",
                                            "injected persistence failure");
      return outcome;
    }
    saved = state;
    have_saved = true;
    SaveOutcome outcome;
    outcome.ok = true;
    outcome.status = PersistenceStatus::Ok;
    outcome.bytes_written = 1;
    return outcome;
  }

  [[nodiscard]] LoadOutcome load() override {
    LoadOutcome outcome;
    if (!have_saved) {
      outcome.status = PersistenceStatus::MissingFile;
      return outcome;
    }
    outcome.status = PersistenceStatus::Ok;
    outcome.state = saved;
    return outcome;
  }

  void discard_temporary() noexcept override {}

  std::uint64_t save_calls = 0;
  /// 0 disables failure; otherwise the first save call that must fail.
  std::uint64_t fail_from = 0;
  bool have_saved = false;
  PersistedState saved;
};

class RecordingHook final : public CommitHook {
 public:
  void before_candidate(MutationKind kind) override {
    events.push_back("before_candidate:" + std::string(to_string(kind)));
  }
  void before_commit(MutationKind kind) override {
    events.push_back("before_commit:" + std::string(to_string(kind)));
  }
  void after_publish(MutationKind kind) override {
    events.push_back("after_publish:" + std::string(to_string(kind)));
  }

  [[nodiscard]] std::string describe() const {
    std::string out;
    for (const std::string& event : events) {
      if (!out.empty()) {
        out += ",";
      }
      out += event;
    }
    return out;
  }

  [[nodiscard]] bool contains(const std::string& prefix) const {
    for (const std::string& event : events) {
      if (event.rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> events;
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle guard
// ---------------------------------------------------------------------------

CF_TEST(mutation_submit_before_start_is_refused_not_hung) {
  // A coordinator that was never started has no commit thread, so a mutation
  // must be refused with a typed reason rather than waiting forever for a
  // result that no thread will produce. The same holds after stop().
  MemoryPersistenceStore store;
  ClusterCoordinator coordinator{CoordinatorConfig{}, &store};

  MutationRequest request;
  request.kind = MutationKind::DeclareCluster;
  request.cluster = *ClusterId::parse("not-started-cluster");
  request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract::permissive();
  request.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, default_clock().now(), 30'000);

  const MutationResult before_start = coordinator.submit(request);
  CF_EXPECT_EQ(before_start.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(before_start.reason, RejectionReason::NotReady);
  CF_EXPECT_EQ(before_start.error.reason, std::string("not_running"));

  const CoordinatorStartOutcome started = coordinator.start();
  CF_EXPECT(started.ok);
  CF_EXPECT_EQ(coordinator.submit(request).outcome, MutationOutcome::Accepted);
  coordinator.stop();

  const MutationResult after_stop = coordinator.submit(request);
  CF_EXPECT_EQ(after_stop.outcome, MutationOutcome::Rejected);
  CF_EXPECT(after_stop.reason == RejectionReason::NotReady ||
            after_stop.reason == RejectionReason::ShuttingDown);
}

// ---------------------------------------------------------------------------
// One case per MutationKind
// ---------------------------------------------------------------------------

CF_TEST(mutation_declare_cluster_accept_repeat_reject) {
  Engine engine;
  CF_EXPECT(!engine.state().epoch.known());

  MutationRequest request = engine.base(MutationKind::DeclareCluster);
  request.authority.cluster_epoch = ClusterEpoch::from_raw(7);
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract::permissive();

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState declared = engine.state();
  CF_EXPECT_EQ(declared.id, engine.cluster());
  CF_EXPECT_EQ(declared.epoch, ClusterEpoch::from_raw(7));
  // DECLARE_CLUSTER initialises every counter to 1 and the commit path then
  // advances the cluster generation once, so the first authoritative cluster
  // generation is 2.
  CF_EXPECT_EQ(declared.generation, ClusterGeneration::from_raw(2));
  CF_EXPECT_EQ(declared.membership_generation, MembershipGeneration::from_raw(1));
  CF_EXPECT_EQ(declared.topology_epoch, TopologyEpoch::from_raw(1));
  CF_EXPECT(declared.domain_generations.all_known());
  CF_EXPECT_EQ(declared.lifecycle, ClusterLifecycle::Declared);

  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::NoChange);

  MutationRequest stale = request;
  stale.authority.cluster_epoch = ClusterEpoch::from_raw(6);
  const MutationResult rejected = engine.submit(stale);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::StaleClusterEpoch);

  MutationRequest malformed = request;
  malformed.cluster = ClusterId{};
  const MutationResult malformed_result = engine.submit(malformed);
  CF_EXPECT_EQ(malformed_result.reason, RejectionReason::Malformed);
}

CF_TEST(mutation_register_rack_publisher_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);

  const RackFixture fixture{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-1"), RackGeneration::from_raw(1),
                            PublicationGeneration::from_raw(1)};

  MutationRequest request = rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState registered = engine.state();
  const auto it = registered.racks.find(fixture.rack);
  CF_EXPECT(it != registered.racks.end());
  CF_EXPECT_EQ(it->second.membership, RackMembershipState::Unknown);
  CF_EXPECT(!it->second.authoritative_current);

  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::NoChange);

  MutationRequest other_publisher = request;
  other_publisher.authority.publisher = make_publisher("publisher-2");
  const MutationResult rejected = engine.submit(other_publisher);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Conflict);
}

CF_TEST(mutation_add_rack_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  const ClusterState added = engine.state();
  const auto it = added.racks.find(fixture.rack);
  CF_EXPECT(it != added.racks.end());
  CF_EXPECT_EQ(it->second.membership, RackMembershipState::Active);
  CF_EXPECT(it->second.authoritative_current);
  CF_EXPECT_EQ(it->second.membership_generation, added.membership_generation);

  const MutationResult repeat = engine.submit(make_add_rack_request(engine, fixture));

  MutationRequest bump = rack_scoped(engine, MutationKind::UpdateRackGeneration, fixture);
  bump.expected_rack_generation = RackGeneration::from_raw(1);
  bump.new_rack_generation = RackGeneration::from_raw(2);
  bump.authority.publication = PublicationGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(bump).outcome, MutationOutcome::Accepted);

  MutationRequest stale = make_add_rack_request(engine, fixture);
  stale.authority.publication = PublicationGeneration::from_raw(9);
  stale.rack_reference.publication = PublicationGeneration::from_raw(9);
  const MutationResult rejected = engine.submit(stale);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::StaleRackGeneration);

  // docs/protocol.md documents that a byte-identical rack republication under
  // the same generation is answered with NO_CHANGE and reason "idempotent"
  // rather than applied twice.
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::NoChange);
}

CF_TEST(mutation_update_rack_generation_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::UpdateRackGeneration, fixture);
  request.expected_rack_generation = RackGeneration::from_raw(1);
  request.new_rack_generation = RackGeneration::from_raw(2);
  request.authority.publication = PublicationGeneration::from_raw(2);
  request.rack_reference.rack_lifecycle = RackLifecycleState::Ready;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState updated = engine.state();
  CF_EXPECT_EQ(updated.racks.at(fixture.rack).reference.generation, RackGeneration::from_raw(2));
  CF_EXPECT(updated.membership_generation.value() > 1u);

  MutationRequest repeat = request;
  repeat.expected_rack_generation = RackGeneration::from_raw(2);
  repeat.new_rack_generation = RackGeneration::from_raw(2);
  repeat.authority.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest conflict = request;
  conflict.expected_rack_generation = RackGeneration::from_raw(1);
  conflict.new_rack_generation = RackGeneration::from_raw(3);
  conflict.authority.publication = PublicationGeneration::from_raw(4);
  const MutationResult rejected = engine.submit(conflict);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Conflict);
}

CF_TEST(mutation_mark_rack_unavailable_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  request.authority.publication = PublicationGeneration::from_raw(2);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState unavailable = engine.state();
  CF_EXPECT_EQ(unavailable.racks.at(fixture.rack).membership, RackMembershipState::Unavailable);
  CF_EXPECT(!unavailable.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT_EQ(unavailable.racks.at(fixture.rack).membership_generation,
               unavailable.membership_generation);

  MutationRequest repeat = request;
  repeat.authority.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown = request;
  unknown.authority.rack = make_rack("rack-missing");
  unknown.rack_reference.rack = make_rack("rack-missing");
  const MutationResult rejected = engine.submit(unknown);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_withdraw_rack_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::WithdrawRack, fixture);
  request.authority.publication = PublicationGeneration::from_raw(2);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState withdrawn = engine.state();
  CF_EXPECT_EQ(withdrawn.racks.at(fixture.rack).membership, RackMembershipState::Withdrawn);
  CF_EXPECT_EQ(withdrawn.withdrawn_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(withdrawn.withdrawn_racks.front(), fixture.rack);

  MutationRequest repeat = request;
  repeat.authority.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown = request;
  unknown.authority.rack = make_rack("rack-missing");
  const MutationResult rejected = engine.submit(unknown);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_retire_rack_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::RetireRack, fixture);
  request.authority.publication = PublicationGeneration::from_raw(2);
  request.reason = "operator_retired";
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState retired = engine.state();
  CF_EXPECT_EQ(retired.racks.at(fixture.rack).membership, RackMembershipState::Retired);
  CF_EXPECT_EQ(retired.retired_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(retired.retired_racks.front().rack, fixture.rack);
  CF_EXPECT_EQ(retired.retired_racks.front().last_generation, RackGeneration::from_raw(1));
  CF_EXPECT_EQ(retired.retired_racks.front().reason, std::string("operator_retired"));

  MutationRequest repeat = request;
  repeat.authority.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  RackFixture readmitted = fixture;
  readmitted.generation = RackGeneration::from_raw(2);
  readmitted.publication = PublicationGeneration::from_raw(4);
  const MutationResult rejected = engine.submit(make_add_rack_request(engine, readmitted));
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Retired);
}

CF_TEST(mutation_recover_rack_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest unavailable = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  unavailable.authority.publication = PublicationGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(unavailable).outcome, MutationOutcome::Accepted);

  RackFixture recovered = fixture;
  recovered.boot = make_boot("boot-2");
  recovered.publication = PublicationGeneration::from_raw(3);
  MutationRequest request = rack_scoped(engine, MutationKind::RecoverRack, recovered);
  request.rack_reference = make_reference(recovered, request.evidence);

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState state = engine.state();
  CF_EXPECT_EQ(state.racks.at(fixture.rack).membership, RackMembershipState::Active);
  CF_EXPECT_EQ(state.racks.at(fixture.rack).reference.boot, recovered.boot);
  CF_EXPECT(state.racks.at(fixture.rack).authoritative_current);

  // RECOVER_RACK has no idempotent NO_CHANGE path: every accepted recovery
  // republishes evidence and advances the membership generation, so a repeat
  // is accepted again instead of reported as NO_CHANGE.
  RackFixture repeat_fixture = recovered;
  repeat_fixture.publication = PublicationGeneration::from_raw(4);
  MutationRequest repeat = rack_scoped(engine, MutationKind::RecoverRack, repeat_fixture);
  repeat.rack_reference = make_reference(repeat_fixture, repeat.evidence);
  const MutationResult repeat_result = engine.submit(repeat);
  CF_EXPECT_EQ(repeat_result.outcome, MutationOutcome::Accepted);
  CF_EXPECT(repeat_result.membership_generation.value() >
            accepted.membership_generation.value());

  MutationRequest no_boot = repeat;
  no_boot.authority.boot.reset();
  const MutationResult rejected = engine.submit(no_boot);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::NotAuthorized);

  MutationRequest unknown = no_boot;
  unknown.authority.rack = make_rack("rack-missing");
  unknown.rack_reference.rack = make_rack("rack-missing");
  CF_EXPECT_EQ(engine.submit(unknown).reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_publish_inter_rack_link_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture second = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);

  const MutationResult accepted =
      engine.submit(make_link_request(engine, "link-a-b", first.rack, second.rack, 1));
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState linked = engine.state();
  CF_EXPECT_EQ(linked.links.size(), std::size_t{1});
  CF_EXPECT(linked.topology_generation.value() > 1u);
  CF_EXPECT(linked.connectivity_generation.value() > 1u);

  MutationRequest repeat = engine.base(MutationKind::PublishInterRackLink);
  repeat.link = linked.links.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest self = make_link_request(engine, "link-self", first.rack, first.rack, 1);
  const MutationResult rejected = engine.submit(self);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::InvalidRelationship);
}

CF_TEST(mutation_withdraw_inter_rack_link_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture second = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);
  CF_EXPECT_EQ(engine.submit(make_link_request(engine, "link-a-b", first.rack, second.rack, 1))
                   .outcome,
               MutationOutcome::Accepted);

  MutationRequest request = engine.base(MutationKind::WithdrawInterRackLink);
  request.link_id = *InterRackLinkId::parse("link-a-b");
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  CF_EXPECT_EQ(engine.state().links.size(), std::size_t{0});
  CF_EXPECT(accepted.topology_generation.value() > 1u);

  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::NoChange);

  MutationRequest missing = engine.base(MutationKind::WithdrawInterRackLink);
  const MutationResult rejected = engine.submit(missing);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Malformed);
}

CF_TEST(mutation_publish_placement_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  const MutationResult accepted =
      publish_placement_domain(engine, "placement-1", PlacementDomainClass::AvailabilityDomain,
                               {rack.rack}, 1);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.placement_domains.size(), std::size_t{1});
  CF_EXPECT(published.domain_generations.placement.value() > 1u);

  MutationRequest repeat = engine.base(MutationKind::PublishPlacementDomain);
  repeat.placement_domain = published.placement_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_class = repeat;
  unknown_class.placement_domain->klass = PlacementDomainClass::Unknown;
  unknown_class.placement_domain->header.generation = DomainGeneration::from_raw(2);
  const MutationResult rejected = engine.submit(unknown_class);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::InvalidDomain);
}

CF_TEST(mutation_publish_capacity_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
  CapacityDomain domain;
  domain.id = *CapacityDomainId::parse("capacity-1");
  domain.klass = CapacityDomainClass::RackGroup;
  domain.racks = {rack.rack};
  CapacityQuantity quantity;
  quantity.unit = "bytes";
  quantity.value = 4096.0;
  quantity.provenance = EvidenceProvenance::Measured;
  domain.quantities = {quantity};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.capacity_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.capacity_domains.size(), std::size_t{1});

  MutationRequest repeat = request;
  repeat.capacity_domain = published.capacity_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest duplicate_unit = request;
  duplicate_unit.capacity_domain->header.generation = DomainGeneration::from_raw(2);
  CapacityQuantity second_quantity;
  second_quantity.unit = "bytes";
  second_quantity.value = 1.0;
  second_quantity.provenance = EvidenceProvenance::Reported;
  duplicate_unit.capacity_domain->quantities.push_back(second_quantity);
  const MutationResult rejected = engine.submit(duplicate_unit);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Conflict);
}

CF_TEST(mutation_publish_failure_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishFailureDomain);
  FailureDomain domain;
  domain.id = *FailureDomainId::parse("failure-1");
  domain.klass = FailureDomainClass::Row;
  domain.racks = {rack.rack};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.failure_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.failure_domains.size(), std::size_t{1});

  MutationRequest repeat = request;
  repeat.failure_domain = published.failure_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_class = request;
  unknown_class.failure_domain->klass = FailureDomainClass::Unknown;
  unknown_class.failure_domain->header.generation = DomainGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(unknown_class).reason, RejectionReason::InvalidDomain);
}

CF_TEST(mutation_publish_network_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishNetworkDomain);
  NetworkDomain domain;
  domain.id = *NetworkDomainId::parse("network-1");
  domain.connectivity = ConnectivityClass::DirectFabric;
  domain.racks = {rack.rack};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.network_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.network_domains.size(), std::size_t{1});

  MutationRequest repeat = request;
  repeat.network_domain = published.network_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_connectivity = request;
  unknown_connectivity.network_domain->connectivity = ConnectivityClass::Unknown;
  unknown_connectivity.network_domain->header.generation = DomainGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(unknown_connectivity).reason, RejectionReason::InvalidDomain);
}

CF_TEST(mutation_publish_storage_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishStorageDomain);
  StorageDomain domain;
  domain.id = *StorageDomainId::parse("storage-1");
  domain.racks = {rack.rack};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.storage_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.storage_domains.size(), std::size_t{1});

  MutationRequest repeat = request;
  repeat.storage_domain = published.storage_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_rack = request;
  unknown_rack.storage_domain->header.generation = DomainGeneration::from_raw(2);
  unknown_rack.storage_domain->racks = {make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown_rack).reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_publish_power_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishPowerDomain);
  PowerDomain domain;
  domain.id = *PowerDomainId::parse("power-1");
  domain.racks = {rack.rack};
  domain.parent = *PowerDomainId::parse("power-root");
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.power_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.power_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(published.power_domains.begin()->second.parent, domain.parent);

  MutationRequest repeat = request;
  repeat.power_domain = published.power_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_rack = request;
  unknown_rack.power_domain->header.generation = DomainGeneration::from_raw(2);
  unknown_rack.power_domain->racks = {make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown_rack).reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_publish_cooling_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishCoolingDomain);
  CoolingDomain domain;
  domain.id = *CoolingDomainId::parse("cooling-1");
  domain.racks = {rack.rack};
  domain.parent = *CoolingDomainId::parse("cooling-root");
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.cooling_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.cooling_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(published.cooling_domains.begin()->second.parent, domain.parent);

  MutationRequest repeat = request;
  repeat.cooling_domain = published.cooling_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_rack = request;
  unknown_rack.cooling_domain->header.generation = DomainGeneration::from_raw(2);
  unknown_rack.cooling_domain->racks = {make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown_rack).reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_publish_link_domain_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::PublishLinkDomain);
  LinkDomain domain;
  domain.id = *LinkDomainId::parse("link-domain-1");
  domain.racks = {rack.rack};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.link_domain = domain;

  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.link_domains.size(), std::size_t{1});

  MutationRequest repeat = request;
  repeat.link_domain = published.link_domains.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_rack = request;
  unknown_rack.link_domain->header.generation = DomainGeneration::from_raw(2);
  unknown_rack.link_domain->racks = {make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown_rack).reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_publish_constraint_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = make_constraint_request(engine, "constraint-1",
                                                    ConstraintKind::PlacementScope, {rack.rack}, 1);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.constraints.size(), std::size_t{1});
  CF_EXPECT(published.constraint_generation.value() > 1u);

  MutationRequest repeat = request;
  repeat.constraint = published.constraints.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest unknown_kind = request;
  unknown_kind.constraint->kind = ConstraintKind::Unknown;
  unknown_kind.constraint->header.generation = DomainGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(unknown_kind).reason, RejectionReason::InvalidDomain);
}

CF_TEST(mutation_publish_health_currentness_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::PublishHealthCurrentness, fixture);
  request.authority.publication = PublicationGeneration::from_raw(2);
  request.health = HealthState::Degraded;
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.racks.at(fixture.rack).reference.health, HealthState::Degraded);
  CF_EXPECT(published.health_generation.value() > 1u);

  MutationRequest repeat = request;
  repeat.authority.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest wrong_boot = request;
  wrong_boot.authority.boot = make_boot("boot-2");
  wrong_boot.authority.publication = PublicationGeneration::from_raw(4);
  const MutationResult rejected = engine.submit(wrong_boot);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::NotAuthorized);
}

CF_TEST(mutation_withdraw_evidence_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = rack_scoped(engine, MutationKind::WithdrawEvidence, fixture);
  request.evidence_selector = "rack_health";
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState withdrawn = engine.state();
  const auto it = withdrawn.racks.find(fixture.rack);
  CF_EXPECT(it != withdrawn.racks.end());
  CF_EXPECT(it->second.reference.evidence.requires_revalidation());
  CF_EXPECT(!it->second.authoritative_current);
  CF_EXPECT(withdrawn.health_generation.value() > 1u);

  // WITHDRAW_EVIDENCE has no idempotent NO_CHANGE path: every accepted
  // withdrawal re-marks the evidence and advances the health generation.
  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::Accepted);
  CF_EXPECT(repeat.health_generation.value() > accepted.health_generation.value());

  MutationRequest unknown_selector = request;
  unknown_selector.evidence_selector = "not_a_selector";
  const MutationResult rejected = engine.submit(unknown_selector);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::Malformed);
}

CF_TEST(mutation_supersede_topology_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture second = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);
  CF_EXPECT_EQ(engine.submit(make_link_request(engine, "link-a-b", first.rack, second.rack, 1))
                   .outcome,
               MutationOutcome::Accepted);

  MutationRequest request = engine.base(MutationKind::SupersedeTopology);
  request.target_topology_epoch = TopologyEpoch::from_raw(2);
  request.topology_reason = "link_superseded";
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState superseded = engine.state();
  CF_EXPECT_EQ(superseded.topology_epoch, TopologyEpoch::from_raw(2));
  CF_EXPECT(superseded.topology_generation.value() > 1u);
  CF_EXPECT_EQ(superseded.topology_record.reason, std::string("link_superseded"));
  CF_EXPECT_EQ(superseded.topology_record.epoch, superseded.topology_epoch);
  CF_EXPECT_EQ(superseded.links.size(), std::size_t{1});
  for (const auto& entry : superseded.links) {
    CF_EXPECT(entry.second.header.evidence.requires_revalidation());
    CF_EXPECT_EQ(entry.second.reachability, Reachability::Unknown);
    CF_EXPECT_EQ(entry.second.health, HealthState::Unknown);
  }

  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(repeat.reason, RejectionReason::StaleTopologyEpoch);

  MutationRequest missing = engine.base(MutationKind::SupersedeTopology);
  CF_EXPECT_EQ(engine.submit(missing).reason, RejectionReason::Malformed);
}

CF_TEST(mutation_publish_snapshot_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);

  MutationRequest request = engine.base(MutationKind::PublishSnapshot);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  CF_EXPECT(accepted.snapshot_generation.value() > 1u);
  CF_EXPECT(accepted.publication_generation.value() > 1u);

  // PUBLISH_SNAPSHOT advances the snapshot and publication generations on
  // every call, so a repeat is accepted rather than reported NO_CHANGE.
  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::Accepted);
  CF_EXPECT(repeat.snapshot_generation.value() > accepted.snapshot_generation.value());

  MutationRequest wrong_cluster = request;
  wrong_cluster.cluster = make_cluster("cluster-other");
  const MutationResult rejected = engine.submit(wrong_cluster);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::WrongCluster);
}

CF_TEST(mutation_revalidate_recovered_state_accept_repeat_reject) {
  Engine empty;
  declare_cluster(empty);
  MutationRequest nothing_request = empty.base(MutationKind::RevalidateRecoveredState);
  const MutationResult nothing = empty.submit(nothing_request);
  CF_EXPECT_EQ(nothing.outcome, MutationOutcome::NoChange);

  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  MutationRequest ready = engine.base(MutationKind::RevalidateRecoveredState);
  CF_EXPECT_EQ(engine.submit(ready).outcome, MutationOutcome::Accepted);

  MutationRequest withdraw = rack_scoped(engine, MutationKind::WithdrawEvidence, fixture);
  withdraw.evidence_selector = "rack_evidence";
  CF_EXPECT_EQ(engine.submit(withdraw).outcome, MutationOutcome::Accepted);

  MutationRequest blocked = engine.base(MutationKind::RevalidateRecoveredState);
  const MutationResult revalidation = engine.submit(blocked);
  CF_EXPECT_EQ(revalidation.outcome, MutationOutcome::RevalidationRequired);
  CF_EXPECT_EQ(revalidation.reason, RejectionReason::Conflict);
  CF_EXPECT_EQ(revalidation.error.reason, std::string("revalidation_incomplete"));

  MutationRequest unknown = engine.base(MutationKind::RevalidateRecoveredState);
  unknown.racks = {make_rack("rack-missing")};
  const MutationResult rejected = engine.submit(unknown);
  CF_EXPECT_EQ(rejected.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::UnknownRack);
}

CF_TEST(mutation_retire_cluster_accept_repeat_reject) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest request = engine.base(MutationKind::RetireCluster);
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState retired = engine.state();
  CF_EXPECT_EQ(retired.lifecycle, ClusterLifecycle::Retired);
  CF_EXPECT(!retired.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT(!accepts_mutations(retired.lifecycle));

  // A second RETIRE_CLUSTER is short-circuited by authority validation: a
  // retired cluster accepts no further mutations at all.
  const MutationResult repeat = engine.submit(request);
  CF_EXPECT_EQ(repeat.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(repeat.reason, RejectionReason::Retired);

  MutationRequest after = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  after.authority.publication = PublicationGeneration::from_raw(5);
  CF_EXPECT_EQ(engine.submit(after).reason, RejectionReason::Retired);
}

// ---------------------------------------------------------------------------
// Authority validation order
// ---------------------------------------------------------------------------

CF_TEST(mutation_authority_validation_order_is_documented_order) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture other = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);
  CF_EXPECT(engine.state().racks.at(fixture.rack).authoritative_current);

  // Raise the authoritative rack generation to 2 so a publication carrying
  // generation 1 violates the rack-generation rule.
  MutationRequest bump = rack_scoped(engine, MutationKind::UpdateRackGeneration, fixture);
  bump.expected_rack_generation = RackGeneration::from_raw(1);
  bump.new_rack_generation = RackGeneration::from_raw(2);
  bump.authority.publication = PublicationGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(bump).outcome, MutationOutcome::Accepted);

  // Fence the incarnation that owns rack B, then place a barrier mutation
  // behind the fence in the same FIFO queue so the fence is applied before the
  // layered requests below are evaluated.
  CF_EXPECT(engine.coordinator().fence_boot(other.boot, "test_fence"));
  CF_EXPECT_EQ(engine.submit(engine.base(MutationKind::PublishSnapshot)).outcome,
               MutationOutcome::Accepted);
  CF_EXPECT(engine.state().is_boot_fenced(other.boot));

  const auto layered = [&engine, &fixture]() {
    MutationRequest request = make_add_rack_request(engine, fixture);
    request.authority.rack_generation = RackGeneration::from_raw(1);
    request.rack_reference.generation = RackGeneration::from_raw(1);
    request.authority.publication = PublicationGeneration::from_raw(1);
    request.rack_reference.publication = PublicationGeneration::from_raw(1);
    request.rack_reference.health = HealthState::Degraded;
    return request;
  };

  // 1. cluster identity -> REJECT_MALFORMED.
  MutationRequest invalid_identity = layered();
  invalid_identity.cluster = ClusterId{};
  CF_EXPECT_EQ(engine.submit(invalid_identity).reason, RejectionReason::Malformed);

  // 2. wrong cluster wins over every later rule.
  MutationRequest wrong_cluster = layered();
  wrong_cluster.cluster = make_cluster("cluster-other");
  wrong_cluster.authority.cluster_epoch = ClusterEpoch::from_raw(99);
  wrong_cluster.authority.coordinator_epoch = CoordinatorEpoch::from_raw(99);
  wrong_cluster.authority.publisher = make_publisher("publisher-2");
  wrong_cluster.authority.boot = other.boot;
  CF_EXPECT_EQ(engine.submit(wrong_cluster).reason, RejectionReason::WrongCluster);

  // 3. cluster epoch wins over coordinator epoch, boot, publisher, publication
  //    and rack generation.
  MutationRequest stale_cluster = layered();
  stale_cluster.authority.cluster_epoch = ClusterEpoch::from_raw(99);
  stale_cluster.authority.coordinator_epoch = CoordinatorEpoch::from_raw(99);
  stale_cluster.authority.publisher = make_publisher("publisher-2");
  stale_cluster.authority.boot = other.boot;
  CF_EXPECT_EQ(engine.submit(stale_cluster).reason, RejectionReason::StaleClusterEpoch);

  // 4. coordinator epoch wins over boot, publisher and generation.
  MutationRequest stale_coordinator = layered();
  stale_coordinator.authority.coordinator_epoch = CoordinatorEpoch::from_raw(99);
  stale_coordinator.authority.publisher = make_publisher("publisher-2");
  stale_coordinator.authority.boot = other.boot;
  CF_EXPECT_EQ(engine.submit(stale_coordinator).reason,
               RejectionReason::StaleCoordinatorEpoch);

  // 5. a fenced boot wins over publisher ownership and publication monotonicity.
  MutationRequest fenced_boot = layered();
  fenced_boot.authority.boot = other.boot;
  fenced_boot.authority.publisher = make_publisher("publisher-2");
  CF_EXPECT_EQ(engine.submit(fenced_boot).reason, RejectionReason::StaleRackBoot);

  // 6. publisher ownership wins over publication monotonicity and generation.
  MutationRequest wrong_publisher = layered();
  wrong_publisher.authority.publisher = make_publisher("publisher-2");
  wrong_publisher.rack_reference.publisher = make_publisher("publisher-2");
  CF_EXPECT_EQ(engine.submit(wrong_publisher).reason, RejectionReason::NotAuthorized);

  // 7. publication monotonicity wins over rack generation.
  MutationRequest stale_publication = layered();
  CF_EXPECT_EQ(engine.submit(stale_publication).reason, RejectionReason::StalePublication);

  // 8. rack generation, once the publication counter is fresh.
  MutationRequest stale_generation = layered();
  stale_generation.authority.publication = PublicationGeneration::from_raw(50);
  stale_generation.rack_reference.publication = PublicationGeneration::from_raw(50);
  CF_EXPECT_EQ(engine.submit(stale_generation).reason,
               RejectionReason::StaleRackGeneration);

  // 9. link and domain validity are reached only after authority.
  MutationRequest stale_epoch_link =
      make_link_request(engine, "link-self", fixture.rack, fixture.rack, 1);
  stale_epoch_link.authority.cluster_epoch = ClusterEpoch::from_raw(99);
  CF_EXPECT_EQ(engine.submit(stale_epoch_link).reason, RejectionReason::StaleClusterEpoch);

  MutationRequest self_link =
      make_link_request(engine, "link-self", make_rack("rack-missing"),
                        make_rack("rack-missing"), 1);
  CF_EXPECT_EQ(engine.submit(self_link).reason, RejectionReason::InvalidRelationship);
}

// ---------------------------------------------------------------------------
// Rejection reason coverage
// ---------------------------------------------------------------------------

CF_TEST(mutation_every_engine_rejection_reason_is_reachable) {
  // REJECT_NOT_READY: the identity matches but no epoch was declared.
  {
    Engine engine;
    const MutationResult result = engine.submit(engine.base(MutationKind::PublishSnapshot));
    CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
    CF_EXPECT_EQ(result.reason, RejectionReason::NotReady);
  }

  // REJECT_LIMIT_EXCEEDED: the configured rack bound.
  {
    Engine engine(1);
    declare_cluster(engine);
    const RackFixture first{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-1"), RackGeneration::from_raw(1),
                            PublicationGeneration::from_raw(1)};
    const RackFixture second{make_rack("rack-b"), make_publisher("publisher-1"),
                             make_boot("boot-2"), RackGeneration::from_raw(1),
                             PublicationGeneration::from_raw(1)};
    CF_EXPECT_EQ(engine.submit(rack_scoped(engine, MutationKind::RegisterRackPublisher, first))
                     .outcome,
                 MutationOutcome::Accepted);
    const MutationResult result =
        engine.submit(rack_scoped(engine, MutationKind::RegisterRackPublisher, second));
    CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
    CF_EXPECT_EQ(result.reason, RejectionReason::LimitExceeded);
  }

  Engine engine;
  declare_cluster(engine);
  const RackFixture first{make_rack("rack-a"), make_publisher("publisher-1"),
                          make_boot("boot-1"), RackGeneration::from_raw(2),
                          PublicationGeneration::from_raw(1)};
  const RackFixture second{make_rack("rack-b"), make_publisher("publisher-1"),
                           make_boot("boot-2"), RackGeneration::from_raw(1),
                           PublicationGeneration::from_raw(1)};

  // REJECT_WRONG_CLUSTER, REJECT_MALFORMED and the epoch rules.
  {
    MutationRequest request = engine.base(MutationKind::PublishSnapshot);
    request.cluster = make_cluster("cluster-other");
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::WrongCluster);

    request = engine.base(MutationKind::PublishSnapshot);
    request.authority.cluster_epoch = ClusterEpoch::from_raw(99);
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::StaleClusterEpoch);

    request = engine.base(MutationKind::PublishSnapshot);
    request.authority.coordinator_epoch = CoordinatorEpoch::from_raw(99);
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::StaleCoordinatorEpoch);

    request = engine.base(MutationKind::Unknown);
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::Malformed);
  }

  // REJECT_STALE_TOPOLOGY_EPOCH: a supersession at or below the current epoch.
  {
    MutationRequest request = engine.base(MutationKind::SupersedeTopology);
    request.target_topology_epoch = TopologyEpoch::from_raw(99);
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
    MutationRequest older = engine.base(MutationKind::SupersedeTopology);
    older.target_topology_epoch = engine.state().topology_epoch;
    CF_EXPECT_EQ(engine.submit(older).reason, RejectionReason::StaleTopologyEpoch);
  }

  // REJECT_UNKNOWN_RACK: a domain referencing an unknown member.
  CF_EXPECT_EQ(publish_placement_domain(engine, "placement-missing",
                                        PlacementDomainClass::AvailabilityDomain,
                                        {make_rack("rack-missing")}, 1)
                   .reason,
               RejectionReason::UnknownRack);

  // REJECT_INVALID_DOMAIN: a domain with an UNKNOWN class.
  CF_EXPECT_EQ(publish_placement_domain(engine, "placement-unknown",
                                        PlacementDomainClass::Unknown, {}, 1)
                   .reason,
               RejectionReason::InvalidDomain);

  // REJECT_INVALID_RELATIONSHIP: a link that connects a rack to itself.
  CF_EXPECT_EQ(engine.submit(make_link_request(engine, "link-self", first.rack, first.rack, 1))
                   .reason,
               RejectionReason::InvalidRelationship);

  // REJECT_CONFLICT: two capacity quantities for the same unit.
  {
    MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
    CapacityDomain domain;
    domain.id = *CapacityDomainId::parse("capacity-conflict");
    domain.klass = CapacityDomainClass::RackGroup;
    CapacityQuantity entry;
    entry.unit = "bytes";
    entry.value = 1.0;
    entry.provenance = EvidenceProvenance::Measured;
    domain.quantities = {entry, entry};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.capacity_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::Conflict);
  }

  // REJECT_STALE_PUBLICATION: an older constraint generation.
  {
    MutationRequest request = make_constraint_request(engine, "constraint-stale",
                                                      ConstraintKind::PlacementScope, {}, 5);
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
    MutationRequest older = make_constraint_request(engine, "constraint-stale",
                                                    ConstraintKind::PlacementScope, {}, 4);
    CF_EXPECT_EQ(engine.submit(older).reason, RejectionReason::StalePublication);
  }

  // REJECT_STALE_RACK_GENERATION: a registration older than the record.
  {
    CF_EXPECT_EQ(engine.submit(rack_scoped(engine, MutationKind::RegisterRackPublisher, first))
                     .outcome,
                 MutationOutcome::Accepted);
    MutationRequest older = rack_scoped(engine, MutationKind::RegisterRackPublisher, first);
    older.authority.rack_generation = RackGeneration::from_raw(1);
    CF_EXPECT_EQ(engine.submit(older).reason, RejectionReason::StaleRackGeneration);
  }

  // REJECT_NOT_AUTHORIZED: a publisher that does not own the rack.
  {
    MutationRequest request =
        rack_scoped(engine, MutationKind::PublishHealthCurrentness, first);
    request.authority.publisher = make_publisher("publisher-2");
    request.health = HealthState::Healthy;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::NotAuthorized);
  }

  // REJECT_STALE_RACK_BOOT: a permanently fenced incarnation publishes.
  {
    const RackFixture fenced = add_rack(engine, "rack-fenced", "publisher-1", "boot-fenced", 1, 1);
    CF_EXPECT(engine.coordinator().fence_boot(fenced.boot, "test_fence"));
    CF_EXPECT_EQ(engine.submit(engine.base(MutationKind::PublishSnapshot)).outcome,
                 MutationOutcome::Accepted);
    MutationRequest request =
        rack_scoped(engine, MutationKind::PublishHealthCurrentness, fenced);
    request.authority.publication = PublicationGeneration::from_raw(77);
    request.health = HealthState::Healthy;
    const MutationResult result = engine.submit(request);
    CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
    CF_EXPECT_EQ(result.reason, RejectionReason::StaleRackBoot);
  }

  // REJECT_RETIRED: mutations after the cluster is retired.
  {
    CF_EXPECT_EQ(engine.submit(engine.base(MutationKind::RetireCluster)).outcome,
                 MutationOutcome::Accepted);
    const MutationResult result = engine.submit(engine.base(MutationKind::PublishSnapshot));
    CF_EXPECT_EQ(result.reason, RejectionReason::Retired);
  }

  // Every typed reason round-trips through its stable rendering.
  const RejectionReason reasons[] = {
      RejectionReason::Malformed,
      RejectionReason::WrongCluster,
      RejectionReason::NotReady,
      RejectionReason::StaleClusterEpoch,
      RejectionReason::StaleCoordinatorEpoch,
      RejectionReason::StaleRackBoot,
      RejectionReason::StaleRackGeneration,
      RejectionReason::StaleTopologyEpoch,
      RejectionReason::StalePublication,
      RejectionReason::NotAuthorized,
      RejectionReason::Conflict,
      RejectionReason::UnknownRack,
      RejectionReason::UnknownDomain,
      RejectionReason::InvalidDomain,
      RejectionReason::InvalidRelationship,
      RejectionReason::Retired,
      RejectionReason::LimitExceeded,
      RejectionReason::ShuttingDown,
      RejectionReason::PersistenceFailed,
      RejectionReason::InvariantViolation,
      RejectionReason::Internal};
  for (RejectionReason reason : reasons) {
    CF_EXPECT(rejection_reason_from_string(to_string(reason)).has_value());
    CF_EXPECT_EQ(*rejection_reason_from_string(to_string(reason)), reason);
  }
}

CF_TEST(mutation_shutting_down_is_rejected_after_stop) {
  Engine engine;
  declare_cluster(engine);
  engine.coordinator().stop();
  const MutationResult result = engine.submit(engine.base(MutationKind::PublishSnapshot));
  CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(result.reason, RejectionReason::ShuttingDown);
}

// ---------------------------------------------------------------------------
// Rollback and commit hooks
// ---------------------------------------------------------------------------

CF_TEST(mutation_persistence_failure_rolls_back_everything) {
  FailingStore store;
  Engine engine(kMaxRacksPerCluster, &store);
  declare_cluster(engine);
  (void)add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  const ClusterState before = engine.state();
  const std::string digest_before = semantic_digest_of(before);
  const std::uint64_t saves_before = store.save_calls;

  // Every save from the next one onwards fails.
  store.fail_from = store.save_calls + 1;

  const RackFixture second{make_rack("rack-b"), make_publisher("publisher-1"),
                           make_boot("boot-2"), RackGeneration::from_raw(1),
                           PublicationGeneration::from_raw(1)};
  const MutationResult result = engine.submit(make_add_rack_request(engine, second));
  CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(result.reason, RejectionReason::PersistenceFailed);
  CF_EXPECT_EQ(result.error.category, ErrorCategory::Persistence);
  CF_EXPECT_EQ(result.error.stage, ErrorStage::Persist);
  CF_EXPECT(store.save_calls > saves_before);

  const ClusterState after = engine.state();
  const std::string difference = describe_difference(after, before);
  if (!difference.empty()) {
    CF_FAIL("persistence failure did not restore canonical state exactly: " + difference);
  }
  CF_EXPECT_EQ(semantic_digest_of(after), digest_before);
  CF_EXPECT(engine.coordinator().invariants().ok());
}

CF_TEST(mutation_commit_hook_order_for_accepted_and_rejected) {
  Engine engine;
  RecordingHook hook;
  engine.coordinator().set_commit_hook(&hook);

  declare_cluster(engine);
  CF_EXPECT_EQ(hook.describe(),
               std::string("before_candidate:DECLARE_CLUSTER,before_commit:DECLARE_CLUSTER,"
                           "after_publish:DECLARE_CLUSTER"));

  hook.events.clear();
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  CF_EXPECT_EQ(hook.describe(),
               std::string("before_candidate:ADD_RACK,before_commit:ADD_RACK,"
                           "after_publish:ADD_RACK"));

  hook.events.clear();
  MutationRequest rejected_request = make_add_rack_request(engine, fixture);
  rejected_request.authority.cluster_epoch = ClusterEpoch::from_raw(99);
  const MutationResult rejected = engine.submit(rejected_request);
  CF_EXPECT_EQ(rejected.reason, RejectionReason::StaleClusterEpoch);
  CF_EXPECT_EQ(hook.describe(), std::string("before_candidate:ADD_RACK"));
  CF_EXPECT(!hook.contains("before_commit"));
  CF_EXPECT(!hook.contains("after_publish"));

  engine.coordinator().set_commit_hook(nullptr);
}

CF_TEST(mutation_commit_hook_after_publish_fires_once) {
  // coordinator.hpp documents CommitHook::after_publish as "invoked
  // immediately after the new state has been published", so an accepted
  // mutation must report one after_publish event after before_commit.
  Engine engine;
  RecordingHook hook;
  engine.coordinator().set_commit_hook(&hook);
  declare_cluster(engine);
  hook.events.clear();
  (void)add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  if (hook.describe() !=
      std::string("before_candidate:ADD_RACK,before_commit:ADD_RACK,after_publish:ADD_RACK")) {
    CF_FAIL("commit hook sequence was [" + hook.describe() +
            "] but the documented sequence is "
            "[before_candidate:ADD_RACK,before_commit:ADD_RACK,after_publish:ADD_RACK]");
  }
  engine.coordinator().set_commit_hook(nullptr);
}

// ---------------------------------------------------------------------------
// Invariants and generation monotonicity
// ---------------------------------------------------------------------------

namespace {

/// Applies one mutation and verifies the invariants and generation
/// monotonicity of every accepted step.
class Stepper {
 public:
  explicit Stepper(Engine& engine) : engine_(engine) {}

  MutationResult step(const MutationRequest& request, const char* label) {
    const MutationResult result = engine_.submit(request);
    if (!result.accepted() && result.outcome != MutationOutcome::RevalidationRequired) {
      CF_FAIL(std::string(label) + " was rejected with " +
              std::string(to_string(result.reason)) + ": " + result.error.message);
    }
    const InvariantReport report = engine_.coordinator().invariants();
    if (!report.ok()) {
      CF_FAIL(std::string(label) + " left invariants violated: " + report.describe());
    }
    if (have_previous_) {
      const bool monotonic = result.cluster_generation >= previous_.cluster_generation &&
                             result.membership_generation >= previous_.membership_generation &&
                             result.topology_epoch >= previous_.topology_epoch &&
                             result.topology_generation >= previous_.topology_generation &&
                             result.connectivity_generation >=
                                 previous_.connectivity_generation &&
                             result.health_generation >= previous_.health_generation &&
                             result.constraint_generation >= previous_.constraint_generation &&
                             result.snapshot_generation >= previous_.snapshot_generation &&
                             result.publication_generation >= previous_.publication_generation;
      if (!monotonic) {
        CF_FAIL(std::string(label) + " moved a generation backwards: cluster " +
                previous_.cluster_generation.str() + "->" + result.cluster_generation.str() +
                ", membership " + previous_.membership_generation.str() + "->" +
                result.membership_generation.str() + ", topology " +
                previous_.topology_generation.str() + "->" + result.topology_generation.str() +
                ", connectivity " + previous_.connectivity_generation.str() + "->" +
                result.connectivity_generation.str() + ", health " +
                previous_.health_generation.str() + "->" + result.health_generation.str() +
                ", constraint " + previous_.constraint_generation.str() + "->" +
                result.constraint_generation.str() + ", snapshot " +
                previous_.snapshot_generation.str() + "->" + result.snapshot_generation.str() +
                ", publication " + previous_.publication_generation.str() + "->" +
                result.publication_generation.str());
      }
    }
    previous_ = result;
    have_previous_ = true;
    return result;
  }

 private:
  Engine& engine_;
  MutationResult previous_;
  bool have_previous_ = false;
};

}  // namespace

CF_TEST(mutation_invariants_hold_and_generations_are_monotonic) {
  Engine engine;
  Stepper stepper(engine);

  MutationRequest declare = engine.base(MutationKind::DeclareCluster);
  declare.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  declare.readiness_contract = ReadinessContract::permissive();
  stepper.step(declare, "declare cluster");

  const RackFixture first{make_rack("rack-a"), make_publisher("publisher-1"), make_boot("boot-1"),
                          RackGeneration::from_raw(1), PublicationGeneration::from_raw(1)};
  const RackFixture second{make_rack("rack-b"), make_publisher("publisher-1"), make_boot("boot-2"),
                           RackGeneration::from_raw(1), PublicationGeneration::from_raw(1)};
  stepper.step(make_add_rack_request(engine, first), "add rack a");
  stepper.step(make_add_rack_request(engine, second), "add rack b");

  MutationRequest update = rack_scoped(engine, MutationKind::UpdateRackGeneration, first);
  update.expected_rack_generation = RackGeneration::from_raw(1);
  update.new_rack_generation = RackGeneration::from_raw(2);
  update.authority.publication = PublicationGeneration::from_raw(2);
  stepper.step(update, "update rack generation");

  stepper.step(make_link_request(engine, "link-a-b", first.rack, second.rack, 1), "publish link");
  stepper.step(make_placement_request(engine, "placement-1",
                                      PlacementDomainClass::AvailabilityDomain, {first.rack}, 1),
               "publish placement domain");
  stepper.step(make_constraint_request(engine, "constraint-1", ConstraintKind::PlacementScope,
                                       {first.rack}, 1),
               "publish constraint");

  MutationRequest health = rack_scoped(engine, MutationKind::PublishHealthCurrentness, first);
  health.authority.publication = PublicationGeneration::from_raw(3);
  health.health = HealthState::Healthy;
  stepper.step(health, "publish health");

  MutationRequest withdraw_evidence = rack_scoped(engine, MutationKind::WithdrawEvidence, second);
  withdraw_evidence.evidence_selector = "rack_evidence";
  stepper.step(withdraw_evidence, "withdraw evidence");

  MutationRequest supersede = engine.base(MutationKind::SupersedeTopology);
  supersede.target_topology_epoch = *engine.state().topology_epoch.next();
  stepper.step(supersede, "supersede topology");

  stepper.step(engine.base(MutationKind::PublishSnapshot), "publish snapshot");
  stepper.step(engine.base(MutationKind::RevalidateRecoveredState), "revalidate recovered state");

  MutationRequest unavailable = rack_scoped(engine, MutationKind::MarkRackUnavailable, second);
  unavailable.authority.publication = PublicationGeneration::from_raw(4);
  stepper.step(unavailable, "mark rack unavailable");

  RackFixture recovered = second;
  recovered.boot = make_boot("boot-3");
  recovered.publication = PublicationGeneration::from_raw(5);
  MutationRequest recover = rack_scoped(engine, MutationKind::RecoverRack, recovered);
  recover.rack_reference = make_reference(recovered, recover.evidence);
  stepper.step(recover, "recover rack");

  MutationRequest recovered_health =
      rack_scoped(engine, MutationKind::PublishHealthCurrentness, recovered);
  recovered_health.authority.publication = PublicationGeneration::from_raw(6);
  recovered_health.health = HealthState::Healthy;
  stepper.step(recovered_health, "publish recovered health");

  MutationRequest withdraw_link = engine.base(MutationKind::WithdrawInterRackLink);
  withdraw_link.link_id = *InterRackLinkId::parse("link-a-b");
  stepper.step(withdraw_link, "withdraw link");

  RackFixture withdrawn_fixture = recovered;
  withdrawn_fixture.publication = PublicationGeneration::from_raw(7);
  MutationRequest withdraw_rack =
      rack_scoped(engine, MutationKind::WithdrawRack, withdrawn_fixture);
  stepper.step(withdraw_rack, "withdraw rack");

  RackFixture retired_fixture = recovered;
  retired_fixture.publication = PublicationGeneration::from_raw(8);
  MutationRequest retire = rack_scoped(engine, MutationKind::RetireRack, retired_fixture);
  stepper.step(retire, "retire rack");

  const ClusterState final_state = engine.state();
  CF_EXPECT_EQ(final_state.racks.at(second.rack).membership, RackMembershipState::Retired);
  CF_EXPECT_EQ(final_state.retired_racks.size(), std::size_t{1});
  CF_EXPECT(engine.coordinator().invariants().ok());
}

// ---------------------------------------------------------------------------
// Post-commit state consistency
// ---------------------------------------------------------------------------

CF_TEST(mutation_withdraw_evidence_after_topology_supersession_is_accepted) {
  // WITHDRAW_EVIDENCE is a valid mutation at any point in the topology
  // lifecycle: superseding the topology epoch must not make it impossible.
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture second = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);
  CF_EXPECT_EQ(engine.submit(make_link_request(engine, "link-a-b", first.rack, second.rack, 1))
                   .outcome,
               MutationOutcome::Accepted);

  MutationRequest supersede = engine.base(MutationKind::SupersedeTopology);
  supersede.target_topology_epoch = *engine.state().topology_epoch.next();
  CF_EXPECT_EQ(engine.submit(supersede).outcome, MutationOutcome::Accepted);
  CF_EXPECT(engine.coordinator().invariants().ok());

  MutationRequest withdraw = rack_scoped(engine, MutationKind::WithdrawEvidence, second);
  withdraw.evidence_selector = "rack_evidence";
  const MutationResult result = engine.submit(withdraw);
  if (!result.accepted()) {
    CF_FAIL("WITHDRAW_EVIDENCE after a topology supersession was rejected with " +
            std::string(to_string(result.reason)) + ": " + result.error.message);
  }
  CF_EXPECT(result.accepted());
  CF_EXPECT(engine.coordinator().invariants().ok());
}

CF_TEST(mutation_fence_boot_of_registered_rack_keeps_invariants) {
  // Fencing the incarnation of a rack that has registered but is not yet
  // admitted is a legitimate authority operation and must leave canonical
  // state consistent, so later mutations stay possible.
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-1"), RackGeneration::from_raw(1),
                            PublicationGeneration::from_raw(1)};
  CF_EXPECT_EQ(engine.submit(rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture))
                   .outcome,
               MutationOutcome::Accepted);
  CF_EXPECT(engine.coordinator().fence_boot(fixture.boot, "session_closed"));

  // The fence travels the same commit queue, so a rejected request drains it.
  MutationRequest drain = engine.base(MutationKind::PublishSnapshot);
  drain.cluster = make_cluster("cluster-other");
  CF_EXPECT_EQ(engine.submit(drain).reason, RejectionReason::WrongCluster);

  const InvariantReport report = engine.coordinator().invariants();
  if (!report.ok()) {
    CF_FAIL("fencing a registered rack left invariants violated: " + report.describe());
  }
  CF_EXPECT(report.ok());
  const MutationResult snapshot = engine.submit(engine.base(MutationKind::PublishSnapshot));
  if (!snapshot.accepted()) {
    CF_FAIL("publish snapshot was rejected after fencing a registered rack with " +
            std::string(to_string(snapshot.reason)) + ": " + snapshot.error.message);
  }
  CF_EXPECT(snapshot.accepted());
}

int main() { return cf_test::run("test_mutations"); }
