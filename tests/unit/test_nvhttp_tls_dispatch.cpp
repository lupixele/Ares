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
     * @brief Send another HTTP request on the same TLS connection.
     * @param path Target request URI including query parameters.
     * @return HTTP response body identifying the authorized server-owned principal.
     */
    std::string request(const std::string &path = "/identity?client_name=spoof&client_cert=spoof") {
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
        return {};
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
      return body;
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
    config::nvhttp.file_state = old_file_state;
    config::sunshine.credentials_file = old_credentials_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = old_fresh;

    std::filesystem::remove_all(directory);
    BaseTest::TearDown();
  }

  std::shared_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Production backend under test.
  unsigned short port {};  ///< Ephemeral loopback listener port.
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
