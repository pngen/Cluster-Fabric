// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rack membership: registration, admission, membership transitions,
// generation bumps, evidence provenance, retirement memory and authority.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

[[nodiscard]] ClusterId make_cluster(const char* text) { return *ClusterId::parse(text); }
[[nodiscard]] RackId make_rack(const char* text) { return *RackId::parse(text); }
[[nodiscard]] RackPublisherId make_publisher(const char* text) {
  return *RackPublisherId::parse(text);
}
[[nodiscard]] RackAgentBootId make_boot(const char* text) {
  return *RackAgentBootId::parse(text);
}

class Engine {
 public:
  explicit Engine(PersistenceStore* store = nullptr,
                  ReadinessContract contract = ReadinessContract::permissive())
      : store_(store) {
    if (store_ == nullptr) {
      store_ = &own_store_;
    }
    config_.cluster = make_cluster("cluster-1");
    config_.readiness_contract = contract;
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

  [[nodiscard]] EvidenceStamp evidence(EvidenceProvenance provenance = EvidenceProvenance::Measured,
                                       std::int64_t ttl_millis = 60'000) const {
    return EvidenceStamp::make(provenance, clock_.now(), ttl_millis);
  }

  void advance_millis(std::int64_t delta) { clock_.advance_millis(delta); }

  [[nodiscard]] MutationResult submit(const MutationRequest& request) {
    return coordinator_->submit(request);
  }

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
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
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

[[nodiscard]] MutationResult change_membership(Engine& engine, MutationKind kind,
                                               const RackFixture& fixture,
                                               std::uint64_t publication,
                                               EvidenceProvenance provenance) {
  MutationRequest request = rack_scoped(engine, kind, fixture);
  request.authority.publication = PublicationGeneration::from_raw(publication);
  request.evidence = engine.evidence(provenance);
  return engine.submit(request);
}

}  // namespace

CF_TEST(membership_registration_then_admission) {
  Engine engine;
  declare_cluster(engine);
  CF_EXPECT_EQ(engine.state().membership_generation, MembershipGeneration::from_raw(1));

  const RackFixture fixture{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-1"), RackGeneration::from_raw(1),
                            PublicationGeneration::from_raw(1)};

  // Registration alone records an UNKNOWN membership: the rack is known but
  // not part of the composition.
  MutationRequest register_request =
      rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture);
  const MutationResult registered = engine.submit(register_request);
  CF_EXPECT_EQ(registered.outcome, MutationOutcome::Accepted);
  const ClusterState after_register = engine.state();
  const auto registered_it = after_register.racks.find(fixture.rack);
  CF_EXPECT(registered_it != after_register.racks.end());
  CF_EXPECT_EQ(registered_it->second.membership, RackMembershipState::Unknown);
  CF_EXPECT(!registered_it->second.authoritative_current);
  CF_EXPECT_EQ(registered_it->second.non_authoritative_reason,
               std::string("registered_but_not_admitted"));
  CF_EXPECT_EQ(registered_it->second.reference.publisher, fixture.publisher);

  // Admission makes it ACTIVE and advances the membership generation. The rack
  // must publish a newer publication: a publication that is not newer than the
  // accepted one is refused as a stale replay.
  RackFixture admitted_fixture = fixture;
  admitted_fixture.publication = PublicationGeneration::from_raw(2);
  const MutationResult admitted = engine.submit(make_add_rack_request(engine, admitted_fixture));
  CF_EXPECT_EQ(admitted.outcome, MutationOutcome::Accepted);
  const ClusterState after_add = engine.state();
  CF_EXPECT_EQ(after_add.racks.at(fixture.rack).membership, RackMembershipState::Active);
  CF_EXPECT_EQ(after_add.membership_generation, MembershipGeneration::from_raw(2));
  CF_EXPECT_EQ(after_add.racks.at(fixture.rack).membership_generation,
               MembershipGeneration::from_raw(2));
  CF_EXPECT(after_add.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT(after_add.racks.at(fixture.rack).non_authoritative_reason.empty());
  CF_EXPECT_EQ(after_add.current_rack_count(), std::size_t{1});
}

CF_TEST(membership_transitions_bump_generation) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  CF_EXPECT_EQ(engine.state().membership_generation, MembershipGeneration::from_raw(2));

