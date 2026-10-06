/**
 * @file tests/unit/test_nvhttp_tls_dispatch.cpp
 * @brief Exercise production authorization over ephemeral localhost TLS connections.
 */
#include "../certificate_test_utils.h"
#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <src/config.h>
#include <src/input.h>
#include <src/nvhttp.h>
#include <src/platform/virtualhid_input.h>
#include <src/process.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace {
  /** @brief Blocking TLS client used only with the isolated loopback test server. */
  class tls_client {
  public:
    /**
     * @brief Complete a real TLS handshake, optionally presenting test credentials.
     * @param port Ephemeral loopback port.
     * @param credentials Optional test-only client credentials.
     */
    tls_client(unsigned short port, const crypto::creds_t *credentials):
        context(boost::asio::ssl::context::tls_client), socket(io, context) {
      if (credentials) {
        // Configure this already-created TLS stream, not a process trust store.
        if (SSL_use_certificate(socket.native_handle(), crypto::x509(credentials->x509).get()) != 1 ||
            SSL_use_PrivateKey(socket.native_handle(), crypto::pkey(credentials->pkey).get()) != 1) {
          throw std::runtime_error("Cannot install ephemeral TLS client credentials");
        }
      }

      run_bounded([this, port](auto &&handler) {
        socket.lowest_layer().async_connect(
          {boost::asio::ip::make_address("127.0.0.1"), port},
          std::forward<decltype(handler)>(handler)
        );
      }, "connect");

      run_bounded([this](auto &&handler) {
        socket.async_handshake(
          boost::asio::ssl::stream_base::client,
          std::forward<decltype(handler)>(handler)
        );
      }, "handshake");
    }

    /**
     * @brief Destructor ensuring socket shutdown and cancellation cleanup.
     */
    ~tls_client() {
      close();
    }

    /**
     * @brief Send another HTTP request on the same TLS connection and return parsed response details.
     * @param path Target request URI including query parameters.
     * @return Full HTTP response containing status line, headers, and body.
     */
    struct response_t {
      std::string status_line;
      std::string all_headers;
      std::string body;
    };

    response_t request_full(const std::string &path = "/identity?client_name=spoof&client_cert=spoof") {
      const std::string message = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
      run_bounded([this, &message](auto &&handler) {
        boost::asio::async_write(
          socket,
          boost::asio::buffer(message),
          std::forward<decltype(handler)>(handler)
        );
      }, "write");

      run_bounded([this](auto &&handler) {
        boost::asio::async_read_until(
          socket,
          buffer,
          "\r\n\r\n",
          std::forward<decltype(handler)>(handler)
        );
      }, "read headers");

      std::istream input(&buffer);
      std::string line;
      std::optional<std::size_t> length;
      std::string all_headers;
      std::string status_line;
      if (std::getline(input, status_line)) {
        if (!status_line.empty() && status_line.back() == '\r') {
          status_line.pop_back();
        }
      }
      while (std::getline(input, line) && line != "\r") {
        all_headers += line + "\n";
        constexpr std::string_view prefix = "Content-Length: ";
        if (line.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), line.begin(), [](char a, char b) {
              return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
          auto len_str = line.substr(prefix.size());
          if (!len_str.empty() && len_str.back() == '\r') {
            len_str.pop_back();
          }
          length = std::stoul(len_str);
        }
      }

      if (!length.has_value()) {
        throw std::runtime_error("Missing Content-Length header in loopback test response: [" + all_headers + "]");
      }

      if (*length > 65536) {
        throw std::runtime_error("Loopback test response length exceeds bounds: " + std::to_string(*length));
      }

      if (*length == 0) {
        if (all_headers.find("401 Unauthorized") != std::string::npos) {
          close();
        }
        return {status_line, all_headers, {}};
      }

      if (buffer.size() < *length) {
        const std::size_t needed = *length - buffer.size();
        run_bounded([this, needed](auto &&handler) {
          boost::asio::async_read(
            socket,
            buffer,
            boost::asio::transfer_exactly(needed),
            std::forward<decltype(handler)>(handler)
          );
        }, "read body");
      }

      std::string body(*length, '\0');
      input.read(body.data(), static_cast<std::streamsize>(*length));
      if (body == "DENIED" || all_headers.find("401 Unauthorized") != std::string::npos) {
        close();
      }
      return {status_line, all_headers, body};
    }

    /**
     * @brief Send another HTTP request on the same TLS connection.
     * @param path Target request URI including query parameters.
     * @return HTTP response body identifying the authorized server-owned principal.
     */
    std::string request(const std::string &path = "/identity?client_name=spoof&client_cert=spoof") {
      return request_full(path).body;
    }

    /** @brief Explicitly shut down and close the client socket. */
    void close() {
      boost::system::error_code ec;
      socket.lowest_layer().cancel(ec);
      socket.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      socket.lowest_layer().close(ec);
    }

  private:
    /**
     * @brief Execute an asynchronous operation bounded by a deadline timer.
     * @tparam Op Callable accepting an Asio completion handler.
     * @param op Async initiator taking a completion handler.
     * @param op_name Diagnostic name of the operation.
     * @param timeout Maximum allowed duration before cancellation.
     */
    template <typename Op>
    void run_bounded(Op &&op, const char *op_name, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
      io.restart();
      boost::asio::steady_timer timer(io);
      timer.expires_after(timeout);

      bool timed_out = false;
      boost::system::error_code op_ec;

      timer.async_wait([this, &timed_out](const boost::system::error_code &ec) {
        if (!ec) {
          timed_out = true;
          boost::system::error_code cancel_ec;
          socket.lowest_layer().cancel(cancel_ec);
          socket.lowest_layer().close(cancel_ec);
        }
      });

      op([&timer, &op_ec](const boost::system::error_code &ec, auto &&...) {
        op_ec = ec;
        timer.cancel();
      });

      io.run();

      if (timed_out) {
        close();
        throw std::runtime_error(std::string("TLS client operation timed out: ") + op_name);
      }
      if (op_ec) {
        throw boost::system::system_error(op_ec, op_name);
      }
    }

    boost::asio::io_context io;  ///< Local synchronous client executor.
    boost::asio::ssl::context context;  ///< Test-only context, never an OS trust store.
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket;  ///< Actual connected TLS stream.
    boost::asio::streambuf buffer;  ///< Persistent stream receive buffer across sequential requests.
  };
}  // namespace

