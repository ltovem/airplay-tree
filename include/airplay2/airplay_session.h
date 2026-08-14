/*!
 * @file airplay_session.h
 * @brief AirPlay session public interface
 */
#ifndef AIRPLAY2_AIRPLAY_SESSION_H
#define AIRPLAY2_AIRPLAY_SESSION_H

#include "airplay_config.h"
#include <cstdint>
#include <string>
#include <memory>

namespace airplay2 {

class SessionImpl;
class ServerImpl;

/*!
 * @brief Represents a single AirPlay client connection
 */
class AirPlaySession {
public:
    ~AirPlaySession();

    AirPlaySession(const AirPlaySession&) = delete;
    AirPlaySession& operator=(const AirPlaySession&) = delete;

    /*!
     * @brief Get unique session ID
     */
    uint64_t id() const;

    /*!
     * @brief Get client IP address
     */
    std::string client_address() const;

    /*!
     * @brief Get client name / user-agent
     */
    std::string client_name() const;

    /*!
     * @brief Session state
     */
    enum class State : uint8_t {
        IDLE = 0,
        CONNECTED,
        PAIRING,
        SETUP,
        READY,
        PLAYING,
        PAUSED,
        CLOSED,
        ERROR
    };

    State state() const;

    /*!
     * @brief Session statistics
     */
    SessionStats stats() const;

    /*!
     * @brief Get current audio config for this session
     */
    AudioConfig audio_config() const;

    /*!
     * @brief Disconnect this session
     */
    void disconnect();

private:
    friend class ServerImpl;
    explicit AirPlaySession(SessionImpl* impl);
    SessionImpl* impl_ = nullptr;
    bool owns_impl_   = false;   // for future; currently ServerImpl owns sessions
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_SESSION_H