  const MutationResult unavailable =
      change_membership(engine, MutationKind::MarkRackUnavailable, fixture, 2,
                        EvidenceProvenance::Reported);
  CF_EXPECT_EQ(unavailable.outcome, MutationOutcome::Accepted);
  const ClusterState after_unavailable = engine.state();
  CF_EXPECT_EQ(after_unavailable.racks.at(fixture.rack).membership,
               RackMembershipState::Unavailable);
  CF_EXPECT_EQ(after_unavailable.membership_generation, MembershipGeneration::from_raw(3));
  CF_EXPECT_EQ(after_unavailable.racks.at(fixture.rack).membership_generation,
               MembershipGeneration::from_raw(3));
  CF_EXPECT(!after_unavailable.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT_EQ(after_unavailable.racks.at(fixture.rack).reference.currentness,
               RackCurrentness::RevalidationRequired);
  CF_EXPECT_EQ(after_unavailable.racks.at(fixture.rack).non_authoritative_reason,
               std::string("membership_UNAVAILABLE"));
  CF_EXPECT_EQ(after_unavailable.racks.at(fixture.rack).membership_evidence.provenance,
               EvidenceProvenance::Reported);
  CF_EXPECT(after_unavailable.racks.at(fixture.rack).membership_evidence.is_current());
  CF_EXPECT_EQ(after_unavailable.current_rack_count(), std::size_t{0});

  // Re-admission from UNAVAILABLE returns to ACTIVE and bumps again.
  RackFixture readmitted = fixture;
  readmitted.publication = PublicationGeneration::from_raw(3);
  CF_EXPECT_EQ(engine.submit(make_add_rack_request(engine, readmitted)).outcome,
               MutationOutcome::Accepted);
  const ClusterState after_readmission = engine.state();
  CF_EXPECT_EQ(after_readmission.racks.at(fixture.rack).membership, RackMembershipState::Active);
  CF_EXPECT_EQ(after_readmission.membership_generation, MembershipGeneration::from_raw(4));
  CF_EXPECT(after_readmission.racks.at(fixture.rack).authoritative_current);

  const MutationResult withdrawn =
      change_membership(engine, MutationKind::WithdrawRack, fixture, 4,
                        EvidenceProvenance::Reported);
  CF_EXPECT_EQ(withdrawn.outcome, MutationOutcome::Accepted);
  const ClusterState after_withdraw = engine.state();
  CF_EXPECT_EQ(after_withdraw.racks.at(fixture.rack).membership, RackMembershipState::Withdrawn);
  CF_EXPECT_EQ(after_withdraw.membership_generation, MembershipGeneration::from_raw(5));
  CF_EXPECT_EQ(after_withdraw.withdrawn_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(after_withdraw.withdrawn_racks.front(), fixture.rack);

  const MutationResult retired =
      change_membership(engine, MutationKind::RetireRack, fixture, 5,
                        EvidenceProvenance::Reported);
  CF_EXPECT_EQ(retired.outcome, MutationOutcome::Accepted);
  const ClusterState after_retire = engine.state();
  CF_EXPECT_EQ(after_retire.racks.at(fixture.rack).membership, RackMembershipState::Retired);
  CF_EXPECT_EQ(after_retire.membership_generation, MembershipGeneration::from_raw(6));
  CF_EXPECT_EQ(after_retire.retired_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(after_retire.withdrawn_racks.size(), std::size_t{0});
  CF_EXPECT_EQ(after_retire.retired_racks.front().last_generation, RackGeneration::from_raw(1));
  CF_EXPECT(engine.coordinator().invariants().ok());
}

CF_TEST(membership_withdrawn_rack_is_readmitted_and_leaves_the_withdrawn_list) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  const RackFixture other = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);

  CF_EXPECT_EQ(change_membership(engine, MutationKind::WithdrawRack, fixture, 2,
                                 EvidenceProvenance::Reported)
                   .outcome,
               MutationOutcome::Accepted);
  CF_EXPECT_EQ(change_membership(engine, MutationKind::WithdrawRack, other, 2,
                                 EvidenceProvenance::Reported)
                   .outcome,
               MutationOutcome::Accepted);
  const ClusterState withdrawn = engine.state();
  CF_EXPECT_EQ(withdrawn.withdrawn_racks.size(), std::size_t{2});
  CF_EXPECT_EQ(withdrawn.racks.at(fixture.rack).membership, RackMembershipState::Withdrawn);
  CF_EXPECT_EQ(withdrawn.racks.at(other.rack).membership, RackMembershipState::Withdrawn);

  // A fresh admission under current authority re-admits exactly one rack and
  // removes it from the withdrawn list; the other stays withdrawn.
  RackFixture readmitted = fixture;
  readmitted.publication = PublicationGeneration::from_raw(3);
  const MutationResult admission = engine.submit(make_add_rack_request(engine, readmitted));
  CF_EXPECT_EQ(admission.outcome, MutationOutcome::Accepted);
  const ClusterState after = engine.state();
  CF_EXPECT_EQ(after.racks.at(fixture.rack).membership, RackMembershipState::Active);
  CF_EXPECT(after.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT_EQ(after.withdrawn_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(after.withdrawn_racks.front(), other.rack);
  CF_EXPECT_EQ(after.racks.at(other.rack).membership, RackMembershipState::Withdrawn);
  CF_EXPECT(engine.coordinator().invariants().ok());
}

CF_TEST(membership_retired_identity_cannot_be_resurrected) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  CF_EXPECT_EQ(change_membership(engine, MutationKind::RetireRack, fixture, 2,
                                 EvidenceProvenance::Reported)
                   .outcome,
               MutationOutcome::Accepted);

  // A retired identity cannot return to an active membership state: the
  // cluster refuses every later mutation for it.
  const MutationResult unavailable =
      change_membership(engine, MutationKind::MarkRackUnavailable, fixture, 3,
                        EvidenceProvenance::Reported);
  CF_EXPECT_EQ(unavailable.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(unavailable.reason, RejectionReason::Retired);
  const MutationResult re_admit = engine.submit(make_add_rack_request(engine, fixture));
  CF_EXPECT_EQ(re_admit.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(re_admit.reason, RejectionReason::Retired);

  MutationRequest register_again =
      rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture);
  register_again.authority.publication = PublicationGeneration::from_raw(4);
  const MutationResult registered = engine.submit(register_again);
  CF_EXPECT_EQ(registered.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(registered.reason, RejectionReason::Retired);

  RackFixture recovered = fixture;
  recovered.boot = make_boot("boot-2");
  recovered.publication = PublicationGeneration::from_raw(5);
  MutationRequest recover = rack_scoped(engine, MutationKind::RecoverRack, recovered);
  recover.rack_reference = make_reference(recovered, recover.evidence);
  const MutationResult recovered_result = engine.submit(recover);
  CF_EXPECT_EQ(recovered_result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(recovered_result.reason, RejectionReason::Retired);

  // A later dynamic publication must not resurrect the retired rack either.
  MutationRequest health = rack_scoped(engine, MutationKind::PublishHealthCurrentness, fixture);
  health.authority.publication = PublicationGeneration::from_raw(6);
  health.health = HealthState::Healthy;
  const MutationResult health_result = engine.submit(health);
  CF_EXPECT(!health_result.accepted());
  const ClusterState state = engine.state();
  CF_EXPECT_EQ(state.racks.at(fixture.rack).membership, RackMembershipState::Retired);
  CF_EXPECT(!state.racks.at(fixture.rack).authoritative_current);
  CF_EXPECT_EQ(state.retired_racks.size(), std::size_t{1});
}

CF_TEST(membership_retired_identity_is_recorded_once_and_survives_restart) {
  MemoryPersistenceStore store;
  {
    Engine engine(&store);
    declare_cluster(engine);
    const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
    const RackFixture kept = add_rack(engine, "rack-b", "publisher-1", "boot-2", 1, 1);

    MutationRequest retire = rack_scoped(engine, MutationKind::RetireRack, fixture);
    retire.authority.publication = PublicationGeneration::from_raw(2);
    retire.reason = "hardware_removed";
    CF_EXPECT_EQ(engine.submit(retire).outcome, MutationOutcome::Accepted);

    // Retiring again is idempotent and must not append a second identity.
    MutationRequest again = retire;
    again.authority.publication = PublicationGeneration::from_raw(3);
    CF_EXPECT_EQ(engine.submit(again).outcome, MutationOutcome::NoChange);

    // A membership transition on a different rack must not disturb the list.
    CF_EXPECT_EQ(change_membership(engine, MutationKind::MarkRackUnavailable, kept, 3,
                                   EvidenceProvenance::Reported)
                     .outcome,
                 MutationOutcome::Accepted);

    const ClusterState before_restart = engine.state();
    CF_EXPECT_EQ(before_restart.retired_racks.size(), std::size_t{1});
    CF_EXPECT_EQ(before_restart.retired_racks.front().rack, fixture.rack);
    CF_EXPECT_EQ(before_restart.retired_racks.front().reason, std::string("hardware_removed"));
    CF_EXPECT(store.raw_bytes().size() > 0u);

    engine.coordinator().stop();
  }

  // Restart: durable state is reloaded into a new coordinator incarnation.
  Engine restarted(&store);
  const RecoveryReport& recovery = restarted.coordinator().recovery();
  CF_EXPECT(recovery.attempted);
  if (!recovery.loaded) {
    std::string notes;
    for (const std::string& note : recovery.notes) {
      notes += "|" + note;
    }
    CF_FAIL("durable state was not reloaded: status=" +
            std::string(to_string(recovery.status)) + " notes=" + notes +
            " container_bytes=" + std::to_string(store.raw_bytes().size()));
  }
  CF_EXPECT(recovery.loaded);
  CF_EXPECT_EQ(recovery.retired_recovered, std::size_t{1});
  CF_EXPECT(recovery.coordinator_epoch_advanced);
  CF_EXPECT_EQ(restarted.state().coordinator_epoch,
               *recovery.previous_coordinator_epoch.next());

  const ClusterState recovered = restarted.state();
  CF_EXPECT_EQ(recovered.retired_racks.size(), std::size_t{1});
  CF_EXPECT_EQ(recovered.retired_racks.front().rack, make_rack("rack-a"));
  CF_EXPECT_EQ(recovered.retired_racks.front().last_generation, RackGeneration::from_raw(1));
  CF_EXPECT_EQ(recovered.racks.at(make_rack("rack-a")).membership, RackMembershipState::Retired);
  CF_EXPECT(recovered.is_rack_retired(make_rack("rack-a")));

  // Recovered evidence is RECONSTRUCTED and never current.
  const RackRecord& record = recovered.racks.at(make_rack("rack-a"));
  CF_EXPECT_EQ(record.membership_evidence.provenance, EvidenceProvenance::Reconstructed);
  CF_EXPECT_EQ(record.membership_evidence.freshness, Freshness::RevalidationRequired);
  CF_EXPECT(!record.membership_evidence.is_current());
  CF_EXPECT_EQ(record.reference.evidence.provenance, EvidenceProvenance::Reconstructed);
  CF_EXPECT_EQ(record.reference.currentness, RackCurrentness::RevalidationRequired);
  CF_EXPECT(!record.authoritative_current);
  CF_EXPECT_EQ(record.non_authoritative_reason, std::string("recovered_state_requires_revalidation"));

  // The retired identity is still refused after the restart.
  const RackFixture fixture{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-9"), RackGeneration::from_raw(2),
                            PublicationGeneration::from_raw(9)};
  const MutationResult re_admit = restarted.submit(make_add_rack_request(restarted, fixture));
  CF_EXPECT_EQ(re_admit.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(re_admit.reason, RejectionReason::Retired);
  CF_EXPECT_EQ(restarted.state().retired_racks.size(), std::size_t{1});
}

CF_TEST(membership_evidence_provenance_is_recorded_per_decision) {
  Engine engine;
  declare_cluster(engine);

  const RackFixture fixture{make_rack("rack-a"), make_publisher("publisher-1"),
                            make_boot("boot-1"), RackGeneration::from_raw(1),
                            PublicationGeneration::from_raw(1)};
  MutationRequest admission = make_add_rack_request(engine, fixture);
  admission.evidence = engine.evidence(EvidenceProvenance::Measured);
  admission.rack_reference.evidence = admission.evidence;
  CF_EXPECT_EQ(engine.submit(admission).outcome, MutationOutcome::Accepted);

  const RackRecord& admitted = engine.state().racks.at(fixture.rack);
  CF_EXPECT_EQ(admitted.membership_evidence.provenance, EvidenceProvenance::Measured);
  CF_EXPECT_EQ(admitted.membership_evidence.observed_at, engine.now());
  CF_EXPECT(admitted.membership_evidence.is_current());

  // A membership decision carries its own evidence stamp, independent of the
  // rack reference stamp.
  engine.advance_millis(1'000);
  MutationRequest unavailable = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  unavailable.authority.publication = PublicationGeneration::from_raw(2);
  unavailable.evidence = engine.evidence(EvidenceProvenance::Reported);
  CF_EXPECT_EQ(engine.submit(unavailable).outcome, MutationOutcome::Accepted);
  const RackRecord& changed = engine.state().racks.at(fixture.rack);
  CF_EXPECT_EQ(changed.membership_evidence.provenance, EvidenceProvenance::Reported);
  CF_EXPECT_EQ(changed.membership_evidence.observed_at, engine.now());

  // A synthetic stamp is recorded as synthetic and never counts as current.
  engine.advance_millis(1'000);
  MutationRequest synthetic = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  synthetic.authority.publication = PublicationGeneration::from_raw(3);
  synthetic.evidence = engine.evidence(EvidenceProvenance::Synthetic);
  CF_EXPECT_EQ(engine.submit(synthetic).outcome, MutationOutcome::NoChange);
  const RackRecord& unchanged = engine.state().racks.at(fixture.rack);
  CF_EXPECT_EQ(unchanged.membership_evidence.provenance, EvidenceProvenance::Reported);
  CF_EXPECT_EQ(unchanged.membership_evidence.observed_at,
               Timestamp::from_unix_millis(1'001'000));
}

CF_TEST(membership_duplicate_registration_from_another_publisher_conflicts) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);

  MutationRequest duplicate =
      rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture);
  duplicate.authority.publisher = make_publisher("publisher-2");
  duplicate.authority.publication = PublicationGeneration::from_raw(2);
  const MutationResult conflict = engine.submit(duplicate);
  CF_EXPECT_EQ(conflict.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(conflict.reason, RejectionReason::Conflict);
  CF_EXPECT_EQ(conflict.error.reason, std::string("REJECT_CONFLICT"));

  // A different publisher may not advance the generation of an owned rack.
  RackFixture other = fixture;
  other.publisher = make_publisher("publisher-2");
  other.generation = RackGeneration::from_raw(2);
  other.publication = PublicationGeneration::from_raw(2);
  const MutationResult takeover = engine.submit(make_add_rack_request(engine, other));
  CF_EXPECT_EQ(takeover.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(takeover.reason, RejectionReason::NotAuthorized);
  CF_EXPECT_EQ(engine.state().racks.at(fixture.rack).reference.publisher, fixture.publisher);
}

CF_TEST(membership_stale_rack_generation_is_rejected) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 2, 2);

  MutationRequest registration =
      rack_scoped(engine, MutationKind::RegisterRackPublisher, fixture);
  registration.authority.rack_generation = RackGeneration::from_raw(1);
  registration.authority.publication = PublicationGeneration::from_raw(3);
  const MutationResult stale_registration = engine.submit(registration);
  CF_EXPECT_EQ(stale_registration.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(stale_registration.reason, RejectionReason::StaleRackGeneration);

  RackFixture stale = fixture;
  stale.generation = RackGeneration::from_raw(1);
  stale.publication = PublicationGeneration::from_raw(4);
  const MutationResult stale_publication = engine.submit(make_add_rack_request(engine, stale));
  CF_EXPECT_EQ(stale_publication.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(stale_publication.reason, RejectionReason::StaleRackGeneration);
  CF_EXPECT_EQ(engine.state().racks.at(fixture.rack).reference.generation,
               RackGeneration::from_raw(2));
}

CF_TEST(membership_wrong_boot_is_not_authorized) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture fixture = add_rack(engine, "rack-a", "publisher-1", "boot-1", 1, 1);
  CF_EXPECT(engine.state().racks.at(fixture.rack).authoritative_current);

  RackFixture impostor = fixture;
  impostor.boot = make_boot("boot-2");
  impostor.publication = PublicationGeneration::from_raw(2);
  MutationRequest health = rack_scoped(engine, MutationKind::PublishHealthCurrentness, impostor);
  health.health = HealthState::Healthy;
  const MutationResult health_result = engine.submit(health);
  CF_EXPECT_EQ(health_result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(health_result.reason, RejectionReason::NotAuthorized);

  MutationRequest unavailable = rack_scoped(engine, MutationKind::MarkRackUnavailable, impostor);
  unavailable.authority.publication = PublicationGeneration::from_raw(3);
  const MutationResult membership_result = engine.submit(unavailable);
  CF_EXPECT_EQ(membership_result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(membership_result.reason, RejectionReason::NotAuthorized);

  MutationRequest withdraw = rack_scoped(engine, MutationKind::WithdrawRack, impostor);
  withdraw.authority.publication = PublicationGeneration::from_raw(4);
  CF_EXPECT_EQ(engine.submit(withdraw).reason, RejectionReason::NotAuthorized);

  // The owning incarnation still may.
  MutationRequest owned = rack_scoped(engine, MutationKind::MarkRackUnavailable, fixture);
  owned.authority.publication = PublicationGeneration::from_raw(5);
  CF_EXPECT_EQ(engine.submit(owned).outcome, MutationOutcome::Accepted);
  CF_EXPECT_EQ(engine.state().racks.at(fixture.rack).membership,
               RackMembershipState::Unavailable);
}

int main() { return cf_test::run("test_membership"); }
