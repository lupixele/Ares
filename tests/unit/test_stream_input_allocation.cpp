/**
 * @file tests/unit/test_stream_input_allocation.cpp
 * @brief Integration tests for production stream-to-input permission and retained-identity allocation seam.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/crypto.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"

namespace {
  /**
   * @brief Test fixture providing fake platform input devices and clean retained-session state.
   */
  class StreamInputAllocationTest : public ::testing::Test {
  protected:
    /**
     * @brief Configure platform fake input and intercept task scheduling.
     */
    void SetUp() override {
      original_input_config_ = config::input;
      config::input.keyboard = true;
      config::input.mouse = true;
      config::input.controller = true;
      config::input.keybindings.clear();
      config::input.key_rightalt_to_key_win = false;
      config::input.key_repeat_delay = std::chrono::milliseconds {0};

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      input::testing::set_platform_input(std::move(platform_input));
      input::testing::reset_keyboard_state();

      // Intercept scheduled tasks to prevent asynchronous background thread pool execution
      input::testing::set_input_task_sink([](std::shared_ptr<input::input_t>) {});
    }

    /**
     * @brief Restore initial configuration and terminate retained gamepads.
     */
    void TearDown() override {
      input::testing::reset_keyboard_state();
      input::terminate_gamepads();
      input::testing::set_input_task_sink(nullptr);
      input::testing::set_platform_input({});
      config::input = std::move(original_input_config_);
    }

    config::input_t original_input_config_;  ///< Saved configuration restored on teardown.
  };
}  // namespace

/**
 * @brief Verify that production stream allocation captures permissions from launch_session_t,
 * derives stable retained identity from certificate fingerprint, and ignores untrusted unique_id.
 */
TEST_F(StreamInputAllocationTest, ProductionStreamAllocCapturesPermissionsAndDerivesCertFingerprint) {
  const auto creds = crypto::gen_creds("Client Cert 1", 2048);
  const auto expected_perm = crypto::PERM::input_controller | crypto::PERM::input_mouse;

  rtsp_stream::launch_session_t launch {};
  launch.id = 1;
  launch.client_cert = creds.x509;
  launch.perm = expected_perm;
  launch.unique_id = "untrusted-client-uniqueid-1234";
  launch.iv.resize(16);

  stream::config_t config {};
  auto session = stream::session::alloc(config, launch);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(stream::session::permissions(*session), expected_perm);
  EXPECT_EQ(stream::session::client_cert(*session), creds.x509);

  // Retained identity must be the certificate SHA-256 fingerprint, never the untrusted unique_id query key
  const auto expected_fingerprint = crypto::cert_fingerprint(creds.x509);
  EXPECT_FALSE(expected_fingerprint.empty());
  EXPECT_EQ(stream::session::input_session_id(*session), expected_fingerprint);
  EXPECT_NE(stream::session::input_session_id(*session), "untrusted-client-uniqueid-1234");

  auto input_ctx = stream::session::input(*session);
  ASSERT_NE(input_ctx, nullptr);
  EXPECT_EQ(input::get_permissions(input_ctx), expected_perm);
}

/**
 * @brief A supplied malformed certificate cannot fall back to an anonymous input context.
 */
TEST_F(StreamInputAllocationTest, MalformedCertificateRejectsStreamAllocation) {
  rtsp_stream::launch_session_t launch {};
  launch.client_cert = "not a PEM certificate";
  launch.perm = crypto::PERM::_all;
  launch.iv.resize(16);
  stream::config_t config {};
  EXPECT_EQ(stream::session::alloc(config, launch), nullptr);
}

/**
 * @brief Verify that input packets are permitted or blocked based on the allocated permission mask.
 */
TEST_F(StreamInputAllocationTest, InputGatingThroughProductionAllocation) {
  const auto creds = crypto::gen_creds("Client Perm Gate", 2048);
  const auto restricted_perm = crypto::PERM::input_mouse | crypto::PERM::input_touch;

  rtsp_stream::launch_session_t launch {};
  launch.id = 10;
  launch.client_cert = creds.x509;
  launch.perm = restricted_perm;
  launch.iv.resize(16);

  stream::config_t config {};
  auto session = stream::session::alloc(config, launch);
  ASSERT_NE(session, nullptr);

  auto input_ctx = stream::session::input(*session);
  ASSERT_NE(input_ctx, nullptr);

  // 1. Allowed Mouse packet
  NV_REL_MOUSE_MOVE_PACKET mouse_pkt {};
  mouse_pkt.header.size = util::endian::big<std::uint32_t>(sizeof(mouse_pkt) - sizeof(mouse_pkt.header.size));
  mouse_pkt.header.magic = util::endian::little(MOUSE_MOVE_REL_MAGIC_GEN5);
  mouse_pkt.deltaX = 5;
  mouse_pkt.deltaY = 5;

  std::vector<std::uint8_t> mouse_bytes(sizeof(mouse_pkt));
  std::memcpy(mouse_bytes.data(), &mouse_pkt, sizeof(mouse_pkt));

  input::passthrough(input_ctx, std::move(mouse_bytes));
  EXPECT_EQ(input::testing::queued_input_packet_count(input_ctx), 1u);

  // 2. Blocked Keyboard packet (session does not have input_kbd)
  NV_KEYBOARD_PACKET kbd_pkt {};
  kbd_pkt.header.size = util::endian::big<std::uint32_t>(sizeof(kbd_pkt) - sizeof(kbd_pkt.header.size));
  kbd_pkt.header.magic = util::endian::little(KEY_DOWN_EVENT_MAGIC);
  kbd_pkt.keyCode = 0x41;

  std::vector<std::uint8_t> kbd_bytes(sizeof(kbd_pkt));
  std::memcpy(kbd_bytes.data(), &kbd_pkt, sizeof(kbd_pkt));

  input::passthrough(input_ctx, std::move(kbd_bytes));
  // Keyboard packet was blocked by ingress gate; packet count remains 1
  EXPECT_EQ(input::testing::queued_input_packet_count(input_ctx), 1u);
}

