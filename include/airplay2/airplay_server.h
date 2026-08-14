/*!
 * @file airplay_server.h
 * @brief AirPlay 2 server public interface
 */
#ifndef AIRPLAY2_AIRPLAY_SERVER_H
#define AIRPLAY2_AIRPLAY_SERVER_H

#include "airplay_config.h"
#include "airplay_session.h"
#include "audio_renderer.h"
#include <functional>
#include <memory>
#include <vector>

namespace airplay2 {

class ServerImpl;

/*!
 * @brief Event callbacks for server state changes
 */
struct ServerCallbacks {
    /// Server is ready and listening
    std::function<void()> on_started;
    /// Server has stopped
    std::function<void()> on_stopped;
    /// New client connected (session created but not yet authenticated)
    std::function<void(AirPlaySession& session)> on_session_connected;
    /// Session disconnected
    std::function<void(uint64_t session_id)> on_session_disconnected;
    /// PIN code verification required (return true to accept)
    std::function<bool(const std::string& client_address, const std::string& pin)> on_pin_request;
    /// Error occurred
    std::function<void(Status code, const std::string& message)> on_error;
    /// Log message callback (level: 0..4, see ServerConfig)
    std::function<void(int level, const std::string& message)> on_log;
};

/*!
 * @brief AirPlay 2 Server
 *
 * Main entry point. Manages mDNS advertisement, HTTP/RTSP server,
 * RTP audio streams, and AirPlay sessions.
 */
class AirPlayServer {
public:
    /*!
     * @brief Construct with configuration
     * @param config Server configuration
     * @param callbacks Event callbacks (optional)
     * @param renderer Audio renderer for audio playback (required for audio)
     */
    explicit AirPlayServer(const ServerConfig& config,
                           ServerCallbacks callbacks = {},
                           IAudioRenderer* renderer = nullptr);
    ~AirPlayServer();

    AirPlayServer(const AirPlayServer&) = delete;
    AirPlayServer& operator=(const AirPlayServer&) = delete;

    /*!
     * @brief Start the server (bind ports, advertise mDNS, start workers)
     * @return Status OK on success
     */
    Status start();

    /*!
     * @brief Stop the server and disconnect all sessions
     */
    void stop();

    /*!
     * @brief Check if server is currently running
     */
    bool is_running() const;

    /*!
     * @brief Get all active sessions
     */
    std::vector<uint64_t> active_session_ids() const;

    /*!
     * @brief Get a session by ID (returns null if not found)
     */
    AirPlaySession* get_session(uint64_t session_id);

    /*!
     * @brief Get the current server config
     */
    const ServerConfig& config() const;

    /*!
     * @brief Replace audio renderer at runtime
     */
    void set_audio_renderer(IAudioRenderer* renderer);

    /*!
     * @brief Update callbacks at runtime
     */
    void set_callbacks(ServerCallbacks callbacks);

    /*!
     * @brief Global library init (call once at program start)
     *
     * Initializes platform-specific subsystems (Winsock on Windows, etc.)
     * @return Status OK on success
     */
    static Status global_init();

    /*!
     * @brief Global library cleanup (call once at program exit)
     */
    static void global_cleanup();

private:
    std::unique_ptr<ServerImpl> impl_;
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SERVER_H
