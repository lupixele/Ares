/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  EXPECT_TRUE(nvhttp::test_support::add_client("invalid", "not a certificate", true).empty());
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_FALSE(nvhttp::test_support::duplicate_client("missing"));
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RePairingReplacesDuplicateCertificateIdentity) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto original_uuid = nvhttp::test_support::add_client("original", paired_credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_FALSE(original_uuid.empty());
  ASSERT_FALSE(other_uuid.empty());
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(original_uuid));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));

  const auto repaired_uuid = nvhttp::test_support::add_client(
    "repaired",
    test_utils::certificates::to_crlf_pem(paired_credentials.x509),
    true
  );

  ASSERT_FALSE(repaired_uuid.empty());
  EXPECT_NE(repaired_uuid, original_uuid);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_EQ(nvhttp::get_cert_by_uuid(repaired_uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, ExactApolloEnumBitvaluesAndOperators) {
  // Exact bitvalues matching Apollo upstream definition
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_reserved), 1U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_input), 256U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::input_controller), 256U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::input_touch), 512U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::input_pen), 1024U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::input_mouse), 2048U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::input_kbd), 4096U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_all_inputs), 7936U);

  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_operation), 65536U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::clipboard_set), 65536U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::clipboard_read), 131072U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::file_upload), 262144U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::file_dwnload), 524288U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::server_cmd), 1048576U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_all_opeiations), 2031616U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_all_operations), 2031616U);

  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_action), 16777216U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::list), 16777216U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::view), 33554432U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::launch), 67108864U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_allow_view), 100663296U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_all_actions), 117440512U);

  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_default), 50331648U);
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_no), 0U);

  // Exact value of _all = 0x071F1F00 = 119480064 (correct Apollo value, not incorrect 119479984)
  EXPECT_EQ(static_cast<uint32_t>(crypto::PERM::_all), 119480064U);
  EXPECT_NE(static_cast<uint32_t>(crypto::PERM::_all), 119479984U);

  // Operators
  EXPECT_TRUE(!crypto::PERM::_no);
  EXPECT_FALSE(!crypto::PERM::_all);
  EXPECT_EQ(crypto::PERM::view | crypto::PERM::launch, crypto::PERM::_allow_view);
  EXPECT_EQ(crypto::PERM::_all & crypto::PERM::_all_inputs, crypto::PERM::_all_inputs);
  EXPECT_EQ(crypto::PERM::_all & crypto::PERM::_no, crypto::PERM::_no);
  EXPECT_EQ((crypto::PERM::_all ^ crypto::PERM::_all_inputs), (crypto::PERM::_all_opeiations | crypto::PERM::_all_actions));

  crypto::PERM p = crypto::PERM::view;
  p |= crypto::PERM::list;
  EXPECT_EQ(p, crypto::PERM::_default);
  p &= crypto::PERM::view;
  EXPECT_EQ(p, crypto::PERM::view);
}

TEST_F(ClientAuthorizationTest, NewPairedClientDefaultsToFullPermissionsToAvoidLockout) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("FullDefaultClient", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  EXPECT_EQ(nvhttp::test_support::get_client_perm(uuid), crypto::PERM::_all);

  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_EQ(clients[0]["uuid"], uuid);
  EXPECT_EQ(clients[0]["perm"], static_cast<uint32_t>(crypto::PERM::_all));
}

