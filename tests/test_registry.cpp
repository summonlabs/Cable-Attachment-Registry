// Cable Attachment Registry — registry semantics tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCable = object_id(1);
const ObjectId kOther = object_id(2);
const EndpointId kSwitch = endpoint_id(1);
const EndpointId kPanel = endpoint_id(2);

/// A registry with one source that has registered a two-ended cable and two
/// endpoints.
struct Wired {
  Fixture fixture;
  Stream source{source_id(1), Authority{AuthorityClass::Observed, 10}};

  Wired() {
    fixture.open(source, "discovery");
    fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-17")}));
    fixture.ingest(source.make(RegisterObjectPayload{make_object(kOther, "C-18")}));
    fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-01")}));
    fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kPanel, "pp-01")}));
  }
};

} // namespace

CR_TEST_CASE(registry, unknown_port_is_distinct_from_empty_port) {
  Wired wired;
  const PortView untouched = wired.fixture.port(kSwitch, 0);
  CR_CHECK(untouched.state == PortAttachmentState::Unknown);
  CR_CHECK(untouched.edges.empty());

  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);

  wired.fixture.ingest(wired.source.make(detach(kCable, 0, true, kSwitch, 0)));
  const PortView vacated = wired.fixture.port(kSwitch, 0);
  CR_CHECK(vacated.state == PortAttachmentState::Empty);
  CR_CHECK_EQ(vacated.emptiness_claims_total, std::uint32_t{1});

  const PortView never_seen = wired.fixture.port(kSwitch, 5);
  CR_CHECK(never_seen.state == PortAttachmentState::Unknown);
}

CR_TEST_CASE(registry, attach_binds_both_endpoints_of_the_relation) {
  Wired wired;
  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  wired.fixture.ingest(wired.source.make(attach(kCable, 1, kSwitch, 1)));

  const PortView first = wired.fixture.port(kSwitch, 0);
  CR_CHECK(first.state == PortAttachmentState::Attached);
  CR_CHECK_EQ(first.edges.size(), std::size_t{1});
  CR_CHECK(first.edges[0].subject.object == kCable);
  CR_CHECK_EQ(first.edges[0].subject.side.value, std::uint32_t{0});
  CR_CHECK_EQ(first.edges[0].claims.size(), std::size_t{1});
  CR_CHECK(first.edges[0].validation == ValidationState::Validated);

  const ObjectView cable = wired.fixture.object(kCable);
  CR_CHECK(cable.lifecycle == LifecycleState::Attached);
  CR_CHECK_EQ(cable.sides.size(), std::size_t{2});
  CR_CHECK(cable.sides[0].state == PortAttachmentState::Attached);
  CR_CHECK(cable.sides[1].state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, move_is_atomic_and_recorded_as_a_move) {
  Wired wired;
  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  wired.fixture.ingest(wired.source.make(move(kCable, 0, kSwitch, 0, kPanel, 3)));

  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Empty);
  const PortView destination = wired.fixture.port(kPanel, 3);
  CR_CHECK(destination.state == PortAttachmentState::Attached);
  CR_CHECK_EQ(destination.edges.size(), std::size_t{1});
  CR_CHECK(destination.edges[0].subject.object == kCable);

  Outcome<ObjectHistory> history = wired.fixture->QueryHistory(kCable);
  CR_CHECK(history.has_value());
  bool saw_move = false;
  for (const LifecycleEvent& event : history.value().events) {
    if (event.kind == LifecycleEventKind::Moved) {
      saw_move = true;
      CR_CHECK(event.has_from_port);
      CR_CHECK(event.from_port.endpoint == kSwitch);
      CR_CHECK(event.port.endpoint == kPanel);
    }
  }
  CR_CHECK(saw_move);
}