/** @brief Isolate server credentials, pairing state, listener, and configuration. */
class TLSDispatchTest: public BaseTest {
protected:
  /** @brief Start the production backend on a loopback-only ephemeral port. */
  void SetUp() override {
    BaseTest::SetUp();
    old_file_state = config::nvhttp.file_state;
    old_credentials_file = config::sunshine.credentials_file;
    old_fresh = config::sunshine.flags[config::flag::FRESH_STATE];

    directory = std::filesystem::path(SUNSHINE_TEST_BIN_DIR) / "tls-dispatch-credentials";
    std::filesystem::create_directories(directory);

    config::nvhttp.file_state = (directory / "sunshine_state.json").string();
    config::sunshine.credentials_file = config::nvhttp.file_state;
    config::sunshine.flags[config::flag::FRESH_STATE] = true;

    nvhttp::test_support::reset_client_state();

    const auto credentials = crypto::gen_creds("Loopback Test Server", 2048);
    std::ofstream(directory / "cert.pem") << credentials.x509;
    std::ofstream(directory / "key.pem") << credentials.pkey;

    server = nvhttp::test_support::make_authorized_https_server((directory / "cert.pem").string(), (directory / "key.pem").string(), install_resolver());
    server->config.address = "127.0.0.1";
    server->config.port = 0;
    server->config.timeout_request = 5;
    server->config.timeout_content = 5;

    server->resource["^/identity$"]["GET"] = [](auto response, auto request) {
      const auto principal = nvhttp::request_principal(request);
      response->write(principal ? principal->name : "MISSING PRINCIPAL REACHED ENDPOINT");
    };

    server->resource["^/launch-identity$"]["GET"] = [](auto response, auto request) {
      const auto session = nvhttp::test_support::make_request_launch_session(request);
      response->write(session ? session->client_name : "MISSING PRINCIPAL REACHED ENDPOINT");
    };

    server->resource["^/principal-perm$"]["GET"] = [](auto response, auto request) {
      const auto principal = nvhttp::request_principal(request);
      response->write(principal ? std::to_string(static_cast<uint32_t>(principal->perm)) : "MISSING PRINCIPAL REACHED ENDPOINT");
    };

    server->resource["^/launch-perm$"]["GET"] = [](auto response, auto request) {
      const auto session = nvhttp::test_support::make_request_launch_session(request);
      response->write(session ? std::to_string(static_cast<uint32_t>(session->perm)) : "MISSING PRINCIPAL REACHED ENDPOINT");
    };

    nvhttp::test_support::mount_production_action_routes(server, host_audio);

    auto listening = std::make_shared<std::promise<unsigned short>>();
    auto ready = listening->get_future();
    worker = std::thread([this, listening]() {
      try {
        server->start([listening](unsigned short value) { listening->set_value(value); });
      } catch (...) {
        try {
          listening->set_exception(std::current_exception());
        } catch (const std::future_error &) {
          // A startup error is propagated above; an event-loop error after
          // startup must not terminate the entire test process.
        }
      }
    });

    if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      if (server && server->io_service) {
        server->io_service->stop();
      }
      if (server) {
        server->stop();
      }
      if (worker.joinable()) {
        worker.join();
      }
      throw std::runtime_error("Server startup timed out waiting for ephemeral port");
    }
    port = ready.get();
  }

  /** @brief Stop only the isolated test listener and delete ephemeral credentials. */
  void TearDown() override {
    if (server) {
      if (server->io_service) {
        server->io_service->stop();
      }
      server->stop();
    }
    if (worker.joinable()) {
      worker.join();
    }
    server.reset();

    nvhttp::test_support::reset_client_state();
    nvhttp::test_support::skip_hardware_for_testing = false;
    config::nvhttp.file_state = old_file_state;
    config::sunshine.credentials_file = old_credentials_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = old_fresh;

    std::filesystem::remove_all(directory);
    BaseTest::TearDown();
  }

  std::shared_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Production backend under test.
  unsigned short port {};  ///< Ephemeral loopback listener port.
  bool host_audio {false};  ///< Production audio playback destination flag.
  std::thread worker;  ///< Server event loop.
  std::filesystem::path directory;  ///< Test-only PEM storage.
  bool old_fresh {};  ///< Original persistence setting.
  std::string old_file_state;  ///< Original persisted state file path.
  std::string old_credentials_file;  ///< Original credentials file path.

  /** @brief Select whether the backend receives its required authorization resolver. @return True for the ordinary production configuration. */
  virtual bool install_resolver() const {
    return true;
  }
};

/** @brief Exercise a missing resolver on the actual production TLS backend. */
class TLSMissingResolverTest: public TLSDispatchTest {
protected:
  /** @brief Omit the resolver. @return False to test fail-closed dispatch. */
  bool install_resolver() const override {
    return false;
  }
};

TEST_F(TLSMissingResolverTest, MissingResolverDeniesRequests) {
  tls_client client(port, nullptr);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, DistinctConnectionsAndKeepAliveUseTheirOwnCurrentPrincipal) {
  const auto alice = crypto::gen_creds("TLS Alice", 2048);
  const auto bob = crypto::gen_creds("TLS Bob", 2048);
  const auto alice_id = nvhttp::test_support::add_client("Alice", alice.x509, true);
  const auto bob_id = nvhttp::test_support::add_client("Bob", bob.x509, true);
  tls_client first(port, &alice);
  tls_client second(port, &bob);
  // Both handshakes precede either request: the former last-verified global
  // necessarily misattributes at least one of these connections.
  EXPECT_EQ(first.request(), "Alice");
  EXPECT_EQ(second.request(), "Bob");
  auto concurrent_first = std::async(std::launch::async, [&first]() { return first.request(); });
  auto concurrent_second = std::async(std::launch::async, [&second]() { return second.request(); });
  EXPECT_EQ(concurrent_first.get(), "Alice");
  EXPECT_EQ(concurrent_second.get(), "Bob");

  // Keep-alive connection must reflect paired client state modification.
  nvhttp::unpair_client(alice_id);
  EXPECT_EQ(first.request(), "DENIED");
  EXPECT_EQ(second.request(), "Bob");

  nvhttp::unpair_client(bob_id);
  EXPECT_EQ(second.request(), "DENIED");
}

TEST_F(TLSDispatchTest, DisabledPeerCannotReachEndpoint) {
  const auto disabled = crypto::gen_creds("Disabled TLS Client", 2048);
  nvhttp::test_support::add_client("Disabled", disabled.x509, false);
  tls_client client(port, &disabled);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, UnknownPeerCannotSpoofIdentityInQuery) {
  const auto attacker = crypto::gen_creds("Attacker", 2048);
  tls_client client(port, &attacker);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, MissingPeerCannotReachEndpoint) {
  bool rejected = false;
  try {
    tls_client client(port, nullptr);
    rejected = client.request() == "DENIED";
  } catch (const boost::system::system_error &error) {
    // TLS 1.2 rejects during handshake; TLS 1.3 may report the alert on read.
    rejected = &error.code().category() == &boost::asio::error::get_ssl_category() ||
               error.code() == boost::asio::error::eof ||
               error.code() == boost::asio::ssl::error::stream_truncated;
  }
  EXPECT_TRUE(rejected);
}

TEST_F(TLSDispatchTest, DuplicateRegistryIdentityIsRejected) {
  const auto credentials = crypto::gen_creds("Ambiguous TLS Client", 2048);
  const auto id = nvhttp::test_support::add_client("First", credentials.x509, true);
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(id));
  tls_client client(port, &credentials);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, TrustedChainWithoutExactPairedLeafIsRejected) {
  const auto issuer = test_utils::certificates::generate_ca_credentials();
  const auto leaf = test_utils::certificates::generate_derived_leaf(issuer);
  nvhttp::test_support::add_client("Issuer", issuer.x509, true);
  tls_client client(port, &leaf);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, ExistingPairedExpiredCertificatePolicyIsPreserved) {
  const auto expired = test_utils::certificates::expire_credentials(crypto::gen_creds("Expired Client", 2048));
  nvhttp::test_support::add_client("Expired but paired", expired.x509, true);
  tls_client client(port, &expired);
  EXPECT_EQ(client.request(), "Expired but paired");
}

TEST_F(TLSDispatchTest, RegistryReplacementIsVisibleOnNextKeepAliveRequest) {
  const auto original = crypto::gen_creds("TLS Original", 2048);
  const auto replaced = crypto::gen_creds("TLS Replacement", 2048);
  const auto id = nvhttp::test_support::add_client("Original", original.x509, true);

  tls_client client(port, &original);
  EXPECT_EQ(client.request(), "Original");

  nvhttp::unpair_client(id);
  nvhttp::test_support::add_client("Replacement", replaced.x509, true);
  EXPECT_EQ(client.request(), "DENIED");
}

TEST_F(TLSDispatchTest, LaunchSessionIdentityUsesAuthorizedPrincipal) {
  const auto client_creds = crypto::gen_creds("TLS Launch Client", 2048);
  nvhttp::test_support::add_client("LaunchClient", client_creds.x509, true);

  tls_client client(port, &client_creds);
  EXPECT_EQ(client.request("/launch-identity?rikey=0123456789abcdef0123456789abcdef&rikeyid=1&appid=1&client_name=spoof&client_cert=spoof&uniqueid=spoof"), "LaunchClient");
}

