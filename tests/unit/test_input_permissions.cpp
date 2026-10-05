/**
 * @file tests/unit/test_input_permissions.cpp
 * @brief Unit tests for input packet size validation, domain permission gating, and keyboard sink dispatch.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
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
#include "src/utility.h"

namespace {
  /**
   * @brief Metadata specification describing a fixed-size protocol packet.
   */
  struct packet_spec_t {
    std::uint32_t magic;      ///< Packet type magic identifier.
    std::size_t size;         ///< Total structure size including header.
    crypto::PERM permission;  ///< Expected required domain permission bit.
    const char *name;         ///< Human-readable packet name.
  };

  /**
   * @brief Complete matrix of all known fixed-size protocol input packets.
   */
  constexpr std::array KNOWN_PACKET_SPECS {
    // Controller domain
    packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET), crypto::PERM::input_controller, "MULTI_CONTROLLER"},
    packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET), crypto::PERM::input_controller, "CONTROLLER_ARRIVAL"},
    packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET), crypto::PERM::input_controller, "CONTROLLER_TOUCH"},
    packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET), crypto::PERM::input_controller, "CONTROLLER_MOTION"},
    packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET), crypto::PERM::input_controller, "CONTROLLER_BATTERY"},

    // Mouse domain
    packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET), crypto::PERM::input_mouse, "MOUSE_MOVE_REL"},
    packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET), crypto::PERM::input_mouse, "MOUSE_MOVE_ABS"},
    packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET), crypto::PERM::input_mouse, "MOUSE_BUTTON_DOWN"},
    packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET), crypto::PERM::input_mouse, "MOUSE_BUTTON_UP"},
    packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET), crypto::PERM::input_mouse, "SCROLL"},
    packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET), crypto::PERM::input_mouse, "HSCROLL"},

    // Keyboard domain
    packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET), crypto::PERM::input_kbd, "KEY_DOWN"},
    packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET), crypto::PERM::input_kbd, "KEY_UP"},

    // Touch domain
    packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET), crypto::PERM::input_touch, "SS_TOUCH"},

    // Pen domain
    packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET), crypto::PERM::input_pen, "SS_PEN"},
  };

  /**
   * @brief Construct raw packet buffer with caller-specified magic and sizes.
   *
   * @param magic Packet type magic identifier.
   * @param declared_size Size reported in the header payload size field.
   * @param actual_size Number of bytes in the allocated buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size, 0);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  /**
   * @brief Construct a valid fixed-size packet for the given specification.
   *
   * @param spec Packet specification.
   * @return Raw packet bytes matching exact declared and buffer sizes.
   */
  std::vector<std::uint8_t> make_valid_packet(const packet_spec_t &spec) {
    const auto declared_size = static_cast<std::uint32_t>(spec.size - sizeof(std::uint32_t));
    return make_input_packet(spec.magic, declared_size, spec.size);
  }

  /**
   * @brief Test fixture that disables platform backend input injection and routes keys to test sinks.
   */
  class InputPermissionsTest: public ::testing::Test {
  protected:
    /**
     * @brief Configure input system and install sinks to intercept events before OS delivery.
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

      // Intercept scheduled tasks so background thread pool does not execute asynchronously
      input::testing::set_input_task_sink([](std::shared_ptr<input::input_t>) {});

      // Record keyboard events without injecting to host OS
      input::testing::set_keyboard_sink([this](const input::testing::keyboard_event_t &event) {
        recorded_keyboard_events_.push_back(event);
      });
    }

    /**
     * @brief Clean up sinks and restore configuration.
     */
    void TearDown() override {
      input::testing::reset_keyboard_state();
      input::terminate_gamepads();
      input::testing::set_keyboard_sink(nullptr);
      input::testing::set_input_task_sink(nullptr);
      input::testing::set_platform_input({});
      config::input = std::move(original_input_config_);
    }

    config::input_t original_input_config_;  ///< Saved configuration restored on teardown.
    std::vector<input::testing::keyboard_event_t> recorded_keyboard_events_;  ///< Events intercepted by keyboard sink.
  };
}  // namespace

/**
 * @brief Verify that malformed packet headers and sizes are rejected before typed parsing.
 */