CR_TEST_CASE(registry, generation_order_wins_over_arrival_order) {
  Wired wired;
  const Evidence attach_first = wired.source.make(attach(kCable, 0, kSwitch, 0));
  const Evidence move_second = wired.source.make(move(kCable, 0, kSwitch, 0, kPanel, 1));

  // Feed the newer record first: the older one must not win by arriving later.
  wired.fixture.ingest(move_second);
  const IngestResult late = wired.fixture.try_ingest(attach_first);
  CR_CHECK(late.disposition == IngestDisposition::AcceptedSuperseded);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown ||
           wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Empty);
  CR_CHECK(wired.fixture.port(kPanel, 1).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, identical_resend_is_a_duplicate_and_changes_nothing) {
  Wired wired;
  const Evidence evidence = wired.source.make(attach(kCable, 0, kSwitch, 0));
  wired.fixture.ingest(evidence);
  const IngestResult again = wired.fixture.try_ingest(evidence);
  CR_CHECK(again.disposition == IngestDisposition::Duplicate);
  const PortView view = wired.fixture.port(kSwitch, 0);
  CR_CHECK_EQ(view.edges.size(), std::size_t{1});
  CR_CHECK_EQ(view.edges[0].claims.size(), std::size_t{1});
}

CR_TEST_CASE(registry, one_generation_cannot_carry_two_different_facts) {
  Wired wired;
  const Evidence first = wired.source.make(attach(kCable, 0, kSwitch, 0));
  wired.fixture.ingest(first);

  Evidence collision = first;
  collision.header.id = EvidenceId::from_parts(0xAA, 0xBB);
  collision.payload = attach(kCable, 0, kSwitch, 2);
  const IngestResult refused = wired.fixture.try_ingest(collision);
  CR_CHECK(refused.disposition == IngestDisposition::RefusedConflict);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
  CR_CHECK(wired.fixture.port(kSwitch, 2).state == PortAttachmentState::Unknown);
}

CR_TEST_CASE(registry, evidence_without_a_session_is_refused) {
  Fixture fixture;
  Stream unknown{source_id(9), Authority{AuthorityClass::Observed, 1}};
  const IngestResult result = fixture.try_ingest(unknown.make(RegisterObjectPayload{make_object(kCable, "C-17")}));
  CR_CHECK(result.disposition == IngestDisposition::RefusedUnknownSource);

  fixture.open(unknown, "late");
  const IngestResult accepted =
      fixture.try_ingest(unknown.make(RegisterObjectPayload{make_object(kCable, "C-17")}));
  CR_CHECK(accepted.disposition == IngestDisposition::Accepted);
}

CR_TEST_CASE(registry, a_port_index_outside_the_endpoint_is_never_accepted_silently) {
  Wired wired;
  const IngestResult result = wired.fixture.try_ingest(wired.source.make(attach(kCable, 0, kSwitch, 99)));
  CR_CHECK(result.disposition == IngestDisposition::AcceptedPendingReference);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);
}