TEST_F(TLSDispatchTest, LaunchSessionPermissionsMappedFromAuthorizedPrincipal) {
  const auto client_creds = crypto::gen_creds("TLS Perm Launch Client", 2048);
  const auto expected_perm = crypto::PERM::input_mouse | crypto::PERM::input_controller;
  nvhttp::test_support::add_client("PermLaunchClient", client_creds.x509, true, expected_perm);

  tls_client client(port, &client_creds);
  // Spoofed query parameters cannot alter mapped permission
  EXPECT_EQ(
    client.request("/launch-perm?rikey=0123456789abcdef0123456789abcdef&rikeyid=1&appid=1&perm=12345&permissions=99999"),
    std::to_string(static_cast<uint32_t>(expected_perm))
  );
}

TEST_F(TLSDispatchTest, KeepAliveRequestPrincipalPermissionSnapshotTracksRegistryUpdate) {
  const auto creds = crypto::gen_creds("TLS Perm Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("Perm Client", creds.x509, true, crypto::PERM::_all);
  ASSERT_FALSE(uuid.empty());

  tls_client client(port, &creds);
  EXPECT_EQ(client.request("/principal-perm"), std::to_string(static_cast<uint32_t>(crypto::PERM::_all)));

  // Update permissions in the server pairing registry without closing the TLS connection
  ASSERT_TRUE(nvhttp::test_support::set_client_perm(uuid, crypto::PERM::_default));

  // The next request on the same keep-alive TLS connection receives the updated permission snapshot
  EXPECT_EQ(client.request("/principal-perm"), std::to_string(static_cast<uint32_t>(crypto::PERM::_default)));
}

TEST_F(TLSDispatchTest, PermNoDoesNotDisableClientConnection) {
  const auto creds = crypto::gen_creds("TLS Perm No Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("PermNoClient", creds.x509, true, crypto::PERM::_no);
  ASSERT_FALSE(uuid.empty());

  tls_client client(port, &creds);
  // Client is enabled, so TLS handshake and dispatch succeed, reflecting snapshot with perm == 0
  EXPECT_EQ(client.request("/principal-perm"), "0");
  EXPECT_EQ(client.request("/identity"), "PermNoClient");
}

TEST_F(TLSDispatchTest, DisabledClientCannotConnectRegardlessOfPermissions) {
  const auto creds = crypto::gen_creds("TLS Disabled Full Perm Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("DisabledFullPerm", creds.x509, false, crypto::PERM::_all);
  ASSERT_FALSE(uuid.empty());

  tls_client client(port, &creds);
  // Client is disabled, so authorization fails closed despite having PERM::_all
  EXPECT_EQ(client.request("/principal-perm"), "DENIED");
}

TEST(ActionDecisionTest, MatrixCoversAllApolloActionPermPermutations) {
  using action = nvhttp::action_type_t;

  // Mask 0: PERM::_no grants nothing
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::applist, crypto::PERM::_no).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::resume, crypto::PERM::_no).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::_no).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::_no, 1, 1).allowed);

  // List only: PERM::list grants applist, but neither resume nor launch
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::applist, crypto::PERM::list).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::resume, crypto::PERM::list).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::list).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::list, 1, 1).allowed);

  // View only: PERM::view grants applist and resume, but NOT new app launch
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::applist, crypto::PERM::view).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::resume, crypto::PERM::view).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::view, 0, 1).allowed);
  // Apollo nuance: same currently running app allows view-only client to join session
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::view, 42, 42).allowed);
  // Different running app does not permit view-only client
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::view, 42, 99).allowed);
  // Blank or 0 appid does not match running app (blank appid false)
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::view, 42, 0).allowed);

  // Launch only: PERM::launch grants applist, resume, and launch
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::applist, crypto::PERM::launch).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::resume, crypto::PERM::launch).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::launch, 0, 1).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::launch, 42, 42).allowed);

  // Full legacy permissions: PERM::_all grants all actions
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::applist, crypto::PERM::_all).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::resume, crypto::PERM::_all).allowed);
  EXPECT_TRUE(nvhttp::make_allowed_decision(action::launch, crypto::PERM::_all, 0, 1).allowed);

  // Input only permissions: no action permissions granted
  const auto input_only = crypto::PERM::input_mouse | crypto::PERM::input_kbd | crypto::PERM::input_controller;
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::applist, input_only).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::resume, input_only).allowed);
  EXPECT_FALSE(nvhttp::make_allowed_decision(action::launch, input_only).allowed);
}