TEST_F(InputPermissionsTest, PacketBoundsAndSizeValidation) {
  // Truncated header smaller than NV_INPUT_HEADER
  for (std::size_t size = 0; size < sizeof(NV_INPUT_HEADER); ++size) {
    std::vector<std::uint8_t> short_buf(size, 0);
    EXPECT_FALSE(input::testing::is_valid_input_packet(short_buf));
  }

  // Declared size smaller than sizeof(header.magic)
  {
    auto invalid_size = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(std::uint32_t) - 1, sizeof(NV_KEYBOARD_PACKET));
    EXPECT_FALSE(input::testing::is_valid_input_packet(invalid_size));
  }

  // Declared size exceeding available buffer bytes
  {
    auto exceeding_size = make_input_packet(KEY_DOWN_EVENT_MAGIC, 100, sizeof(NV_KEYBOARD_PACKET));
    EXPECT_FALSE(input::testing::is_valid_input_packet(exceeding_size));
  }

  // All known packet specs: test valid, undersized declared, oversized declared, and truncated buffer
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    const auto valid_decl = static_cast<std::uint32_t>(spec.size - sizeof(std::uint32_t));

    // Valid packet
    auto valid_pkt = make_input_packet(spec.magic, valid_decl, spec.size);
    EXPECT_TRUE(input::testing::is_valid_input_packet(valid_pkt));

    // Declared size 1 byte too small
    auto under_decl = make_input_packet(spec.magic, valid_decl - 1, spec.size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(under_decl));

    // Declared size 1 byte larger than buffer
    auto over_decl = make_input_packet(spec.magic, valid_decl + 1, spec.size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(over_decl));

    // Buffer truncated by 1 byte
    auto trunc_buf = make_input_packet(spec.magic, valid_decl, spec.size - 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(trunc_buf));
  }

  // Unicode text packet size bounds (UTF8_TEXT_EVENT_MAX_COUNT = 32)
  {
    // Valid text lengths: 0 to 32 bytes
    for (std::size_t text_len = 0; text_len <= 32; ++text_len) {
      const auto decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + text_len);
      auto text_pkt = make_input_packet(UTF8_TEXT_EVENT_MAGIC, decl, sizeof(std::uint32_t) + decl);
      EXPECT_TRUE(input::testing::is_valid_input_packet(text_pkt));
    }

    // Exceeding 32 bytes
    const auto oversized_decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 33);
    auto oversized_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, oversized_decl, sizeof(std::uint32_t) + oversized_decl);
    EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_text));
  }
}

/**
 * @brief Verify that full permission mask accepts all valid packets across all domains.
 */
TEST_F(InputPermissionsTest, PermissionMatrixFullPermissions) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::_all);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));
    ++expected_queued;
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }

  // Unicode text packet under full permissions
  const auto valid_decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 8);
  auto text_pkt = make_input_packet(UTF8_TEXT_EVENT_MAGIC, valid_decl, sizeof(std::uint32_t) + valid_decl);
  input::passthrough(session_input, std::move(text_pkt));
  ++expected_queued;
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
}

/**
 * @brief Verify that zero permission mask drops all packets at ingress.
 */
TEST_F(InputPermissionsTest, PermissionMatrixZeroPermissions) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::_no);
  ASSERT_NE(session_input, nullptr);

  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);
  }

  // Unicode text packet under zero permissions
  const auto valid_decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 8);
  auto text_pkt = make_input_packet(UTF8_TEXT_EVENT_MAGIC, valid_decl, sizeof(std::uint32_t) + valid_decl);
  input::passthrough(session_input, std::move(text_pkt));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);
}

/**
 * @brief Verify domain-restricted gating: controller permission only.
 */
TEST_F(InputPermissionsTest, DomainControllerOnly) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_controller);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));

    if (spec.permission == crypto::PERM::input_controller) {
      ++expected_queued;
    }
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }
}

/**
 * @brief Verify domain-restricted gating: mouse permission only.
 */
TEST_F(InputPermissionsTest, DomainMouseOnly) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_mouse);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));

    if (spec.permission == crypto::PERM::input_mouse) {
      ++expected_queued;
    }
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }
}

/**
 * @brief Verify domain-restricted gating: keyboard permission only.
 */
TEST_F(InputPermissionsTest, DomainKeyboardOnly) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_kbd);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));

    if (spec.permission == crypto::PERM::input_kbd) {
      ++expected_queued;
    }
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }

  // Unicode text event is also under keyboard domain
  const auto valid_decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 8);
  auto text_pkt = make_input_packet(UTF8_TEXT_EVENT_MAGIC, valid_decl, sizeof(std::uint32_t) + valid_decl);
  input::passthrough(session_input, std::move(text_pkt));
  ++expected_queued;
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
}

/**
 * @brief Verify domain-restricted gating: touch permission only.
 */
TEST_F(InputPermissionsTest, DomainTouchOnly) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_touch);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));

    if (spec.permission == crypto::PERM::input_touch) {
      ++expected_queued;
    }
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }
}