TEST_F(ClientAuthorizationTest, PermissionPersistenceRoundtripAndExplicitValues) {
  const auto cred1 = test_utils::certificates::generate_ca_credentials("Client No");
  const auto cred2 = test_utils::certificates::generate_ca_credentials("Client List");
  const auto cred3 = test_utils::certificates::generate_ca_credentials("Client ViewLaunch");
  const auto cred4 = test_utils::certificates::generate_ca_credentials("Client All");

  const auto id1 = nvhttp::test_support::add_client("Client No", cred1.x509, true, crypto::PERM::_no);
  const auto id2 = nvhttp::test_support::add_client("Client List", cred2.x509, true, crypto::PERM::list);
  const auto id3 = nvhttp::test_support::add_client("Client ViewLaunch", cred3.x509, true, crypto::PERM::_allow_view);
  const auto id4 = nvhttp::test_support::add_client("Client All", cred4.x509, true, crypto::PERM::_all);

  ASSERT_FALSE(id1.empty());
  ASSERT_FALSE(id2.empty());
  ASSERT_FALSE(id3.empty());
  ASSERT_FALSE(id4.empty());

  EXPECT_EQ(nvhttp::test_support::get_client_perm(id1), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id2), crypto::PERM::list);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id3), crypto::PERM::_allow_view);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id4), crypto::PERM::_all);

  // Clear in-memory state and reload from disk
  nvhttp::test_support::reset_client_state();
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id1), crypto::PERM::_no);
  nvhttp::test_support::reload_client_state();

  EXPECT_EQ(nvhttp::test_support::get_client_perm(id1), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id2), crypto::PERM::list);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id3), crypto::PERM::_allow_view);
  EXPECT_EQ(nvhttp::test_support::get_client_perm(id4), crypto::PERM::_all);
}

TEST_F(ClientAuthorizationTest, LegacyMissingPermDefaultsToFullAvoidsLockout) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Legacy Client");

  // Create state file with a paired client record that does not contain a "perm" field
  nlohmann::json root_json;
  root_json["root"]["uniqueid"] = "11111111-2222-3333-4444-555555555555";
  nlohmann::json device;
  device["name"] = "Legacy Client";
  device["cert"] = credentials.x509;
  device["uuid"] = "legacy-uuid-12345";
  device["enabled"] = true;
  // Intentionally omit "perm"
  root_json["root"]["named_devices"] = nlohmann::json::array({device});

  std::ofstream out(config::nvhttp.file_state);
  out << root_json.dump();
  out.close();

  nvhttp::test_support::reload_client_state();

  EXPECT_EQ(nvhttp::test_support::get_client_perm("legacy-uuid-12345"), crypto::PERM::_all);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, UnknownBitsMaskedAgainstAll) {
  const auto cred1 = test_utils::certificates::generate_ca_credentials("Client AllOnes");
  const auto cred2 = test_utils::certificates::generate_ca_credentials("Client HighBit");
  const auto cred3 = test_utils::certificates::generate_ca_credentials("Client ReservedBit");

  nlohmann::json root_json;
  root_json["root"]["uniqueid"] = "11111111-2222-3333-4444-555555555555";

  nlohmann::json dev1;
  dev1["name"] = "AllOnes";
  dev1["cert"] = cred1.x509;
  dev1["uuid"] = "all-ones-uuid";
  dev1["enabled"] = true;
  dev1["perm"] = 4294967295U;  // 0xFFFFFFFF, all 32 bits set

  nlohmann::json dev2;
  dev2["name"] = "HighBit";
  dev2["cert"] = cred2.x509;
  dev2["uuid"] = "high-bit-uuid";
  dev2["enabled"] = true;
  dev2["perm"] = 2147483648U;  // 0x80000000, unknown bit 31 set

  nlohmann::json dev3;
  dev3["name"] = "ReservedBit";
  dev3["cert"] = cred3.x509;
  dev3["uuid"] = "reserved-bit-uuid";
  dev3["enabled"] = true;
  dev3["perm"] = 16777217U;  // bit 24 (list) | bit 0 (_reserved)

  root_json["root"]["named_devices"] = nlohmann::json::array({dev1, dev2, dev3});

  std::ofstream out(config::nvhttp.file_state);
  out << root_json.dump();
  out.close();

  nvhttp::test_support::reload_client_state();

  // 0xFFFFFFFF masked against 0x071F1F00 yields exact PERM::_all
  EXPECT_EQ(nvhttp::test_support::get_client_perm("all-ones-uuid"), crypto::PERM::_all);
  // 0x80000000 masked against 0x071F1F00 yields 0 (PERM::_no)
  EXPECT_EQ(nvhttp::test_support::get_client_perm("high-bit-uuid"), crypto::PERM::_no);
  // Reserved bit 0 stripped, only list remains
  EXPECT_EQ(nvhttp::test_support::get_client_perm("reserved-bit-uuid"), crypto::PERM::list);
}

