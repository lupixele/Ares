/**
 * @file src/platform/windows/virtual_display_session.cpp
 * @brief Implementation of the streaming-session virtual display seam.
 */

// located in src/platform/windows

// standard includes
#include <memory>
#include <optional>
#include <string>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/windows/session_driver_boundary.h"
#include "src/platform/windows/sudovda_driver.h"
#include "src/platform/windows/virtual_display_session.h"

namespace platf::windows {
  using namespace std::literals;

  namespace {
    /**
     * @brief Authorizer that permits every session.
     *
     * @details Deliberate and temporary: new pairings already default to full permissions, and
     * refusing here would silently remove a working feature. It exists as a seam so a verified
     * permission policy can be substituted without touching the lifecycle code.
     */
    class allow_all_sessions_t final : public isession_authorizer_t {
      public:
        bool authorize_session(const std::string &, const session_display_spec_t &) override {
          return true;
        }
    };

    /**
     * @brief Build the driver boundary backed by the installed SudoVDA driver.
     * @return A boundary ready to allocate, or one whose allocations will report unavailable.
     *
     * @note No driver file is installed, signed or modified. The driver handle is opened once,
     *       here, so that a host without the driver is detected once rather than per session.
     */
    std::shared_ptr<session_driver_boundary_t> make_driver_boundary() {
      auto backend = std::make_shared<sudovda_driver_t>();
      const auto status = backend->open();
      if (status != sudovda_open_status_e::ok) {
        BOOST_LOG(info) << "Virtual display driver unavailable; streaming will use the existing display"sv;
      }
      auto authorizer = std::make_shared<allow_all_sessions_t>();
      return std::make_shared<session_driver_boundary_t>(std::move(authorizer), std::move(backend), 3);
    }

    /**
     * @brief Manager for this process, with its owning session display.
     */
    struct session_state_t {
      std::shared_ptr<session_driver_boundary_t> boundary {make_driver_boundary()};  ///< Driver boundary.
      std::shared_ptr<virtual_display_manager_t> manager;  ///< Manager, created on first use.
      std::optional<std::string> current_session;  ///< Session the manager currently holds a display for.
    };

    /**
     * @brief Process-global state for this seam.
     * @return Reference to the state; never null.
     */
    session_state_t &state() {
      static session_state_t instance;
      return instance;
    }

    /**
     * @brief Process-global manager, lazily created against the process boundary.
     * @return Reference to the manager; never null.
     */
    virtual_display_manager_t &manager() {
      auto &s = state();
      if (!s.manager) {
        s.manager = std::make_shared<virtual_display_manager_t>(s.boundary);
      }
      return *s.manager;
    }

    /**
     * @brief Whether the video configuration enables session virtual displays.
     * @param video Video configuration.
     * @return True when the operator opted in.
     */
    bool enabled(const config::video_t &video) {
      return video.virtual_display;
    }
  }  // namespace

  virtual_display_manager_t &session_virtual_display_manager() {
    return manager();
  }

  void session_virtual_display_begin(const config::video_t &video, const rtsp_stream::launch_session_t &launch_session) {
    if (!enabled(video)) {
      // Feature off: behave exactly as an unpatched host would.
      return;
    }

    // A restart or a second launch of the same session must reuse its display rather than
    // allocating a second one, so the key is the client identity, not the per-connection session id.
    std::string session_key = launch_session.unique_id;
    if (session_key.empty()) {
      session_key = std::to_string(launch_session.id);
    }

    const auto result = manager().acquire(session_key, launch_session.width, launch_session.height, launch_session.fps, 0);

    switch (result) {
      case virtual_display_result_e::created:
        BOOST_LOG(info) << "Virtual display ready for session ["sv << session_key << ']';
        state().current_session = session_key;
        break;
      case virtual_display_result_e::already_present:
        // Reusing an existing display is the correct outcome, not a failure.
        BOOST_LOG(info) << "Existing virtual display reused for session ["sv << session_key << ']';
        state().current_session = session_key;
        break;
      case virtual_display_result_e::unavailable:
        BOOST_LOG(warning) << "No virtual display for session ["sv << session_key << "]; streaming continues"sv;
        break;
      case virtual_display_result_e::rejected:
        BOOST_LOG(warning) << "Session ["sv << session_key << "] was refused a virtual display"sv;
        break;
    }
  }

  void session_virtual_display_end() {
    auto &s = state();
    if (!s.current_session.has_value()) {
      // Nothing was allocated; releasing is a no-op so every teardown path can call this.
      return;
    }

    const auto report = manager().release(*s.current_session);
    if (!report.released) {
      BOOST_LOG(warning) << "Virtual display for session ["sv << *s.current_session
                         << "] could not be confirmed released"sv;
    }
    s.current_session.reset();
  }

  bool session_virtual_display_active() {
    const auto &s = state();
    return s.current_session.has_value();
  }
}  // namespace platf::windows