TEST_F(TLSDispatchTest, ApplistPermissionMatrixEnforcesActionGating) {
  const auto creds_no = crypto::gen_creds("TLS Applist No Perm", 2048);
  const auto creds_input = crypto::gen_creds("TLS Applist Input Only", 2048);
  const auto creds_list = crypto::gen_creds("TLS Applist List Only", 2048);
  const auto creds_view = crypto::gen_creds("TLS Applist View Only", 2048);
  const auto creds_launch = crypto::gen_creds("TLS Applist Launch Only", 2048);
  const auto creds_full = crypto::gen_creds("TLS Applist Full Perm", 2048);

  ASSERT_FALSE(nvhttp::test_support::add_client("NoPerm", creds_no.x509, true, crypto::PERM::_no).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("InputOnly", creds_input.x509, true, crypto::PERM::input_mouse).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ListOnly", creds_list.x509, true, crypto::PERM::list).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ViewOnly", creds_view.x509, true, crypto::PERM::view).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("LaunchOnly", creds_launch.x509, true, crypto::PERM::launch).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("FullPerm", creds_full.x509, true, crypto::PERM::_all).empty());

  // 1. Client with PERM::_no receives Apollo denial XML (status 200 with Permission Denied app)
  {
    tls_client client(port, &creds_no);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("<AppTitle>Permission Denied</AppTitle>"), std::string::npos);
    EXPECT_NE(resp.find("<ID>114514</ID>"), std::string::npos);
  }

  // 2. Client with input-only permissions also receives Apollo denial XML
  {
    tls_client client(port, &creds_input);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("<AppTitle>Permission Denied</AppTitle>"), std::string::npos);
    EXPECT_NE(resp.find("<ID>114514</ID>"), std::string::npos);
  }

  // 3. Client with PERM::list is permitted: status 200, no "Permission Denied" entry
  {
    tls_client client(port, &creds_list);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_EQ(resp.find("Permission Denied"), std::string::npos);
  }

  // 4. Client with PERM::view is permitted under Apollo action group (_all_actions)
  {
    tls_client client(port, &creds_view);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_EQ(resp.find("Permission Denied"), std::string::npos);
  }

  // 5. Client with PERM::launch is permitted
  {
    tls_client client(port, &creds_launch);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_EQ(resp.find("Permission Denied"), std::string::npos);
  }

  // 6. Client with PERM::_all is permitted
  {
    tls_client client(port, &creds_full);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_EQ(resp.find("Permission Denied"), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, ApplistReadsConfiguredAppsForAuthorizedClient) {
  proc::ctx_t test_app {};
  test_app.name = "CustomGame";
  test_app.id = "42";
  proc::proc.get_apps().push_back(std::move(test_app));

  const auto creds_full = crypto::gen_creds("TLS Applist Config Full", 2048);
  const auto creds_no = crypto::gen_creds("TLS Applist Config No", 2048);
  ASSERT_FALSE(nvhttp::test_support::add_client("ConfigFull", creds_full.x509, true, crypto::PERM::_all).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ConfigNo", creds_no.x509, true, crypto::PERM::_no).empty());

  // Authorized client receives custom configured app from apps collection
  {
    tls_client client(port, &creds_full);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("<AppTitle>CustomGame</AppTitle>"), std::string::npos);
    EXPECT_NE(resp.find("<ID>42</ID>"), std::string::npos);
    EXPECT_EQ(resp.find("Permission Denied"), std::string::npos);
  }

  // Denied client receives denial XML and does NOT enumerate custom app
  {
    tls_client client(port, &creds_no);
    const auto resp = client.request("/applist");
    EXPECT_NE(resp.find("<AppTitle>Permission Denied</AppTitle>"), std::string::npos);
    EXPECT_NE(resp.find("<ID>114514</ID>"), std::string::npos);
    EXPECT_EQ(resp.find("CustomGame"), std::string::npos);
  }

  proc::proc.get_apps().clear();
}

TEST_F(TLSDispatchTest, ResumePermissionMatrixEnforcesActionGating) {
  const auto creds_no = crypto::gen_creds("TLS Resume No Perm", 2048);
  const auto creds_list = crypto::gen_creds("TLS Resume List Only", 2048);
  const auto creds_input = crypto::gen_creds("TLS Resume Input Only", 2048);
  const auto creds_view = crypto::gen_creds("TLS Resume View Only", 2048);
  const auto creds_launch = crypto::gen_creds("TLS Resume Launch Only", 2048);
  const auto creds_full = crypto::gen_creds("TLS Resume Full Perm", 2048);

  ASSERT_FALSE(nvhttp::test_support::add_client("NoPerm", creds_no.x509, true, crypto::PERM::_no).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ListOnly", creds_list.x509, true, crypto::PERM::list).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("InputOnly", creds_input.x509, true, crypto::PERM::input_kbd).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ViewOnly", creds_view.x509, true, crypto::PERM::view).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("LaunchOnly", creds_launch.x509, true, crypto::PERM::launch).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("FullPerm", creds_full.x509, true, crypto::PERM::_all).empty());

  // 1. Client with PERM::_no: denied with 403 BEFORE process status check
  {
    tls_client client(port, &creds_no);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
    EXPECT_NE(resp.find("<resume>0</resume>"), std::string::npos);
  }

  // 2. Client with PERM::list (no view/launch): denied with 403
  {
    tls_client client(port, &creds_list);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 3. Client with input only: denied with 403
  {
    tls_client client(port, &creds_input);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 4. Client with PERM::view: passes permission check!
  // Since no app is running in tests (current_appid == 0), handler returns 503 "No running app to resume".
  // This confirms the request was NOT denied by permission (not 403).
  {
    tls_client client(port, &creds_view);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"503\""), std::string::npos);
    EXPECT_NE(resp.find("No running app to resume"), std::string::npos);
  }

  // 5. Client with PERM::launch: also passes permission check (since _allow_view includes launch)
  {
    tls_client client(port, &creds_launch);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"503\""), std::string::npos);
    EXPECT_NE(resp.find("No running app to resume"), std::string::npos);
  }

  // 6. Client with PERM::_all: passes permission check
  {
    tls_client client(port, &creds_full);
    const auto resp = client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"503\""), std::string::npos);
    EXPECT_NE(resp.find("No running app to resume"), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, LaunchPermissionMatrixEnforcesActionGating) {
  const auto creds_no = crypto::gen_creds("TLS Launch No Perm", 2048);
  const auto creds_list = crypto::gen_creds("TLS Launch List Only", 2048);
  const auto creds_input = crypto::gen_creds("TLS Launch Input Only", 2048);
  const auto creds_view = crypto::gen_creds("TLS Launch View Only", 2048);
  const auto creds_launch = crypto::gen_creds("TLS Launch Launch Only", 2048);

  ASSERT_FALSE(nvhttp::test_support::add_client("NoPerm", creds_no.x509, true, crypto::PERM::_no).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ListOnly", creds_list.x509, true, crypto::PERM::list).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("InputOnly", creds_input.x509, true, crypto::PERM::input_mouse).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("ViewOnly", creds_view.x509, true, crypto::PERM::view).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("LaunchOnly", creds_launch.x509, true, crypto::PERM::launch).empty());

  // 1. Client with PERM::_no: denied with 403 BEFORE parameter check or process execution
  {
    tls_client client(port, &creds_no);
    const auto resp = client.request("/launch?appid=1");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 2. Client with PERM::list: denied with 403
  {
    tls_client client(port, &creds_list);
    const auto resp = client.request("/launch?appid=1");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 3. Client with input only: denied with 403
  {
    tls_client client(port, &creds_input);
    const auto resp = client.request("/launch?appid=1");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 4. Client with view only: launching a new app (when current_appid == 0) requires PERM::launch; denied with 403
  {
    tls_client client(port, &creds_view);
    const auto resp = client.request("/launch?appid=1");
    EXPECT_NE(resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resp.find("Permission denied"), std::string::npos);
  }

  // 5. Client with PERM::launch: passes permission check!
  // Without test seam and without launch args, reaches missing required parameter validation (400)
  // proving that permission check succeeded and was NOT 403.
  {
    tls_client client(port, &creds_launch);
    const auto resp = client.request("/launch?appid=1");
    EXPECT_NE(resp.find("status_code=\"400\""), std::string::npos);
    EXPECT_NE(resp.find("Missing a required launch parameter"), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, HTTPQuerySpoofCannotOverrideTLSSnapshot) {
  const auto creds_no = crypto::gen_creds("TLS Spoof Client", 2048);
  ASSERT_FALSE(nvhttp::test_support::add_client("Spoofer", creds_no.x509, true, crypto::PERM::_no).empty());

  // Attempt to spoof full permissions in HTTP query string for applist
  {
    tls_client client(port, &creds_no);
    const auto applist_resp = client.request("/applist?perm=4294967295&permissions=4294967295&action=all");
    EXPECT_NE(applist_resp.find("<AppTitle>Permission Denied</AppTitle>"), std::string::npos);
  }

  // Attempt to spoof permissions in HTTP query string for resume
  {
    tls_client client(port, &creds_no);
    const auto resume_resp = client.request("/resume?perm=4294967295&permissions=4294967295&view=true");
    EXPECT_NE(resume_resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(resume_resp.find("Permission denied"), std::string::npos);
  }

  // Attempt to spoof permissions in HTTP query string for launch
  {
    tls_client client(port, &creds_no);
    const auto launch_resp = client.request("/launch?appid=1&perm=4294967295&permissions=4294967295&launch=true");
    EXPECT_NE(launch_resp.find("status_code=\"403\""), std::string::npos);
    EXPECT_NE(launch_resp.find("Permission denied"), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, LaunchPositiveExecutionThroughNarrowTestSeam) {
  // Documented non-E2E test: verifies production launch handler parses parameters,
  // captures authenticated principal and permissions, and responds 200 without
  // executing live hardware encoders or spawning external processes.
  nvhttp::test_support::skip_hardware_for_testing = true;

  const auto creds_launch = crypto::gen_creds("TLS Positive Launch", 2048);
  const auto creds_full = crypto::gen_creds("TLS Positive Full", 2048);

  ASSERT_FALSE(nvhttp::test_support::add_client("LaunchPositive", creds_launch.x509, true, crypto::PERM::launch).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("FullPositive", creds_full.x509, true, crypto::PERM::_all).empty());

  const std::string valid_launch_query =
    "/launch?rikey=0123456789abcdef0123456789abcdef&rikeyid=1&localAudioPlayMode=0&appid=1";

  // Permitted client with PERM::launch executes handler successfully
  {
    tls_client client(port, &creds_launch);
    const auto resp = client.request(valid_launch_query);
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_NE(resp.find("<gamesession>1</gamesession>"), std::string::npos);
  }

  // Permitted client with PERM::_all executes handler successfully
  {
    tls_client client(port, &creds_full);
    const auto resp = client.request(valid_launch_query);
    EXPECT_NE(resp.find("status_code=\"200\""), std::string::npos);
    EXPECT_NE(resp.find("<gamesession>1</gamesession>"), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, ExclusiveOwnershipNotEnforcedForResume) {
  // Verifies existing Apollo policy: paired clients with _allow_view permission may view/resume
  // without exclusive ownership barriers restricted to a single launching client identity.
  const auto alice_creds = crypto::gen_creds("Alice Viewer", 2048);
  const auto bob_creds = crypto::gen_creds("Bob Viewer", 2048);

  ASSERT_FALSE(nvhttp::test_support::add_client("Alice", alice_creds.x509, true, crypto::PERM::view).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("Bob", bob_creds.x509, true, crypto::PERM::view).empty());

  // Both distinct paired identities reach the resume handler and receive 503 (no app running),
  // proving neither is blocked by an exclusive ownership constraint.
  {
    tls_client alice_client(port, &alice_creds);
    const auto resp = alice_client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"503\""), std::string::npos);
  }
  {
    tls_client bob_client(port, &bob_creds);
    const auto resp = bob_client.request("/resume");
    EXPECT_NE(resp.find("status_code=\"503\""), std::string::npos);
  }
}

TEST_F(TLSDispatchTest, PendingLaunchSessionPurgedOnTerminateSessionsByCert) {
  const auto alice_creds = test_utils::certificates::generate_ca_credentials("Alice Pending Client");
  const auto bob_creds = test_utils::certificates::generate_ca_credentials("Bob Pending Client");

  nvhttp::test_support::add_client("AlicePending", alice_creds.x509, true, crypto::PERM::_all);
  nvhttp::test_support::add_client("BobPending", bob_creds.x509, true, crypto::PERM::_all);

  // 1. Raise a launch session for Alice
  auto launch_alice = std::make_shared<rtsp_stream::launch_session_t>();
  launch_alice->id = 101;
  launch_alice->client_cert = alice_creds.x509;
  launch_alice->perm = crypto::PERM::_all;
  rtsp_stream::launch_session_raise(launch_alice);

  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());
  EXPECT_EQ(rtsp_stream::test_support::pending_launch_session_cert(), alice_creds.x509);

  // 2. Terminating Bob's sessions must NOT clear Alice's pending session
  rtsp_stream::terminate_sessions_by_cert(bob_creds.x509);
  EXPECT_TRUE(rtsp_stream::test_support::has_pending_launch_session());
  EXPECT_EQ(rtsp_stream::test_support::pending_launch_session_cert(), alice_creds.x509);

  // 3. Terminating Alice's sessions clears the pending session
  rtsp_stream::terminate_sessions_by_cert(alice_creds.x509);
  EXPECT_FALSE(rtsp_stream::test_support::has_pending_launch_session());

  // 4. Server clear_all clears any pending launch session
  auto launch_bob = std::make_shared<rtsp_stream::launch_session_t>();
  launch_bob->id = 102;
  launch_bob->client_cert = bob_creds.x509;
  launch_bob->perm = crypto::PERM::_all;
  rtsp_stream::launch_session_raise(launch_bob);
  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  rtsp_stream::test_support::clear_all();
  EXPECT_FALSE(rtsp_stream::test_support::has_pending_launch_session());
}

TEST_F(TLSDispatchTest, RtspAdmissionRevalidationProtectsPendingLaunchAgainstStaleEscalation) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Admission Client");
  const auto uuid = nvhttp::test_support::add_client("AdmissionClient", creds.x509, true, crypto::PERM::_all);
  ASSERT_FALSE(uuid.empty());

  rtsp_stream::launch_session_t session {};
  session.id = 201;
  session.client_cert = creds.x509;
  session.perm = crypto::PERM::_all;  // Stale initial snapshot from earlier /launch authorization

  // 1. Client disabled in registry: record shows enabled=false
  ASSERT_TRUE(nvhttp::update_client(uuid, false, std::nullopt));
  const auto rec_disabled = nvhttp::get_client_record(session.client_cert);
  EXPECT_TRUE(rec_disabled.found);
  EXPECT_FALSE(rec_disabled.enabled);

  // 2. Client re-enabled but permissions reduced to mouse-only (lacks _allow_view)
  const auto mouse_only = crypto::PERM::input_mouse;
  ASSERT_TRUE(nvhttp::update_client(uuid, true, static_cast<uint32_t>(mouse_only)));
  const auto rec_no_view = nvhttp::get_client_record(session.client_cert);
  EXPECT_TRUE(rec_no_view.found);
  EXPECT_TRUE(rec_no_view.enabled);
  EXPECT_FALSE(bool(rec_no_view.perm & crypto::PERM::_allow_view));

  // 3. Client granted view but restricted from controller: permissions revalidate to reduced snapshot
  const auto view_mouse = crypto::PERM::view | crypto::PERM::input_mouse;
  ASSERT_TRUE(nvhttp::update_client(uuid, true, static_cast<uint32_t>(view_mouse)));
  const auto rec_restricted = nvhttp::get_client_record(session.client_cert);
  EXPECT_TRUE(rec_restricted.found);
  EXPECT_TRUE(rec_restricted.enabled);
  EXPECT_TRUE(bool(rec_restricted.perm & crypto::PERM::_allow_view));
  EXPECT_EQ(rec_restricted.perm, view_mouse);
  EXPECT_FALSE(bool(rec_restricted.perm & crypto::PERM::input_controller));

  // Simulating the admission step in rtsp.cpp before stream::session::alloc
  if (!session.client_cert.empty()) {
    const auto client_rec = nvhttp::get_client_record(session.client_cert);
    ASSERT_TRUE(client_rec.found && client_rec.enabled && bool(client_rec.perm & crypto::PERM::_allow_view));
    session.perm = client_rec.perm;
  }
  EXPECT_EQ(session.perm, view_mouse);
}

/**
 * @brief Concurrency barrier and lifecycle test fixture for RTSP stream admission.
 */
class RtspAdmissionBarrierTest : public TLSDispatchTest {
protected:
  /**
   * @brief Configure isolated client state, fake input backend, and injected lifecycle hooks.
   */
  void SetUp() override {
    TLSDispatchTest::SetUp();

    original_input_config_ = config::input;
    config::input.keyboard = true;
    config::input.mouse = true;
    config::input.controller = true;
    config::input.keybindings.clear();
    config::input.key_rightalt_to_key_win = false;
    config::input.key_repeat_delay = std::chrono::milliseconds {0};

    auto platform_input = platf::input();
    if (platform_input) {
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      input::testing::set_platform_input(std::move(platform_input));
    }
    input::testing::reset_keyboard_state();
    input::testing::set_input_task_sink([](std::shared_ptr<input::input_t>) {});

    rtsp_stream::test_support::reset_test_hooks();
    rtsp_stream::test_support::clear_all();

    start_calls_.store(0);
    stop_calls_.store(0);
    join_calls_.store(0);

    rtsp_stream::test_support::set_start_session_hook([this](stream::session_t &, const std::string &) {
      ++start_calls_;
      return 0;
    });
    rtsp_stream::test_support::set_stop_session_hook([this](stream::session_t &) {
      ++stop_calls_;
    });
    rtsp_stream::test_support::set_join_session_hook([this](stream::session_t &) {
      ++join_calls_;
    });
  }

  /**
   * @brief Clean up injected test hooks and restored configuration.
   */
  void TearDown() override {
    rtsp_stream::test_support::stop_test_server();
    rtsp_stream::test_support::clear_all();
    rtsp_stream::test_support::reset_test_hooks();

    input::testing::reset_keyboard_state();
    input::terminate_gamepads();
    input::testing::set_input_task_sink(nullptr);
    input::testing::set_platform_input({});
    config::input = std::move(original_input_config_);

    TLSDispatchTest::TearDown();
  }

  config::input_t original_input_config_;  ///< Original input configuration.
  std::atomic<int> start_calls_ {0};  ///< Invocation count of fake start hook.
  std::atomic<int> stop_calls_ {0};  ///< Invocation count of fake stop hook.
  std::atomic<int> join_calls_ {0};  ///< Invocation count of fake join hook.
};

/**
 * @brief Race condition barrier test:
 *        When admin revokes client A during an in-flight admission paused after registry check,
 *        the admin clear blocks until admission commits, and then immediately stops/joins the new session,
 *        guaranteeing zero active sessions when admin termination returns.
 */
TEST_F(RtspAdmissionBarrierTest, AdminRevocationSerializesWithInFlightAdmissionZeroOrphanSessions) {
  const auto creds_a = test_utils::certificates::generate_ca_credentials("Admission Client A");
  const auto uuid_a = nvhttp::test_support::add_client(
    "ClientA",
    creds_a.x509,
    true,
    crypto::PERM::view | crypto::PERM::input_controller
  );
  ASSERT_FALSE(uuid_a.empty());

  rtsp_stream::launch_session_t launch_a {};
  launch_a.id = 301;
  launch_a.client_cert = creds_a.x509;
  launch_a.perm = crypto::PERM::view | crypto::PERM::input_controller;
  launch_a.unique_id = "test-unique-301";
  launch_a.iv.resize(16);

  stream::config_t stream_config {};

  std::promise<void> admit_holding_lane_barrier;
  std::promise<void> admin_clear_called_barrier;
  std::promise<void> release_alloc_barrier;
  auto release_future = release_alloc_barrier.get_future().share();

  rtsp_stream::test_support::set_pause_before_alloc_hook([&](const rtsp_stream::launch_session_t &) {
    admit_holding_lane_barrier.set_value();
    const auto wait_status = release_future.wait_for(std::chrono::seconds(5));
    if (wait_status != std::future_status::ready) {
      throw std::runtime_error("Pause before alloc barrier timed out waiting for release");
    }
  });

  rtsp_stream::test_support::set_terminate_sessions_hook([&](std::string_view cert) {
    if (cert == creds_a.x509) {
      try {
        admin_clear_called_barrier.set_value();
      } catch (const std::future_error &) {
      }
    }
  });

  auto unblock_guard = util::fail_guard([&release_alloc_barrier]() {
    try {
      release_alloc_barrier.set_value();
    } catch (...) {
    }
  });

  std::atomic<int> admit_result {-1};
  std::jthread admission_thread([&]() {
    admit_result.store(rtsp_stream::test_support::admit(stream_config, launch_a, "127.0.0.1"));
  });

  auto lane_acquired_future = admit_holding_lane_barrier.get_future();
  ASSERT_EQ(lane_acquired_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  std::atomic<bool> admin_returned {false};
  std::jthread admin_thread([&]() {
    const bool updated = nvhttp::update_client(uuid_a, false, std::nullopt);
    EXPECT_TRUE(updated);
    rtsp_stream::terminate_sessions_by_cert(creds_a.x509);
    admin_returned.store(true);
  });

  auto admin_entered_future = admin_clear_called_barrier.get_future();
  ASSERT_EQ(admin_entered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  // Admin clear must be blocked while admission holds the lifecycle lane lock
  EXPECT_FALSE(admin_returned.load());

  // Release admission to finish alloc + commit
  release_alloc_barrier.set_value();
  unblock_guard.disable();

  admission_thread.join();
  EXPECT_EQ(admit_result.load(), 200);
  EXPECT_EQ(start_calls_.load(), 1);

  admin_thread.join();
  EXPECT_TRUE(admin_returned.load());

  // All old sessions for client A must be torn down and purged
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_FALSE(rtsp_stream::test_support::has_session_for_cert(creds_a.x509));
  EXPECT_EQ(stop_calls_.load(), 1);
  EXPECT_EQ(join_calls_.load(), 1);
}

/**
 * @brief When administrative revocation completes before admission attempts to acquire the lane lock,
 *        admission must fail closed and reject with 403 Forbidden.
 */
TEST_F(RtspAdmissionBarrierTest, ClearCompleteBeforeAdmitRejectsRevokedClient) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Revoked Before Client");
  const auto uuid = nvhttp::test_support::add_client("RevokedBefore", creds.x509, true, crypto::PERM::view);
  ASSERT_FALSE(uuid.empty());

  // Admin disables client and terminates all sessions before admission starts
  ASSERT_TRUE(nvhttp::update_client(uuid, false, std::nullopt));
  rtsp_stream::terminate_sessions_by_cert(creds.x509);

  rtsp_stream::launch_session_t launch {};
  launch.id = 302;
  launch.client_cert = creds.x509;
  launch.perm = crypto::PERM::view;
  launch.unique_id = "test-unique-302";
  launch.iv.resize(16);

  stream::config_t stream_config {};
  const int status = rtsp_stream::test_support::admit(stream_config, launch, "127.0.0.1");
  EXPECT_EQ(status, 403);
  EXPECT_EQ(start_calls_.load(), 0);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_FALSE(rtsp_stream::test_support::has_session_for_cert(creds.x509));
}

/**
 * @brief Permission reduced reconnect:
 *        When an existing pairing's permissions are downgraded in the registry,
 *        admission restricts the session permission snapshot to the fresh registry record,
 *        discarding the stale initial launch session mask.
 */
TEST_F(RtspAdmissionBarrierTest, PermissionReducedReconnectRestrictsToFreshRegistrySnapshot) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Downgraded Client");
  const auto full_perms = crypto::PERM::view | crypto::PERM::input_controller | crypto::PERM::input_mouse;
  const auto uuid = nvhttp::test_support::add_client("DowngradedClient", creds.x509, true, full_perms);
  ASSERT_FALSE(uuid.empty());

  const auto reduced_perms = crypto::PERM::view | crypto::PERM::input_mouse;
  ASSERT_TRUE(nvhttp::update_client(uuid, true, static_cast<uint32_t>(reduced_perms)));

  rtsp_stream::launch_session_t launch {};
  launch.id = 303;
  launch.client_cert = creds.x509;
  launch.perm = full_perms;  // Stale initial snapshot
  launch.unique_id = "test-unique-303";
  launch.iv.resize(16);

  stream::config_t stream_config {};
  const int status = rtsp_stream::test_support::admit(stream_config, launch, "127.0.0.1");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(launch.perm, reduced_perms);
  EXPECT_FALSE(bool(launch.perm & crypto::PERM::input_controller));
  EXPECT_TRUE(bool(launch.perm & crypto::PERM::input_mouse));
  EXPECT_TRUE(bool(launch.perm & crypto::PERM::view));

  EXPECT_EQ(start_calls_.load(), 1);
  EXPECT_EQ(rtsp_stream::session_count(), 1);
  EXPECT_TRUE(rtsp_stream::test_support::has_session_for_cert(creds.x509));
}

/**
 * @brief Unknown client or missing certificate must fail closed with 403 Forbidden.
 */
TEST_F(RtspAdmissionBarrierTest, UnknownClientOrMissingCertificateFailsClosed) {
  stream::config_t stream_config {};

  // Case 1: Empty client cert
  rtsp_stream::launch_session_t launch_empty {};
  launch_empty.id = 304;
  launch_empty.client_cert = "";
  launch_empty.perm = crypto::PERM::view;
  launch_empty.unique_id = "test-empty";
  launch_empty.iv.resize(16);
  EXPECT_EQ(rtsp_stream::test_support::admit(stream_config, launch_empty, "127.0.0.1"), 403);
  EXPECT_EQ(start_calls_.load(), 0);
  EXPECT_EQ(rtsp_stream::session_count(), 0);

  // Case 2: Unknown client cert not in registry
  const auto creds_unknown = test_utils::certificates::generate_ca_credentials("Unknown Client");
  rtsp_stream::launch_session_t launch_unknown {};
  launch_unknown.id = 305;
  launch_unknown.client_cert = creds_unknown.x509;
  launch_unknown.perm = crypto::PERM::view;
  launch_unknown.unique_id = "test-unknown";
  launch_unknown.iv.resize(16);
  EXPECT_EQ(rtsp_stream::test_support::admit(stream_config, launch_unknown, "127.0.0.1"), 403);
  EXPECT_EQ(start_calls_.load(), 0);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_FALSE(rtsp_stream::test_support::has_session_for_cert(creds_unknown.x509));
}

/**
 * @brief Distinct certificate sessions remain completely unaffected when another client is terminated.
 */
TEST_F(RtspAdmissionBarrierTest, DistinctCertificateSessionRemainsUnaffected) {
  const auto creds_a = test_utils::certificates::generate_ca_credentials("Client A Distinct");
  const auto creds_b = test_utils::certificates::generate_ca_credentials("Client B Distinct");

  const auto uuid_a = nvhttp::test_support::add_client("ClientA", creds_a.x509, true, crypto::PERM::view);
  const auto uuid_b = nvhttp::test_support::add_client("ClientB", creds_b.x509, true, crypto::PERM::view);
  ASSERT_FALSE(uuid_a.empty());
  ASSERT_FALSE(uuid_b.empty());

  stream::config_t stream_config {};

  rtsp_stream::launch_session_t launch_b {};
  launch_b.id = 306;
  launch_b.client_cert = creds_b.x509;
  launch_b.perm = crypto::PERM::view;
  launch_b.unique_id = "test-306";
  launch_b.iv.resize(16);

  ASSERT_EQ(rtsp_stream::test_support::admit(stream_config, launch_b, "127.0.0.1"), 200);
  EXPECT_EQ(start_calls_.load(), 1);
  EXPECT_EQ(rtsp_stream::session_count(), 1);
  EXPECT_TRUE(rtsp_stream::test_support::has_session_for_cert(creds_b.x509));

  // Admin terminates Client A
  rtsp_stream::terminate_sessions_by_cert(creds_a.x509);

  // Client B remains active and unaffected
  EXPECT_EQ(rtsp_stream::session_count(), 1);
  EXPECT_TRUE(rtsp_stream::test_support::has_session_for_cert(creds_b.x509));
  EXPECT_FALSE(rtsp_stream::test_support::has_session_for_cert(creds_a.x509));
  EXPECT_EQ(stop_calls_.load(), 0);
  EXPECT_EQ(join_calls_.load(), 0);
}

/**
 * @brief Verify that test pause hooks enforce bounded timeout contracts and do not hang indefinitely.
 */
TEST_F(RtspAdmissionBarrierTest, InitialStagePauseHookBoundedTimeoutSafety) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Timeout Safety Client");
  const auto uuid = nvhttp::test_support::add_client("TimeoutClient", creds.x509, true, crypto::PERM::view);
  ASSERT_FALSE(uuid.empty());

  rtsp_stream::launch_session_t launch {};
  launch.id = 307;
  launch.client_cert = creds.x509;
  launch.perm = crypto::PERM::view;
  launch.unique_id = "test-307";
  launch.iv.resize(16);

  stream::config_t stream_config {};

  std::promise<void> unfulfilled_barrier;
  auto unfulfilled_future = unfulfilled_barrier.get_future().share();

  rtsp_stream::test_support::set_pause_before_alloc_hook([&](const rtsp_stream::launch_session_t &) {
    const auto status = unfulfilled_future.wait_for(std::chrono::milliseconds(100));
    if (status != std::future_status::ready) {
      throw std::runtime_error("Bounded wait expired as expected without blocking test runner");
    }
  });

  EXPECT_THROW(
    rtsp_stream::test_support::admit(stream_config, launch, "127.0.0.1"),
    std::runtime_error
  );

  EXPECT_EQ(start_calls_.load(), 0);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
}

/**
 * @brief Multi-TCP ticket snapshot reuse across accepts.
 *
 * @details Moonlight opens a new TCP connection per RTSP transaction (OPTIONS, DESCRIBE, SETUP, etc.)
 *          per moonlight-common-c RtspConnection.c lines 387/407/515. The production handle_accept
 *          flow uses a non-consuming snapshot (peek_pending) under the lifecycle lane so multiple
 *          independent TCP sockets bind to the same pending launch session without popping the ticket
 *          or cancelling the deadline on first accept.
 */
TEST_F(RtspAdmissionBarrierTest, MultiTcpTicketSnapshotReusedAcrossAccepts) {
  const auto creds = test_utils::certificates::generate_ca_credentials("MultiTCP Client");
  nvhttp::test_support::add_client("MultiTcpClient", creds.x509, true, crypto::PERM::_all);

  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->id = 401;
  launch->client_cert = creds.x509;
  launch->perm = crypto::PERM::_all;
  launch->unique_id = "test-multitcp-401";

  rtsp_stream::launch_session_raise(launch);
  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  // Simulate multiple successive TCP accepts (e.g. OPTIONS, then DESCRIBE, then SETUP)
  auto accept_one = rtsp_stream::test_support::simulate_accept_snapshot();
  ASSERT_NE(accept_one, nullptr);
  EXPECT_EQ(accept_one->id, 401u);

  // Ticket must still remain pending for subsequent RTSP negotiation sockets
  EXPECT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  auto accept_two = rtsp_stream::test_support::simulate_accept_snapshot();
  ASSERT_NE(accept_two, nullptr);
  EXPECT_EQ(accept_two->id, 401u);

  // Both accepted sockets reference the exact same underlying launch session
  EXPECT_EQ(accept_one, accept_two);

  auto accept_three = rtsp_stream::test_support::simulate_accept_snapshot();
  EXPECT_EQ(accept_three, accept_one);

  // Snapshot remains peekable until revoked or cleared
  EXPECT_EQ(rtsp_stream::test_support::peek_pending(), accept_one);
}

/**
 * @brief Administrative revocation purges the pending snapshot under the lifecycle lane.
 */
TEST_F(RtspAdmissionBarrierTest, RevocationClearsSnapshotUnderLane) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Revoke Snapshot Client");
  nvhttp::test_support::add_client("RevokeSnapshotClient", creds.x509, true, crypto::PERM::_all);

  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->id = 402;
  launch->client_cert = creds.x509;
  launch->perm = crypto::PERM::_all;
  launch->unique_id = "test-revoke-402";

  rtsp_stream::launch_session_raise(launch);
  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());
  EXPECT_NE(rtsp_stream::test_support::peek_pending(), nullptr);

  rtsp_stream::terminate_sessions_by_cert(creds.x509);

  EXPECT_FALSE(rtsp_stream::test_support::has_pending_launch_session());
  EXPECT_EQ(rtsp_stream::test_support::peek_pending(), nullptr);
  EXPECT_EQ(rtsp_stream::test_support::simulate_accept_snapshot(), nullptr);
}

/**
 * @brief An already-ready expiration callback cannot discard a replacement launch.
 */
TEST_F(RtspAdmissionBarrierTest, StaleExpirationCannotDiscardNewPendingLaunch) {
  auto previous = std::make_shared<rtsp_stream::launch_session_t>();
  previous->id = 901;
  rtsp_stream::launch_session_raise(previous);
  const auto previous_generation = rtsp_stream::test_support::pending_launch_generation();
  rtsp_stream::launch_session_clear(previous->id);
  rtsp_stream::test_support::drain_io_for_testing();
  ASSERT_FALSE(rtsp_stream::test_support::has_pending_launch_session());

  auto replacement = std::make_shared<rtsp_stream::launch_session_t>();
  replacement->id = 902;
  rtsp_stream::launch_session_raise(replacement);
  const auto replacement_generation = rtsp_stream::test_support::pending_launch_generation();
  ASSERT_NE(previous_generation, replacement_generation);
  rtsp_stream::test_support::expire_pending_generation(previous_generation);
  EXPECT_EQ(rtsp_stream::test_support::peek_pending(), replacement);
  rtsp_stream::test_support::expire_pending_generation(replacement_generation);
  EXPECT_FALSE(rtsp_stream::test_support::has_pending_launch_session());
}

/**
 * @brief Launch session clear posted to executor clears pending snapshot without inline lock inversion.
 */
TEST_F(RtspAdmissionBarrierTest, LaunchSessionClearPostsToExecutorAndClearsSnapshot) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Clear Snapshot Client");
  nvhttp::test_support::add_client("ClearSnapshotClient", creds.x509, true, crypto::PERM::_all);

  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->id = 403;
  launch->client_cert = creds.x509;
  launch->perm = crypto::PERM::_all;
  launch->unique_id = "test-clear-403";

  rtsp_stream::launch_session_raise(launch);
  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  // Public launch_session_clear defers to server io_context
  rtsp_stream::launch_session_clear(403);

  // Before drain, the post is queued on the executor
  rtsp_stream::test_support::drain_io_for_testing();

  // After draining executor, pending session is cleared
  EXPECT_FALSE(rtsp_stream::test_support::has_pending_launch_session());
  EXPECT_EQ(rtsp_stream::test_support::peek_pending(), nullptr);
}

/**
 * @brief Multi-TCP wire test verifying separate OPTIONS and DESCRIBE connections on ephemeral loopback.
 *
 * @details Spawns an isolated loopback test listener, raises a launch session, and transmits
 *          real OPTIONS and DESCRIBE RTSP protocol frames over separate TCP connections. Both
 *          connections must be accepted, dispatched to production handlers, and receive 200 OK.
 */
TEST_F(RtspAdmissionBarrierTest, MultiTcpWireOptionsDescribeOverSeparateConnections) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Wire RTSP Client");
  nvhttp::test_support::add_client("WireClient", creds.x509, true, crypto::PERM::_all);

  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->id = 404;
  launch->client_cert = creds.x509;
  launch->perm = crypto::PERM::_all;
  launch->unique_id = "test-wire-404";
  launch->iv.resize(16);

  rtsp_stream::launch_session_raise(launch);
  ASSERT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  boost::system::error_code ec;
  const uint16_t port = rtsp_stream::test_support::bind_loopback_ephemeral(ec);
  ASSERT_EQ(ec.value(), 0);
  ASSERT_GT(port, 0);

  std::atomic<bool> running {true};
  std::jthread io_worker([&]() {
    while (running.load()) {
      rtsp_stream::test_support::drain_io_for_testing();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  boost::asio::io_context client_io;

  // 1. First TCP connection: send OPTIONS request
  {
    boost::asio::ip::tcp::socket sock(client_io);
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});

    const std::string req = "OPTIONS rtsp://127.0.0.1/ RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    boost::asio::write(sock, boost::asio::buffer(req));

    boost::asio::streambuf resp_buf;
    boost::system::error_code read_ec;
    boost::asio::read_until(sock, resp_buf, "\r\n\r\n", read_ec);
    ASSERT_FALSE(read_ec);

    const std::string resp {
      boost::asio::buffers_begin(resp_buf.data()),
      boost::asio::buffers_end(resp_buf.data())
    };
    EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("CSeq: 1"), std::string::npos);

    boost::system::error_code close_ec;
    sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    sock.close(close_ec);
  }

  // Pending launch session must still be intact for subsequent RTSP negotiation
  EXPECT_TRUE(rtsp_stream::test_support::has_pending_launch_session());

  // 2. Second TCP connection: send DESCRIBE request (new socket per Moonlight protocol)
  {
    boost::asio::ip::tcp::socket sock(client_io);
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), port});

    const std::string req = "DESCRIBE rtsp://127.0.0.1/ RTSP/1.0\r\nCSeq: 2\r\n\r\n";
    boost::asio::write(sock, boost::asio::buffer(req));

    boost::asio::streambuf resp_buf;
    boost::system::error_code read_ec;
    boost::asio::read_until(sock, resp_buf, "\r\n\r\n", read_ec);
    ASSERT_FALSE(read_ec);

    const std::string resp {
      boost::asio::buffers_begin(resp_buf.data()),
      boost::asio::buffers_end(resp_buf.data())
    };
    EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("CSeq: 2"), std::string::npos);

    boost::system::error_code close_ec;
    sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    sock.close(close_ec);
  }

  running.store(false);
  io_worker.join();
  rtsp_stream::test_support::stop_test_server();
}