CR_TEST_CASE(registry, registering_an_endpoint_later_resolves_pending_evidence) {
  Fixture fixture;
  Stream source{source_id(2), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-30")}));
  const IngestResult pending = fixture.try_ingest(source.make(attach(kCable, 0, kSwitch, 4)));
  CR_CHECK(pending.disposition == IngestDisposition::AcceptedPendingReference);

  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-02")}));
  CR_CHECK(fixture.port(kSwitch, 4).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, a_live_label_cannot_be_reused) {
  Wired wired;
  const ObjectId impostor = object_id(3);
  const IngestResult refused =
      wired.fixture.try_ingest(wired.source.make(RegisterObjectPayload{make_object(impostor, "C-17")}));
  CR_CHECK(refused.disposition == IngestDisposition::RefusedLabelConflict);

  // Retiring the holder releases the label.
  wired.fixture.ingest(wired.source.make(lifecycle(kCable, LifecycleAction::Retired, "scrapped")));
  const IngestResult accepted =
      wired.fixture.try_ingest(wired.source.make(RegisterObjectPayload{make_object(impostor, "C-17")}));
  CR_CHECK(accepted.disposition == IngestDisposition::Accepted);
}

CR_TEST_CASE(registry, reincarnation_fences_the_previous_physical_unit) {
  Wired wired;
  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);

  ReincarnateObjectPayload reincarnate;
  reincarnate.object = kCable;
  reincarnate.from_incarnation = ObjectIncarnation{1};
  reincarnate.new_serial_like = "SN-REPLACEMENT";
  reincarnate.reason = "unit swapped";
  wired.fixture.ingest(wired.source.make(reincarnate));

  const ObjectView cable = wired.fixture.object(kCable);
  CR_CHECK_EQ(cable.incarnation.value, std::uint64_t{2});
  CR_CHECK_EQ(cable.serial_like, std::string("SN-REPLACEMENT"));
  // The attachment was recorded against incarnation one and can never bind.
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);

  // Evidence that still names the old incarnation is stored but fenced.
  const IngestResult stale = wired.fixture.try_ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(stale.disposition == IngestDisposition::AcceptedFencedIncarnation);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);

  // Evidence against the current incarnation binds normally.
  const IngestResult fresh =
      wired.fixture.try_ingest(wired.source.make(attach(kCable, 0, kSwitch, 0, ObjectIncarnation{2})));
  CR_CHECK(fresh.disposition == IngestDisposition::Accepted);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, a_source_incarnation_change_fences_its_older_claims) {
  Wired wired;
  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);

  Stream restarted{source_id(1), Authority{AuthorityClass::Observed, 10}};
  restarted.set_incarnation(Incarnation{2});
  const OpenSessionResult reopened = wired.fixture.open(restarted, "discovery");
  CR_CHECK(reopened.disposition == OpenSessionResult::Disposition::NewIncarnation);
  CR_CHECK(reopened.fenced_claims > 0);

  // The attachment was asserted only by the fenced incarnation, so the port is
  // no longer attached. The endpoint registration is a catalogue fact and
  // survives, which is why the port can still be queried.
  const PortView port = wired.fixture.port(kSwitch, 0);
  CR_CHECK(port.state == PortAttachmentState::Unknown);
  CR_CHECK(port.conflicts.empty());

  // Re-asserting under the new incarnation restores the attachment.
  const IngestResult again = wired.fixture.try_ingest(restarted.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(again.disposition == IngestDisposition::Accepted);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, a_fenced_incarnation_cannot_reopen) {
  Wired wired;
  Stream restarted{source_id(1), Authority{AuthorityClass::Observed, 10}};
  restarted.set_incarnation(Incarnation{5});
  wired.fixture.open(restarted, "discovery");

  Stream older{source_id(1), Authority{AuthorityClass::Observed, 10}};
  older.set_incarnation(Incarnation{2});
  Outcome<OpenSessionResult> refused = wired.fixture->OpenSession(older.descriptor("discovery"), older.incarnation());
  CR_CHECK(refused.has_value());
  CR_CHECK(refused.value().disposition == OpenSessionResult::Disposition::RefusedFenced);
}

CR_TEST_CASE(registry, authority_is_bound_to_the_incarnation) {
  Wired wired;
  SourceDescriptor changed = wired.source.descriptor("discovery");
  changed.authority = Authority{AuthorityClass::Authoritative, 1};
  Outcome<OpenSessionResult> refused = wired.fixture->OpenSession(changed, wired.source.incarnation());
  CR_CHECK(refused.has_value());
  CR_CHECK(refused.value().disposition == OpenSessionResult::Disposition::RefusedDescriptorConflict);
}