TEST_F(ClientAuthorizationTest, InvalidNegativeAndNonIntegerValuesFailSafeToNoPerm) {
  const auto cred_neg = test_utils::certificates::generate_ca_credentials("Negative Client");
  const auto cred_neg2 = test_utils::certificates::generate_ca_credentials("Negative2 Client");
  const auto cred_text = test_utils::certificates::generate_ca_credentials("Text Client");
  const auto cred_float = test_utils::certificates::generate_ca_credentials("Float Client");
  const auto cred_bool = test_utils::certificates::generate_ca_credentials("Bool Client");
  const auto cred_obj = test_utils::certificates::generate_ca_credentials("Obj Client");
  const auto cred_ovf = test_utils::certificates::generate_ca_credentials("Overflow Client");

  nlohmann::json root_json;
  root_json["root"]["uniqueid"] = "11111111-2222-3333-4444-555555555555";

  nlohmann::json dev_neg;
  dev_neg["name"] = "Neg";
  dev_neg["cert"] = cred_neg.x509;
  dev_neg["uuid"] = "uuid-neg";
  dev_neg["enabled"] = true;
  dev_neg["perm"] = -1;

  nlohmann::json dev_neg2;
  dev_neg2["name"] = "Neg2";
  dev_neg2["cert"] = cred_neg2.x509;
  dev_neg2["uuid"] = "uuid-neg2";
  dev_neg2["enabled"] = true;
  dev_neg2["perm"] = -100;

  nlohmann::json dev_text;
  dev_text["name"] = "Text";
  dev_text["cert"] = cred_text.x509;
  dev_text["uuid"] = "uuid-text";
  dev_text["enabled"] = true;
  dev_text["perm"] = "invalid_not_a_number";

  nlohmann::json dev_float;
  dev_float["name"] = "Float";
  dev_float["cert"] = cred_float.x509;
  dev_float["uuid"] = "uuid-float";
  dev_float["enabled"] = true;
  dev_float["perm"] = 12.34;

  nlohmann::json dev_bool;
  dev_bool["name"] = "Bool";
  dev_bool["cert"] = cred_bool.x509;
  dev_bool["uuid"] = "uuid-bool";
  dev_bool["enabled"] = true;
  dev_bool["perm"] = true;

  nlohmann::json dev_obj;
  dev_obj["name"] = "Obj";
  dev_obj["cert"] = cred_obj.x509;
  dev_obj["uuid"] = "uuid-obj";
  dev_obj["enabled"] = true;
  dev_obj["perm"] = nlohmann::json::object();

  nlohmann::json dev_ovf;
  dev_ovf["name"] = "Ovf";
  dev_ovf["cert"] = cred_ovf.x509;
  dev_ovf["uuid"] = "uuid-ovf";
  dev_ovf["enabled"] = true;
  dev_ovf["perm"] = "99999999999999999999";

  root_json["root"]["named_devices"] = nlohmann::json::array({
    dev_neg, dev_neg2, dev_text, dev_float, dev_bool, dev_obj, dev_ovf
  });

  std::ofstream out(config::nvhttp.file_state);
  out << root_json.dump();
  out.close();

  nvhttp::test_support::reload_client_state();

  // All invalid entries fail safe to PERM::_no instead of silently defaulting to full!
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-neg"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-neg2"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-text"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-float"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-bool"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-obj"), crypto::PERM::_no);
  EXPECT_EQ(nvhttp::test_support::get_client_perm("uuid-ovf"), crypto::PERM::_no);

  // Enabled state remains independent from permissions
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(cred_neg.x509));
}
