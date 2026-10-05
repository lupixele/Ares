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
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>

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