/**
 * @brief Startup failure during admission abandons session without calling join or leaking state.
 *
 * @details When stream::session::start returns -1, admission disposes of the session
 *          via abandon (cleaning input devices) and removes it from slots. It does NOT
 *          invoke stop or join, preventing thread hangs and running_sessions underflow.
 */
TEST_F(RtspAdmissionBarrierTest, AdmissionStartFailureAbandonsSessionWithoutInvokingJoin) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Start Failure Client");
  nvhttp::test_support::add_client("StartFailClient", creds.x509, true, crypto::PERM::_all);

  rtsp_stream::launch_session_t launch {};
  launch.id = 405;
  launch.client_cert = creds.x509;
  launch.perm = crypto::PERM::_all;
  launch.unique_id = "test-start-fail-405";
  launch.iv.resize(16);

  stream::config_t stream_config {};

  // Inject start failure
  rtsp_stream::test_support::set_start_session_hook([this](stream::session_t &, const std::string &) {
    ++start_calls_;
    return -1;
  });

  const int status = rtsp_stream::test_support::admit(stream_config, launch, "127.0.0.1");

  EXPECT_EQ(status, 500);
  EXPECT_EQ(start_calls_.load(), 1);
  // Crucial: neither stop nor join may be invoked on an unstarted failed session
  EXPECT_EQ(stop_calls_.load(), 0);
  EXPECT_EQ(join_calls_.load(), 0);

  // Slot collection must be cleanly cleared
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_FALSE(rtsp_stream::test_support::has_session_for_cert(creds.x509));
}

