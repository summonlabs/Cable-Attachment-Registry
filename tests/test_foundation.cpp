// Cable Attachment Registry — foundation tests: identity, codec, digests, limits.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "test_harness.hpp"

using namespace cable_registry;

namespace {

std::span<const std::byte> bytes_of(const std::vector<std::byte>& data) {
  return std::span<const std::byte>(data.data(), data.size());
}

std::string hex(const Digest256& digest) {
  return to_hex(digest);
}

} // namespace

CR_TEST_CASE(identity, id128_round_trip) {
  Id128 value{};
  CR_CHECK(parse_hex_id("00112233445566778899aabbccddeeff", value));
  CR_CHECK_EQ(value.hi, 0x0011223344556677ull);
  CR_CHECK_EQ(value.lo, 0x8899aabbccddeeffull);
  CR_CHECK_EQ(to_hex(value), std::string("00112233445566778899aabbccddeeff"));

  Id128 dashed{};
  CR_CHECK(parse_hex_id("00112233-4455-6677-8899-aabbccddeeff", dashed));
  CR_CHECK(dashed == value);

  Id128 rejected{};
  CR_CHECK(!parse_hex_id("", rejected));
  CR_CHECK(!parse_hex_id("00112233445566778899aabbccddeef", rejected));
  CR_CHECK(!parse_hex_id("00112233445566778899aabbccddeeff00", rejected));
  CR_CHECK(!parse_hex_id("00112233-44556677-8899-aabbccddeeff", rejected));
  CR_CHECK(!parse_hex_id("zz112233445566778899aabbccddeeff", rejected));
}

CR_TEST_CASE(identity, strong_ids_are_distinct_types) {
  const ObjectId object = ObjectId::from_parts(1, 2);
  CR_CHECK(!object.is_nil());
  CR_CHECK_EQ(object.to_hex(), std::string("00000000000000010000000000000002"));
  const ObjectSideRef side{object, SideIndex{1}};
  const ObjectSideRef other{object, SideIndex{0}};
  CR_CHECK(side != other);
  CR_CHECK(other < side);

  const PortRef port{EndpointId::from_parts(3, 4), PortIndex{7}};
  const PortRef same{EndpointId::from_parts(3, 4), PortIndex{7}};
  CR_CHECK(port == same);
}