/**
 * @brief Verify domain-restricted gating: pen permission only.
 */
TEST_F(InputPermissionsTest, DomainPenOnly) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_pen);
  ASSERT_NE(session_input, nullptr);

  std::size_t expected_queued = 0;
  for (const auto &spec : KNOWN_PACKET_SPECS) {
    SCOPED_TRACE(spec.name);
    auto packet = make_valid_packet(spec);
    input::passthrough(session_input, std::move(packet));

    if (spec.permission == crypto::PERM::input_pen) {
      ++expected_queued;
    }
    EXPECT_EQ(input::testing::queued_input_packet_count(session_input), expected_queued);
  }
}

/**
 * @brief Verify permission intersection: explicit passthrough mask cannot exceed session allocation.
 */
TEST_F(InputPermissionsTest, ExplicitPermissionIntersection) {
  auto mail = std::make_shared<safe::mail_raw_t>();

  // Session allocated with ONLY mouse permission
  auto session_input = input::alloc(mail, crypto::PERM::input_mouse);
  ASSERT_NE(session_input, nullptr);

  // Attempt to pass keyboard packet with passthrough parameter claiming all permissions
  auto kbd_pkt = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - sizeof(std::uint32_t), sizeof(NV_KEYBOARD_PACKET));
  input::passthrough(session_input, std::move(kbd_pkt), crypto::PERM::_all);
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  // Valid mouse packet with matching permission succeeds
  auto mouse_pkt = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - sizeof(std::uint32_t), sizeof(NV_REL_MOUSE_MOVE_PACKET));
  input::passthrough(session_input, std::move(mouse_pkt), crypto::PERM::_all);
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 1u);

  // Mouse packet with explicit snapshot excluding mouse fails
  auto mouse_pkt2 = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - sizeof(std::uint32_t), sizeof(NV_REL_MOUSE_MOVE_PACKET));
  input::passthrough(session_input, std::move(mouse_pkt2), crypto::PERM::input_controller);
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 1u);

  EXPECT_EQ(input::get_permissions(session_input), crypto::PERM::input_mouse);
  EXPECT_EQ(input::testing::input_permissions(session_input), crypto::PERM::input_mouse);
}

/**
 * @brief Verify that malformed packets and unknown magics are dropped before entering the queue.
 */
TEST_F(InputPermissionsTest, MalformedPacketsNeverQueue) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::_all_inputs);
  ASSERT_NE(session_input, nullptr);

  // Unknown magic
  constexpr std::uint32_t unknown_magic = 0xDEADBEEF;
  auto unknown_pkt = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  input::passthrough(session_input, std::move(unknown_pkt));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  // Truncated buffer
  std::vector<std::uint8_t> short_buf(4, 0);
  input::passthrough(session_input, std::move(short_buf));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  // Buffer truncated by 1 byte for keyboard packet
  auto trunc_kbd = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - sizeof(std::uint32_t), sizeof(NV_KEYBOARD_PACKET) - 1);
  input::passthrough(session_input, std::move(trunc_kbd));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  // Oversized text packet
  const auto oversized_decl = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 33);
  auto oversized_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, oversized_decl, sizeof(std::uint32_t) + oversized_decl);
  input::passthrough(session_input, std::move(oversized_text));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);
}

/**
 * @brief Verify real keyboard processor dispatch into safe test sink without OS injection.
 */
