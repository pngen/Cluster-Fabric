// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Identities, generations and evidence stamps.

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

CF_TEST(identity_rejects_invalid_text) {
  CF_EXPECT(!RackId::parse("").has_value());
  CF_EXPECT(!RackId::parse("..").has_value());
  CF_EXPECT(!RackId::parse("a..b").has_value());
  CF_EXPECT(!RackId::parse("bad/id").has_value());
  CF_EXPECT(!RackId::parse("bad id").has_value());
  CF_EXPECT(!RackId::parse(std::string(129, 'a')).has_value());
  CF_EXPECT(RackId::parse("rack-01").has_value());
  CF_EXPECT(RackId::parse("rack_01:gpu@0").has_value());
  CF_EXPECT_EQ(validate_identity("rack-01").status, IdentityStatus::Ok);
  CF_EXPECT_EQ(validate_identity("").status, IdentityStatus::Empty);
  CF_EXPECT_EQ(validate_identity("a b").status, IdentityStatus::InvalidCharacter);
}

CF_TEST(identity_default_is_unknown) {
  RackId rack;
  CF_EXPECT(!rack.known());
  CF_EXPECT(rack.view().empty());
  CF_EXPECT(!RackId::is_valid(rack.view()));
  CF_EXPECT_EQ(rack, RackId{});
  const RackId parsed = *RackId::parse("rack-01");
  CF_EXPECT_NE(rack, parsed);
  CF_EXPECT_NE(parsed, rack);
}

CF_TEST(boot_identities_are_unique_and_valid) {
  const RackAgentBootId first = make_rack_agent_boot_id("boot");
  const RackAgentBootId second = make_rack_agent_boot_id("boot");
  CF_EXPECT(first.known());
  CF_EXPECT(second.known());
  CF_EXPECT_NE(first, second);
  CF_EXPECT_EQ(validate_identity(first.view()).status, IdentityStatus::Ok);
}

CF_TEST(generations_are_monotonic_and_checked) {
  RackGeneration generation = RackGeneration::from_raw(1);
  CF_EXPECT(generation.known());
  CF_EXPECT(generation.next().has_value());
  CF_EXPECT_EQ(*generation.next(), RackGeneration::from_raw(2));
  CF_EXPECT(RackGeneration::from_raw(1).precedes(RackGeneration::from_raw(2)));
  CF_EXPECT(!RackGeneration::from_raw(2).precedes(RackGeneration::from_raw(1)));
  CF_EXPECT(!RackGeneration::from_raw(1).precedes(RackGeneration::from_raw(1)));
  const RackGeneration maximum = RackGeneration::from_raw(~std::uint64_t{0});
  CF_EXPECT(!maximum.next().has_value());
  CF_EXPECT_EQ(parse_counter_text("17", true).value(), 17u);
  CF_EXPECT_EQ(parse_counter_text("0", true).value(), 0u);
  CF_EXPECT(!parse_counter_text("0", false).has_value());
  CF_EXPECT(!parse_counter_text("x", true).has_value());
  CF_EXPECT(!parse_counter_text("", true).has_value());
  CF_EXPECT(!parse_counter_text("-1", true).has_value());
  CF_EXPECT(!parse_counter_text(" 1", true).has_value());
}

CF_TEST(evidence_freshness_is_conservative) {
  const Timestamp now = Timestamp::from_unix_millis(1'000'000);
  EvidenceStamp fresh = EvidenceStamp::make(EvidenceProvenance::Measured, now, 5'000);
  CF_EXPECT_EQ(fresh.freshness, Freshness::Fresh);
  CF_EXPECT(fresh.is_current());
  fresh.refresh(Timestamp::from_unix_millis(1'006'000));
  CF_EXPECT_EQ(fresh.freshness, Freshness::Stale);
  CF_EXPECT(!fresh.is_current());
  fresh.mark_revalidation_required();
  CF_EXPECT_EQ(fresh.freshness, Freshness::RevalidationRequired);
  CF_EXPECT(fresh.requires_revalidation());
  CF_EXPECT(!fresh.is_current());

  EvidenceStamp synthetic = EvidenceStamp::make(EvidenceProvenance::Synthetic, now, 5'000);
  CF_EXPECT(!synthetic.is_current());
  EvidenceStamp unknown;
  CF_EXPECT_EQ(unknown.freshness, Freshness::Unknown);
  CF_EXPECT(!unknown.is_current());
  CF_EXPECT(!is_current_evidence_provenance(EvidenceProvenance::Estimated));
  CF_EXPECT(!is_current_evidence_provenance(EvidenceProvenance::Unknown));
  CF_EXPECT(is_current_evidence_provenance(EvidenceProvenance::Reported));
}

CF_TEST(enum_text_round_trips) {
  CF_EXPECT_EQ(std::string(to_string(ClusterLifecycle::RevalidationRequired)),
               std::string("REVALIDATION_REQUIRED"));
  CF_EXPECT_EQ(*cluster_lifecycle_from_string("READY"), ClusterLifecycle::Ready);
  CF_EXPECT_EQ(*evidence_provenance_from_string("Measured"), EvidenceProvenance::Measured);
  CF_EXPECT_EQ(*freshness_from_string("STALE"), Freshness::Stale);
  CF_EXPECT_EQ(*durability_from_string("EPHEMERAL"), Durability::Ephemeral);
  CF_EXPECT(!cluster_lifecycle_from_string("nonsense").has_value());
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::RevalidationRequired));
  CF_EXPECT(is_consumable_lifecycle(ClusterLifecycle::Degraded));
}

int main() { return cf_test::run("test_identity_generation"); }