CR_TEST_CASE(digest, sha256_known_vectors) {
  CR_CHECK_EQ(hex(sha256(std::string_view(""))),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CR_CHECK_EQ(hex(sha256(std::string_view("abc"))),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CR_CHECK_EQ(hex(sha256(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const std::string million(1000000, 'a');
  CR_CHECK_EQ(hex(sha256(std::string_view(million))),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

CR_TEST_CASE(digest, sha256_incremental_matches_one_shot) {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  Sha256 incremental;
  for (std::size_t index = 0; index < text.size(); ++index) {
    incremental.update(std::string_view(text).substr(index, 1));
  }
  CR_CHECK(incremental.finish() == sha256(std::string_view(text)));

  Sha256 blocked;
  for (std::size_t offset = 0; offset < text.size(); offset += 7) {
    blocked.update(std::string_view(text).substr(offset, 7));
  }
  CR_CHECK(blocked.finish() == sha256(std::string_view(text)));

  Sha256 boundary;
  std::string block(64, 'x');
  boundary.update(std::string_view(block));
  boundary.update(std::string_view("y"));
  CR_CHECK(boundary.finish() == sha256(std::string_view(block + "y")));
}

CR_TEST_CASE(digest, crc32c_known_vectors) {
  CR_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  CR_CHECK_EQ(crc32c(std::string_view("")), 0u);
  CR_CHECK_EQ(crc32c(std::string_view("a")), 0xC1D04330u);
  CR_CHECK_EQ(crc32c(std::string_view("The quick brown fox jumps over the lazy dog")), 0x22620404u);

  Crc32c incremental;
  incremental.update(std::string_view("1234"));
  incremental.update(std::string_view("56789"));
  CR_CHECK_EQ(incremental.finish(), 0xE3069283u);
  incremental.reset();
  CR_CHECK_EQ(incremental.finish(), 0u);
}

CR_TEST_CASE(codec, round_trip_all_primitives) {
  Encoder encoder;
  encoder.u8(0x12);
  encoder.u16(0x3456);
  encoder.u32(0x789ABCDEu);
  encoder.u64(0x0123456789ABCDEFull);
  encoder.i64(-1234567890123);
  encoder.boolean(true);
  encoder.boolean(false);
  encoder.id128(Id128{1, 2});
  encoder.string("hello", 64);
  const std::vector<std::byte> raw = {std::byte{1}, std::byte{2}, std::byte{3}};
  encoder.bytes(bytes_of(raw));
  CR_CHECK(encoder.ok());

  const std::vector<std::byte> encoded = encoder.take();
  Decoder decoder(bytes_of(encoded));
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  std::int64_t i64 = 0;
  bool first = false;
  bool second = true;
  Id128 id{};
  std::string text;
  std::span<const std::byte> blob;
  CR_CHECK(decoder.u8(u8));
  CR_CHECK(decoder.u16(u16));
  CR_CHECK(decoder.u32(u32));
  CR_CHECK(decoder.u64(u64));
  CR_CHECK(decoder.i64(i64));
  CR_CHECK(decoder.boolean(first));
  CR_CHECK(decoder.boolean(second));
  CR_CHECK(decoder.id128(id));
  CR_CHECK(decoder.string(text, 64));
  CR_CHECK(decoder.bytes(blob, 64));
  CR_CHECK(decoder.done());
  CR_CHECK_EQ(u8, std::uint8_t{0x12});
  CR_CHECK_EQ(u16, std::uint16_t{0x3456});
  CR_CHECK_EQ(u32, std::uint32_t{0x789ABCDE});
  CR_CHECK_EQ(u64, 0x0123456789ABCDEFull);
  CR_CHECK_EQ(i64, std::int64_t{-1234567890123});
  CR_CHECK(first);
  CR_CHECK(!second);
  CR_CHECK(id == (Id128{1, 2}));
  CR_CHECK_EQ(text, std::string("hello"));
  CR_CHECK_EQ(blob.size(), std::size_t{3});
}

CR_TEST_CASE(codec, bounds_are_enforced) {
  Encoder limited;
  limited.string("toolong", 3);
  CR_CHECK(!limited.ok());
  CR_CHECK(limited.take().empty());

  Encoder wide;
  wide.u32(7);
  const std::vector<std::byte> wide_bytes = wide.take();
  Decoder decoder(bytes_of(wide_bytes));
  std::string text;
  CR_CHECK(!decoder.string(text, 10));

  Decoder short_buffer;
  Decoder probe;
  const std::vector<std::byte> tiny = {std::byte{1}, std::byte{2}};
  probe.reset(bytes_of(tiny));
  std::uint32_t value = 0;
  CR_CHECK(!probe.u32(value));

  std::uint64_t sum = 0;
  CR_CHECK(checked_add(1, 2, sum));
  CR_CHECK_EQ(sum, 3ull);
  CR_CHECK(!checked_add(UINT64_MAX, 1, sum));
  CR_CHECK(checked_mul(3, 4, sum));
  CR_CHECK_EQ(sum, 12ull);
  CR_CHECK(!checked_mul(UINT64_MAX, 2, sum));
}

CR_TEST_CASE(codec, string_boundary_is_the_declared_maximum) {
  Encoder encoder;
  const std::string exact(16, 'q');
  encoder.string(exact, 16);
  CR_CHECK(encoder.ok());
  const std::vector<std::byte> encoded = encoder.take();
  Decoder decoder(bytes_of(encoded));
  std::string decoded;
  CR_CHECK(decoder.string(decoded, 16));
  CR_CHECK_EQ(decoded, exact);

  Decoder strict(bytes_of(encoded));
  std::string rejected;
  CR_CHECK(!strict.string(rejected, 15));
}

CR_TEST_CASE(limits, validation_rejects_degenerate_policies) {
  Limits limits;
  CR_CHECK(validate_limits(limits).has_value());

  Limits zero_objects;
  zero_objects.max_objects = 0;
  CR_CHECK(!validate_limits(zero_objects).has_value());

  Limits zero_frame;
  zero_frame.max_frame_payload_bytes = 0;
  CR_CHECK(!validate_limits(zero_frame).has_value());

  Limits huge_frame;
  huge_frame.max_frame_payload_bytes = 1u << 30;
  CR_CHECK(!validate_limits(huge_frame).has_value());

  Limits huge_strings;
  huge_strings.max_string_bytes = 1u << 21;
  CR_CHECK(!validate_limits(huge_strings).has_value());
}

CR_TEST_CASE(time, rfc3339_rendering) {
  CR_CHECK_EQ(to_rfc3339_utc(Timestamp{0}), std::string("1970-01-01T00:00:00.000000000Z"));
  CR_CHECK_EQ(to_rfc3339_utc(Timestamp{1'700'000'000'123'456'789}),
              std::string("2023-11-14T22:13:20.123456789Z"));
  CR_CHECK_EQ(to_rfc3339_utc(Timestamp{-1'000'000'000}), std::string("1969-12-31T23:59:59.000000000Z"));
}

CR_TEST_CASE(vocabulary, enums_round_trip_through_text) {
  const std::array<MediaClass, 9> media = {
      MediaClass::Unknown,      MediaClass::CopperTwinax,       MediaClass::CopperTwistedPair,
      MediaClass::MultimodeFiber, MediaClass::SinglemodeFiber, MediaClass::ActiveOptical,
      MediaClass::DirectAttachCopper, MediaClass::PowerBus,   MediaClass::Other};
  for (const MediaClass value : media) {
    MediaClass parsed = MediaClass::Unknown;
    CR_CHECK(parse_media_class(to_string(value), parsed));
    CR_CHECK(parsed == value);
  }

  const std::array<ConnectorClass, 19> connectors = {
      ConnectorClass::Unknown, ConnectorClass::Rj45,    ConnectorClass::Sfp,    ConnectorClass::SfpPlus,
      ConnectorClass::Sfp28,   ConnectorClass::Qsfp,    ConnectorClass::Qsfp28, ConnectorClass::Qsfp56,
      ConnectorClass::QsfpDd,  ConnectorClass::Ospf,    ConnectorClass::Mpo12,  ConnectorClass::Mpo16,
      ConnectorClass::Mpo24,   ConnectorClass::Lc,      ConnectorClass::Sc,     ConnectorClass::Cs,
      ConnectorClass::Sn,      ConnectorClass::BareFiber, ConnectorClass::Other};
  for (const ConnectorClass value : connectors) {
    ConnectorClass parsed = ConnectorClass::Unknown;
    CR_CHECK(parse_connector_class(to_string(value), parsed));
    CR_CHECK(parsed == value);
  }

  const std::array<AuthorityClass, 5> authorities = {AuthorityClass::Speculative, AuthorityClass::Imported,
                                                     AuthorityClass::Observed, AuthorityClass::Declared,
                                                     AuthorityClass::Authoritative};
  for (const AuthorityClass value : authorities) {
    AuthorityClass parsed = AuthorityClass::Speculative;
    CR_CHECK(parse_authority_class(to_string(value), parsed));
    CR_CHECK(parsed == value);
  }

  const std::array<LifecycleState, 7> lifecycles = {
      LifecycleState::Registered, LifecycleState::Unattached,  LifecycleState::Attached,
      LifecycleState::Quarantined, LifecycleState::Removed,    LifecycleState::Retired,
      LifecycleState::Conflicting};
  for (const LifecycleState value : lifecycles) {
    LifecycleState parsed = LifecycleState::Registered;
    CR_CHECK(parse_lifecycle_state(to_string(value), parsed));
    CR_CHECK(parsed == value);
  }

  EvidenceKind kind = EvidenceKind::RegisterObject;
  CR_CHECK(parse_evidence_kind("Move", kind));
  CR_CHECK(kind == EvidenceKind::Move);
  CR_CHECK(!parse_evidence_kind("Nonsense", kind));
}

CR_TEST_CASE(vocabulary, authority_orders_by_class_then_rank) {
  const Authority speculative{AuthorityClass::Speculative, 1000};
  const Authority observed{AuthorityClass::Observed, 0};
  CR_CHECK(speculative < observed);
  const Authority observed_low{AuthorityClass::Observed, 1};
  CR_CHECK(observed < observed_low);
  CR_CHECK((observed == Authority{AuthorityClass::Observed, 0}));
}