/**
 * @brief A throwing start removes the tracked session and abandons pre-start input.
 */
TEST_F(RtspAdmissionBarrierTest, ThrowingStartupRollsBackAdmission) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Throwing Startup");
  nvhttp::test_support::add_client("ThrowingStartup", credentials.x509, true, crypto::PERM::_all);
  rtsp_stream::launch_session_t launch {};
  launch.client_cert = credentials.x509;
  launch.perm = crypto::PERM::_all;
  launch.iv.resize(16);
  stream::config_t configuration {};
  std::shared_ptr<input::input_t> allocated_input;
  rtsp_stream::test_support::set_start_session_hook([&](stream::session_t &session, const std::string &) -> int {
    allocated_input = stream::session::input(session);
    throw std::runtime_error("Injected startup exception");
  });
  EXPECT_EQ(rtsp_stream::test_support::admit(configuration, launch, "127.0.0.1"), 500);
  EXPECT_EQ(rtsp_stream::session_count(), 0);
  EXPECT_EQ(stop_calls_.load(), 0);
  EXPECT_EQ(join_calls_.load(), 0);
  ASSERT_TRUE(allocated_input);
  EXPECT_TRUE(input::testing::is_input_stopped(allocated_input));
}

/**
 * @brief Abandoning an unstarted session guarantees STOPPED state and cleans input without worker joins.
 */
TEST_F(RtspAdmissionBarrierTest, StreamSessionAbandonContract) {
  const auto creds = test_utils::certificates::generate_ca_credentials("Abandon Contract Client");

  rtsp_stream::launch_session_t launch {};
  launch.id = 406;
  launch.client_cert = creds.x509;
  launch.perm = crypto::PERM::_all;
  launch.unique_id = "test-abandon-406";
  launch.iv.resize(16);

  stream::config_t stream_config {};
  auto session = stream::session::alloc(stream_config, launch);
  ASSERT_NE(session, nullptr);

  // Initial allocated state is STOPPED
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);

  // Abandon transitions/guarantees state is STOPPED and cleans up input devices
  stream::session::abandon(*session);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);
  EXPECT_EQ(stream::session::input(*session), nullptr);

  // Subsequent stop on abandoned STOPPED session returns immediately without hanging
  stream::session::stop(*session);
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);
}
