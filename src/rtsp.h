/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace stream {
  struct config_t;
  struct session_t;
}  // namespace stream

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool host_audio;  ///< Whether host audio should be played locally.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_cert;  ///< PEM certificate for the paired Moonlight client.
    crypto::PERM perm {crypto::PERM::_all};  ///< Client permission bitmask snapshot authorized for this session.
  };

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   */
  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session asynchronously via the RTSP server io_context.
   *
   * @details Defers clearance under the server io_context executor to eliminate lock inversion
   *          between stream control_server_t sessions lock and the RTSP lifecycle lane mutex.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @details Synchronized under the dedicated admission lifecycle lane mutex. Upon return,
   *          any matching pending launch session is cancelled and active sessions
   *          are guaranteed stopped and joined. Any in-flight RTSP ANNOUNCE handshake
   *          for this certificate that was in progress prior to return either commits
   *          and is torn down before return, or arrives after return and fails closed
   *          (403 Forbidden) against the updated client registry.
   *          Matching session entries are erased immediately from the slot collection
   *          under the slot snapshot lock before worker threads are joined outside
   *          the slot lock, ensuring slot snapshot inspection does not block.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

#ifdef SUNSHINE_TESTS
  namespace test_support {
    /**
     * @brief Read the identity of the currently armed launch expiration timer.
     * @return Current timer generation.
     */
    uint64_t pending_launch_generation();
    /**
     * @brief Execute the production expiration handler with a specified timer identity.
     * @param generation Captured timer generation to expire.
     */
    void expire_pending_generation(uint64_t generation);
    /**
     * @brief Install a test hook invoked whenever terminate_sessions_by_cert is called.
     *
     * @param hook Callback receiving the client certificate string view.
     */
    void set_terminate_sessions_hook(std::function<void(std::string_view)> hook);

    /**
     * @brief Install a test hook invoked in rtsp_server_t::admit after registry authorization and before stream allocation.
     *
     * @param hook Callback receiving the launch session being admitted.
     */
    void set_pause_before_alloc_hook(std::function<void(const launch_session_t &)> hook);

    /**
     * @brief Install a test hook replacing stream::session::start during admission tests.
     *
     * @param hook Callback invoked with session and peer address, returning 0 on success.
     */
    void set_start_session_hook(std::function<int(stream::session_t &, const std::string &)> hook);

    /**
     * @brief Install a test hook replacing stream::session::stop during teardown tests.
     *
     * @param hook Callback invoked with the session being stopped.
     */
    void set_stop_session_hook(std::function<void(stream::session_t &)> hook);

    /**
     * @brief Install a test hook replacing stream::session::join during teardown tests.
     *
     * @param hook Callback invoked with the session being joined.
     */
    void set_join_session_hook(std::function<void(stream::session_t &)> hook);

    /**
     * @brief Reset all installed test hooks to empty functions.
     */
    void reset_test_hooks();

    /**
     * @brief Check whether a launch session is currently pending.
     *
     * @return True if a pending launch session is waiting for RTSP connection.
     */
    bool has_pending_launch_session();

    /**
     * @brief Query the client certificate of the pending launch session if any.
     *
     * @return PEM certificate of the pending launch session, or empty string.
     */
    std::string pending_launch_session_cert();

    /**
     * @brief Atomically peek at the pending launch session under the lifecycle lock.
     *
     * @return Shared pointer to the pending launch session snapshot, or nullptr if none was pending.
     */
    std::shared_ptr<launch_session_t> peek_pending();

    /**
     * @brief Drain pending queued tasks on the server io_context executor for deterministic tests.
     */
    void drain_io_for_testing();

    /**
     * @brief Bind the RTSP server to an ephemeral port on 127.0.0.1 loopback for testing.
     *
     * @param ec Error code out-parameter.
     * @return Bound ephemeral port number, or 0 on error.
     */
    uint16_t bind_loopback_ephemeral(boost::system::error_code &ec);

    /**
     * @brief Stop the test server and reset its listener.
     */
    void stop_test_server();

    /**
     * @brief Simulate production handle_accept snapshot association under the lifecycle lane.
     *
     * @return The session snapshot bound to the accepted socket, or nullptr.
     */
    std::shared_ptr<launch_session_t> simulate_accept_snapshot();

    /**
     * @brief Clear all RTSP server sessions and pending launch events for testing.
     */
    void clear_all();

    /**
     * @brief Directly exercise the production admission workflow on the singleton RTSP server.
     *
     * @param config Streaming configuration to apply.
     * @param session Launch session information.
     * @param peer_address Client peer IP address.
     * @return Protocol status code (200 on success, 403 on forbidden, 500 on error).
     */
    int admit(stream::config_t &config, launch_session_t &session, const std::string &peer_address);

    /**
     * @brief Check whether an active streaming session exists for the specified certificate.
     *
     * @param cert Client certificate PEM string.
     * @return True if a matching active session is tracked.
     */
    bool has_session_for_cert(std::string_view cert);
  }  // namespace test_support
#endif

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