TEST_F(InputPermissionsTest, KeyboardProcessorSafeSinkDispatch) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto session_input = input::alloc(mail, crypto::PERM::input_kbd);
  ASSERT_NE(session_input, nullptr);

  // 1. Permitted KeyDown packet
  constexpr std::uint16_t test_vk = 0x41;  // 'A' key
  NV_KEYBOARD_PACKET kbd_down {};
  kbd_down.header.size = util::endian::big<std::uint32_t>(sizeof(kbd_down) - sizeof(kbd_down.header.size));
  kbd_down.header.magic = util::endian::little(KEY_DOWN_EVENT_MAGIC);
  kbd_down.keyCode = static_cast<short>(test_vk);
  kbd_down.modifiers = 0;
  kbd_down.flags = 0;

  std::vector<std::uint8_t> down_bytes(sizeof(kbd_down));
  std::memcpy(down_bytes.data(), &kbd_down, sizeof(kbd_down));

  input::passthrough(session_input, std::move(down_bytes));
  ASSERT_EQ(input::testing::queued_input_packet_count(session_input), 1u);

  // Process synchronously through real Sunshine processor
  EXPECT_TRUE(input::testing::process_next_message_sync(session_input));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  ASSERT_EQ(recorded_keyboard_events_.size(), 1u);
  EXPECT_EQ(recorded_keyboard_events_[0].key_code, test_vk);
  EXPECT_FALSE(recorded_keyboard_events_[0].release);

  // 2. Permitted KeyUp packet
  NV_KEYBOARD_PACKET kbd_up = kbd_down;
  kbd_up.header.magic = util::endian::little(KEY_UP_EVENT_MAGIC);

  std::vector<std::uint8_t> up_bytes(sizeof(kbd_up));
  std::memcpy(up_bytes.data(), &kbd_up, sizeof(kbd_up));

  input::passthrough(session_input, std::move(up_bytes));
  ASSERT_EQ(input::testing::queued_input_packet_count(session_input), 1u);

  EXPECT_TRUE(input::testing::process_next_message_sync(session_input));
  EXPECT_EQ(input::testing::queued_input_packet_count(session_input), 0u);

  ASSERT_EQ(recorded_keyboard_events_.size(), 2u);
  EXPECT_EQ(recorded_keyboard_events_[1].key_code, test_vk);
  EXPECT_TRUE(recorded_keyboard_events_[1].release);

  // 3. Blocked KeyDown packet on session lacking keyboard permission
  auto no_kbd_session = input::alloc(mail, crypto::PERM::input_mouse);
  ASSERT_NE(no_kbd_session, nullptr);

  std::vector<std::uint8_t> blocked_bytes(sizeof(kbd_down));
  std::memcpy(blocked_bytes.data(), &kbd_down, sizeof(kbd_down));

  input::passthrough(no_kbd_session, std::move(blocked_bytes));
  EXPECT_EQ(input::testing::queued_input_packet_count(no_kbd_session), 0u);

  // Synchronous processor drains nothing because queue is empty
  EXPECT_FALSE(input::testing::process_next_message_sync(no_kbd_session));
  EXPECT_EQ(recorded_keyboard_events_.size(), 2u);
}

/**
 * @brief Reuse a retained session only with the same input permission snapshot.
 */
TEST_F(InputPermissionsTest, RetainedSessionImmutablePermissionsOnResume) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  const std::string session_id = "retained-input-permissions-test";

  // Initial stream allocation with restricted mouse-only permissions
  auto initial_session = input::alloc(mail, session_id, crypto::PERM::input_mouse);
  ASSERT_NE(initial_session, nullptr);
  EXPECT_EQ(input::get_permissions(initial_session), crypto::PERM::input_mouse);

  auto resumed_session = input::alloc(mail, session_id, crypto::PERM::input_mouse);
  ASSERT_NE(resumed_session, nullptr);

  // Verify retained session instance is reused
  EXPECT_EQ(initial_session.get(), resumed_session.get());

  // Verify permissions remain immutable while session is active
  EXPECT_EQ(input::get_permissions(resumed_session), crypto::PERM::input_mouse);
  // Changed masks are rejected instead of silently reusing or mutating retained privileges.
  EXPECT_EQ(input::alloc(mail, session_id, crypto::PERM::_all_inputs), nullptr);

  // Controller packet must still be dropped on the resumed session
  auto controller_pkt = make_input_packet(MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET) - sizeof(std::uint32_t), sizeof(NV_MULTI_CONTROLLER_PACKET));
  input::passthrough(resumed_session, std::move(controller_pkt));
  EXPECT_EQ(input::testing::queued_input_packet_count(resumed_session), 0u);

  // Mouse packet must still succeed
  auto mouse_pkt = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - sizeof(std::uint32_t), sizeof(NV_REL_MOUSE_MOVE_PACKET));
  input::passthrough(resumed_session, std::move(mouse_pkt));
  EXPECT_EQ(input::testing::queued_input_packet_count(resumed_session), 1u);
}

/**
 * @brief A lower requested mask must not retrieve an older, more privileged retained context.
 */
TEST_F(InputPermissionsTest, RetainedContextRejectsPermissionReduction) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  const std::string identity = "retained-permission-reduction-test";
  auto privileged = input::alloc(mail, identity, crypto::PERM::_all_inputs);
  ASSERT_TRUE(privileged);
  EXPECT_EQ(input::alloc(mail, identity, crypto::PERM::_all_inputs), privileged);
  EXPECT_EQ(input::alloc(mail, identity, crypto::PERM::input_mouse), nullptr);
  EXPECT_EQ(input::alloc(mail, identity, crypto::PERM::_no), nullptr);
  EXPECT_EQ(input::get_permissions(privileged), crypto::PERM::_all_inputs);
}