/**
 * @brief Verify that re-pairing or presenting a new UUID with the same certificate preserves retained identity.
 */
TEST_F(StreamInputAllocationTest, RePairOrNewUuidRetainsSameContext) {
  const auto creds = crypto::gen_creds("Client RePair", 2048);
  const auto perm = crypto::PERM::input_controller | crypto::PERM::input_kbd;

  rtsp_stream::launch_session_t launch1 {};
  launch1.id = 20;
  launch1.client_cert = creds.x509;
  launch1.perm = perm;
  launch1.unique_id = "initial-uuid-1111";
  launch1.iv.resize(16);

  stream::config_t config {};
  auto session1 = stream::session::alloc(config, launch1);
  ASSERT_NE(session1, nullptr);

  // Reconnecting client presents different UUID / unique_id, but the same certificate
  rtsp_stream::launch_session_t launch2 {};
  launch2.id = 21;
  launch2.client_cert = creds.x509;
  launch2.perm = perm;
  launch2.unique_id = "repaired-uuid-2222";
  launch2.iv.resize(16);

  auto session2 = stream::session::alloc(config, launch2);
  ASSERT_NE(session2, nullptr);

  // Retained input contexts must be identical because fingerprint is identical
  EXPECT_EQ(stream::session::input(*session1).get(), stream::session::input(*session2).get());
  EXPECT_EQ(stream::session::input_session_id(*session1), stream::session::input_session_id(*session2));
}

/**
 * @brief Verify that a forged unique_id with a different client certificate never shares retained contexts.
 */
TEST_F(StreamInputAllocationTest, ForgedUniqueIdDifferentCertContextsNeverShare) {
  const auto creds1 = crypto::gen_creds("Victim Client", 2048);
  const auto creds2 = crypto::gen_creds("Attacker Client", 2048);
  const auto perm = crypto::PERM::_all_inputs;

  rtsp_stream::launch_session_t launch1 {};
  launch1.id = 30;
  launch1.client_cert = creds1.x509;
  launch1.perm = perm;
  launch1.unique_id = "shared-unique-id";
  launch1.iv.resize(16);

  stream::config_t config {};
  auto session1 = stream::session::alloc(config, launch1);
  ASSERT_NE(session1, nullptr);

  // Attacker presents the same unique_id in query args, but different TLS certificate
  rtsp_stream::launch_session_t launch2 {};
  launch2.id = 31;
  launch2.client_cert = creds2.x509;
  launch2.perm = perm;
  launch2.unique_id = "shared-unique-id";
  launch2.iv.resize(16);

  auto session2 = stream::session::alloc(config, launch2);
  ASSERT_NE(session2, nullptr);

  // Must have distinct fingerprints and independent input contexts
  EXPECT_NE(stream::session::input_session_id(*session1), stream::session::input_session_id(*session2));
  EXPECT_NE(stream::session::input(*session1).get(), stream::session::input(*session2).get());
}

/**
 * @brief Verify that reconnecting with different permissions fails orderly without terminating old active context.
 */
TEST_F(StreamInputAllocationTest, DifferentPermissionsResumeFailsOrderlyWithoutTerminatingOldActiveContext) {
  const auto creds = crypto::gen_creds("Client Perm Mutate", 2048);
  const auto initial_perm = crypto::PERM::input_mouse;

  rtsp_stream::launch_session_t launch1 {};
  launch1.id = 40;
  launch1.client_cert = creds.x509;
  launch1.perm = initial_perm;
  launch1.iv.resize(16);

  stream::config_t config {};
  auto session1 = stream::session::alloc(config, launch1);
  ASSERT_NE(session1, nullptr);

  auto initial_input = stream::session::input(*session1);
  ASSERT_NE(initial_input, nullptr);
  EXPECT_EQ(input::get_permissions(initial_input), initial_perm);

  // Reconnecting with different permissions on the same certificate
  rtsp_stream::launch_session_t launch2 {};
  launch2.id = 41;
  launch2.client_cert = creds.x509;
  launch2.perm = crypto::PERM::_all_inputs;
  launch2.iv.resize(16);

  auto session2 = stream::session::alloc(config, launch2);
  // Allocation fails closed orderly because permissions cannot be silently escalated or mutated
  EXPECT_EQ(session2, nullptr);

  // The original active input context remains intact and retains its original permissions
  EXPECT_EQ(input::get_permissions(initial_input), initial_perm);
}