CR_TEST_CASE(registry, quarantine_removes_the_object_from_the_graph_and_restores_it) {
  Wired wired;
  wired.fixture.ingest(wired.source.make(attach(kCable, 0, kSwitch, 0)));
  wired.fixture.ingest(wired.source.make(lifecycle(kCable, LifecycleAction::Quarantined, "suspect")));
  CR_CHECK(wired.fixture.object(kCable).lifecycle == LifecycleState::Quarantined);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);

  wired.fixture.ingest(wired.source.make(lifecycle(kCable, LifecycleAction::Present, "cleared")));
  CR_CHECK(wired.fixture.object(kCable).lifecycle == LifecycleState::Attached);
  CR_CHECK(wired.fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, retirement_is_sticky_across_equal_authority_sources) {
  Fixture fixture;
  Stream agent{source_id(20), Authority{AuthorityClass::Observed, 10}};
  Stream rival{source_id(21), Authority{AuthorityClass::Observed, 10}};
  fixture.open(agent, "agent");
  fixture.open(rival, "rival");
  fixture.ingest(agent.make(RegisterObjectPayload{make_object(kCable, "C-70")}));
  fixture.ingest(agent.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-08")}));
  fixture.ingest(agent.make(attach(kCable, 0, kSwitch, 0)));
  fixture.ingest(agent.make(lifecycle(kCable, LifecycleAction::Retired, "end of life")));
  CR_CHECK(fixture.object(kCable).lifecycle == LifecycleState::Retired);

  // Another source of the same authority cannot resurrect it by declaring it
  // present.
  fixture.ingest(rival.make(lifecycle(kCable, LifecycleAction::Present, "back")));
  CR_CHECK(fixture.object(kCable).lifecycle == LifecycleState::Retired);

  // The source that retired it can still correct itself, because inside one
  // stream the newest generation is the source's current word.
  fixture.ingest(agent.make(lifecycle(kCable, LifecycleAction::Present, "mistake")));
  CR_CHECK(fixture.object(kCable).lifecycle != LifecycleState::Retired);

  // A reincarnation restores a retired identity even from another source at
  // the same authority.
  fixture.ingest(agent.make(lifecycle(kCable, LifecycleAction::Retired, "again")));
  CR_CHECK(fixture.object(kCable).lifecycle == LifecycleState::Retired);
  ReincarnateObjectPayload reincarnate;
  reincarnate.object = kCable;
  reincarnate.from_incarnation = ObjectIncarnation{1};
  reincarnate.reason = "refurbished";
  fixture.ingest(rival.make(reincarnate));
  CR_CHECK(fixture.object(kCable).lifecycle != LifecycleState::Retired);
  CR_CHECK_EQ(fixture.object(kCable).incarnation.value, std::uint64_t{2});
}

CR_TEST_CASE(registry, removal_hides_the_object_and_a_higher_authority_restores_it) {
  Fixture fixture;
  Stream observer{source_id(3), Authority{AuthorityClass::Observed, 1}};
  Stream operator_source{source_id(4), Authority{AuthorityClass::Authoritative, 1}};
  fixture.open(observer, "observer");
  fixture.open(operator_source, "operator");
  fixture.ingest(observer.make(RegisterObjectPayload{make_object(kCable, "C-40")}));
  fixture.ingest(observer.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-03")}));
  fixture.ingest(observer.make(attach(kCable, 0, kSwitch, 0)));
  CR_CHECK(fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);

  fixture.ingest(observer.make(lifecycle(kCable, LifecycleAction::Removed, "pulled")));
  CR_CHECK(fixture.object(kCable).lifecycle == LifecycleState::Removed);
  CR_CHECK(fixture.port(kSwitch, 0).state == PortAttachmentState::Unknown);

  SetLifecyclePayload restored;
  restored.object = kCable;
  restored.object_incarnation = ObjectIncarnation{1};
  restored.action = LifecycleAction::Present;
  fixture.ingest(operator_source.make(restored));
  CR_CHECK(fixture.object(kCable).lifecycle == LifecycleState::Attached);
  CR_CHECK(fixture.port(kSwitch, 0).state == PortAttachmentState::Attached);
}

CR_TEST_CASE(registry, an_object_side_cannot_be_in_two_ports_at_once) {
  Fixture fixture;
  Stream left{source_id(5), Authority{AuthorityClass::Observed, 5}};
  Stream right{source_id(6), Authority{AuthorityClass::Observed, 5}};
  fixture.open(left, "left");
  fixture.open(right, "right");
  fixture.ingest(left.make(RegisterObjectPayload{make_object(kCable, "C-50")}));
  fixture.ingest(left.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-04")}));
  fixture.ingest(right.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-04")}));

  fixture.ingest(left.make(attach(kCable, 0, kSwitch, 0)));
  fixture.ingest(right.make(attach(kCable, 0, kSwitch, 1)));

  const PortView first = fixture.port(kSwitch, 0);
  const PortView second = fixture.port(kSwitch, 1);
  CR_CHECK(first.state == PortAttachmentState::Conflicting);
  CR_CHECK(second.state == PortAttachmentState::Conflicting);
  bool saw_reason = false;
  for (const ConflictNote& note : first.conflicts) {
    if (note.reason == ConflictReason::ObjectSideDoubleBooked) {
      saw_reason = true;
    }
  }
  CR_CHECK(saw_reason);
}

CR_TEST_CASE(registry, multi_attachment_capability_admits_several_subjects) {
  Fixture fixture;
  Stream source{source_id(7), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "breakout");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-60")}));
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kOther, "C-61")}));

  PublishPortCapabilityPayload capability;
  capability.port = port_of(kSwitch, 0);
  capability.capability.max_simultaneous_attachments = 2;
  // The endpoint is registered by the same record stream.
  RegisterEndpointPayload endpoint;
  endpoint.descriptor = make_endpoint(kSwitch, "sw-05");
  fixture.ingest(source.make(endpoint));
  fixture.ingest(source.make(capability));

  const PortView view = fixture.port(kSwitch, 0);
  CR_CHECK_EQ(view.capability.max_simultaneous_attachments, std::uint32_t{2});

  // Two subjects at the same port are a conflict on an exclusive port and a
  // legitimate breakout on a port that declares the capability.
  Fixture exclusive;
  Stream other{source_id(8), Authority{AuthorityClass::Observed, 1}};
  Stream also{source_id(10), Authority{AuthorityClass::Observed, 1}};
  exclusive.open(other, "a");
  exclusive.open(also, "b");
  exclusive.ingest(other.make(RegisterObjectPayload{make_object(kCable, "C-62")}));
  exclusive.ingest(other.make(RegisterObjectPayload{make_object(kOther, "C-63")}));
  exclusive.ingest(also.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-06")}));
  exclusive.ingest(other.make(attach(kCable, 0, kSwitch, 0)));
  exclusive.ingest(also.make(attach(kOther, 0, kSwitch, 0)));
  CR_CHECK(exclusive.port(kSwitch, 0).state == PortAttachmentState::Conflicting);

  Fixture declared;
  Stream one{source_id(11), Authority{AuthorityClass::Observed, 1}};
  Stream two{source_id(12), Authority{AuthorityClass::Observed, 1}};
  declared.open(one, "a");
  declared.open(two, "b");
  declared.ingest(one.make(RegisterObjectPayload{make_object(kCable, "C-64")}));
  declared.ingest(one.make(RegisterObjectPayload{make_object(kOther, "C-65")}));
  PublishPortCapabilityPayload wide;
  wide.port = port_of(kSwitch, 0);
  wide.capability.max_simultaneous_attachments = 2;
  RegisterEndpointPayload wide_endpoint;
  wide_endpoint.descriptor = make_endpoint(kSwitch, "sw-07", 8, 2);
  declared.ingest(one.make(wide_endpoint));
  declared.ingest(one.make(wide));
  declared.ingest(one.make(attach(kCable, 0, kSwitch, 0)));
  declared.ingest(two.make(attach(kOther, 0, kSwitch, 0)));
  const PortView shared = declared.port(kSwitch, 0);
  CR_CHECK(shared.state == PortAttachmentState::Attached);
  CR_CHECK_EQ(shared.edges.size(), std::size_t{2});
}
