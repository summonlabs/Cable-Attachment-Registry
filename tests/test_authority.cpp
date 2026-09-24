// Cable Attachment Registry — authority, conflict and inspection tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCable = object_id(100);
const ObjectId kRival = object_id(101);
const EndpointId kSwitch = endpoint_id(100);

struct TwoSources {
  Fixture fixture;
  Stream low{source_id(100), Authority{AuthorityClass::Imported, 1}};
  Stream high{source_id(101), Authority{AuthorityClass::Authoritative, 1}};

  TwoSources() {
    fixture.open(low, "import");
    fixture.open(high, "system-of-record");
    fixture.ingest(low.make(RegisterObjectPayload{make_object(kCable, "C-100")}));
    fixture.ingest(low.make(RegisterObjectPayload{make_object(kRival, "C-101")}));
    fixture.ingest(low.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-100")}));
  }
};

} // namespace

CR_TEST_CASE(authority, a_higher_authority_claim_wins_and_the_loser_is_retained) {
  TwoSources sources;
  sources.fixture.ingest(sources.low.make(attach(kCable, 0, kSwitch, 0)));
  sources.fixture.ingest(sources.high.make(attach(kRival, 0, kSwitch, 0)));

  const PortView view = sources.fixture.port(kSwitch, 0);
  CR_CHECK(view.state == PortAttachmentState::Attached);
  CR_CHECK_EQ(view.edges.size(), std::size_t{1});
  CR_CHECK(view.edges[0].subject.object == kRival);
  CR_CHECK(view.edges[0].authority.authority_class == AuthorityClass::Authoritative);
  CR_CHECK_EQ(view.edges[0].overridden_total, std::uint32_t{1});
  CR_CHECK_EQ(view.edges[0].overridden.size(), std::size_t{1});
  CR_CHECK(view.edges[0].overridden[0].source == sources.low.id());
}

CR_TEST_CASE(authority, equal_authority_disagreement_is_preserved_not_resolved) {
  Fixture fixture;
  Stream left{source_id(102), Authority{AuthorityClass::Observed, 5}};
  Stream right{source_id(103), Authority{AuthorityClass::Observed, 5}};
  fixture.open(left, "left");
  fixture.open(right, "right");
  fixture.ingest(left.make(RegisterObjectPayload{make_object(kCable, "C-102")}));
  fixture.ingest(left.make(RegisterObjectPayload{make_object(kRival, "C-103")}));
  fixture.ingest(left.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-101")}));

  fixture.ingest(left.make(attach(kCable, 0, kSwitch, 0)));
  fixture.ingest(right.make(attach(kRival, 0, kSwitch, 0)));

  const PortView view = fixture.port(kSwitch, 0);
  CR_CHECK(view.state == PortAttachmentState::Conflicting);
  CR_CHECK_EQ(view.edges.size(), std::size_t{2});
  CR_CHECK_EQ(view.conflicts.size(), std::size_t{1});
  CR_CHECK(view.conflicts[0].reason == ConflictReason::TooManyAttachments);
  CR_CHECK_EQ(view.conflicts[0].claims.size(), std::size_t{2});
  for (const AttachmentEdge& edge : view.edges) {
    CR_CHECK(edge.conflicting);
  }

  // The conflict survives a snapshot unchanged: it is data, not an artefact of
  // the query.
  const TopologySnapshot snapshot = fixture.snapshot();
  bool found = false;
  for (const SnapshotPort& port : snapshot.ports) {
    if (port.port == port_of(kSwitch, 0)) {
      found = true;
      CR_CHECK(port.state == PortAttachmentState::Conflicting);
      CR_CHECK_EQ(port.attachments.size(), std::size_t{2});
      CR_CHECK_EQ(port.conflicts.size(), std::size_t{1});
    }
  }
  CR_CHECK(found);
}

CR_TEST_CASE(authority, empty_and_occupied_at_the_same_authority_conflict) {
  Fixture fixture;
  Stream one{source_id(104), Authority{AuthorityClass::Observed, 5}};
  Stream two{source_id(105), Authority{AuthorityClass::Observed, 5}};
  fixture.open(one, "one");
  fixture.open(two, "two");
  fixture.ingest(one.make(RegisterObjectPayload{make_object(kCable, "C-104")}));
  fixture.ingest(one.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-102")}));
  fixture.ingest(one.make(attach(kCable, 0, kSwitch, 0)));
  fixture.ingest(two.make(detach(kCable, 0, true, kSwitch, 0)));

  const PortView view = fixture.port(kSwitch, 0);
  CR_CHECK(view.state == PortAttachmentState::Conflicting);
  bool saw_reason = false;
  for (const ConflictNote& note : view.conflicts) {
    if (note.reason == ConflictReason::EmptyAndOccupied) {
      saw_reason = true;
    }
  }
  CR_CHECK(saw_reason);
  CR_CHECK_EQ(view.emptiness_claims_total, std::uint32_t{1});
}

CR_TEST_CASE(authority, live_only_policy_refuses_to_treat_recovered_claims_as_current) {
  TempDir directory("live-only");
  const std::string store = directory.file("registry.log");

  Stream source{source_id(106), Authority{AuthorityClass::Observed, 5}};
  {
    RegistryOptions options;
    options.store = StoreOptions{};
    options.store->path = store;
    Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
    CR_CHECK(opened.has_value());
    Registry& registry = *opened.value();
    Outcome<OpenSessionResult> session = registry.OpenSession(source.descriptor("agent"), Incarnation{1});
    CR_CHECK(session.has_value());
    CR_CHECK(registry.Ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-105")})).has_value());
    CR_CHECK(registry.Ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-103")})).has_value());
    CR_CHECK(registry.Ingest(source.make(attach(kCable, 0, kSwitch, 0))).has_value());
    CR_CHECK(registry.QueryPort(port_of(kSwitch, 0)).value().state == PortAttachmentState::Attached);
    registry.Close();
  }

  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = store;
  Outcome<std::unique_ptr<Registry>> reopened = Registry::Open(options);
  CR_CHECK(reopened.has_value());
  Registry& registry = *reopened.value();

  // Recovered evidence is reported, and is explicitly not validated.
  const Outcome<PortView> included = registry.QueryPort(port_of(kSwitch, 0));
  CR_CHECK(included.has_value());
  CR_CHECK(included.value().state == PortAttachmentState::Attached);
  CR_CHECK(included.value().validation == ValidationState::Unvalidated);
  CR_CHECK_EQ(included.value().edges[0].claims.size(), std::size_t{1});
  CR_CHECK(!included.value().edges[0].claims[0].asserted_in_lifetime);

  // Under LiveOnly the same port is UNKNOWN rather than attached.
  const Outcome<PortView> strict = registry.QueryPort(port_of(kSwitch, 0), ValidationPolicy::LiveOnly);
  CR_CHECK(strict.has_value());
  CR_CHECK(strict.value().state == PortAttachmentState::Unknown);

  // Reopening the session does not make recovered claims fresh by itself.
  const Outcome<OpenSessionResult> resumed = registry.OpenSession(source.descriptor("agent"), Incarnation{1});
  CR_CHECK(resumed.has_value());
  CR_CHECK_MSG(resumed.value().disposition == OpenSessionResult::Disposition::Reopened,
               std::string("disposition was ") + to_string(resumed.value().disposition));
  CR_CHECK(registry.QueryPort(port_of(kSwitch, 0), ValidationPolicy::LiveOnly).value().state ==
           PortAttachmentState::Unknown);

  // Re-asserting the claim at a higher generation does.
  const Outcome<IngestResult> renewed = registry.Ingest(source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(renewed.has_value());
  CR_CHECK(renewed.value().disposition == IngestDisposition::Accepted);
  const Outcome<PortView> live = registry.QueryPort(port_of(kSwitch, 0), ValidationPolicy::LiveOnly);
  CR_CHECK(live.has_value());
  CR_CHECK(live.value().state == PortAttachmentState::Attached);
  CR_CHECK(live.value().validation == ValidationState::Validated);
  registry.Close();
}

CR_TEST_CASE(authority, inspection_reports_fences_pending_and_refusals) {
  Fixture fixture;
  Stream source{source_id(107), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-106")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-104")}));
  fixture.ingest(source.make(attach(kCable, 0, kSwitch, 0)));
  fixture.ingest(source.make(attach(kCable, 1, endpoint_id(999), 0)));

  ReincarnateObjectPayload reincarnate;
  reincarnate.object = kCable;
  reincarnate.from_incarnation = ObjectIncarnation{1};
  fixture.ingest(source.make(reincarnate));

  const IngestResult refused = fixture.try_ingest(source.make(attach(ObjectId{}, 0, kSwitch, 0)));
  CR_CHECK(!refused.accepted());
  CR_CHECK(refused.disposition == IngestDisposition::RefusedInvalid);

  const Outcome<InspectionView> inspection = fixture->Inspect(64);
  CR_CHECK(inspection.has_value());
  CR_CHECK(!inspection.value().refusals.empty());
  CR_CHECK(inspection.value().summary.claims_fenced > 0);
  CR_CHECK(!inspection.value().pending_references.empty() || inspection.value().summary.claims_pending > 0);
  bool saw_fenced_object = false;
  for (const FencedObjectView& object : inspection.value().fenced_objects) {
    if (object.object == kCable) {
      saw_fenced_object = true;
      CR_CHECK_EQ(object.current_incarnation.value, std::uint64_t{2});
      CR_CHECK(object.fenced_claims > 0);
    }
  }
  CR_CHECK(saw_fenced_object);
}

CR_TEST_CASE(authority, incompatible_connector_is_reported_and_the_observation_still_stands) {
  Fixture fixture;
  Stream source{source_id(108), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  ObjectDescriptor optical = make_object(kCable, "C-107", 2, ConnectorClass::Lc, MediaClass::SinglemodeFiber);
  fixture.ingest(source.make(RegisterObjectPayload{optical}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-105")}));
  fixture.ingest(source.make(attach(kCable, 0, kSwitch, 0)));

  const PortView view = fixture.port(kSwitch, 0);
  CR_CHECK(view.state == PortAttachmentState::Attached);
  CR_CHECK(!view.edges[0].connector_compatible);
  bool saw_note = false;
  for (const ConflictNote& note : view.conflicts) {
    if (note.reason == ConflictReason::IncompatibleConnector) {
      saw_note = true;
    }
  }
  CR_CHECK(saw_note);
}

CR_TEST_CASE(authority, evidence_from_an_unknown_object_is_pending_not_refused) {
  Fixture fixture;
  Stream source{source_id(109), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-106")}));
  const IngestResult pending = fixture.try_ingest(source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(pending.disposition == IngestDisposition::AcceptedPendingReference);
  CR_CHECK(fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);

  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-108")}));
  CR_CHECK(fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
}
