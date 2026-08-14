/*!
 * @file airplay_config.h
 * @brief AirPlay 2 server configuration types
 */
#ifndef AIRPLAY2_AIRPLAY_CONFIG_H
#define AIRPLAY2_AIRPLAY_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace airplay2 {

/*!
 * @brief Audio output format
 */
enum class AudioFormat : uint8_t {
    PCM16LE = 0,      ///< 16-bit signed little-endian PCM
    PCM24LE = 1,      ///< 24-bit signed little-endian PCM
    PCM32LE = 2,      ///< 32-bit signed little-endian PCM
    PCM_FLOAT = 3,    ///< 32-bit float PCM
    ALAC = 4,         ///< Apple Lossless Audio Codec
    AAC_LC = 5,       ///< AAC Low Complexity
    AAC_ELD = 6,      ///< AAC Enhanced Low Delay
    OPUS = 7          ///< Opus
};

/*!
 * @brief Audio stream configuration
 */
struct AudioConfig {
    uint32_t sample_rate = 44100;     ///< Sample rate in Hz (44100, 48000, etc.)
    uint8_t  channels = 2;            ///< Number of audio channels
    AudioFormat format = AudioFormat::PCM16LE; ///< Output audio format
    uint16_t frame_size = 0;          ///< Frame size (0 = auto)
    uint32_t bitrate = 0;             ///< Bitrate for encoded formats (0 = auto)
};

/*!
 * @brief AirPlay device information for Bonjour/mDNS advertisement
 */
struct DeviceInfo {
    std::string name;                 ///< Display name (e.g., "Living Room Speaker")
    std::string device_id;            ///< Unique device ID (MAC address format: aa:bb:cc:dd:ee:ff)
    std::string model;                ///< Model identifier (e.g., "AppleTV3,2", "AudioAccessory1,2")
    std::string manufacturer = "airplay2lib";
    std::string serial_number;
    uint16_t    port = 7000;          ///< AirPlay control port (default 7000)
    uint32_t    features = 0x5A7FFFF7;///< Feature flags bitmask
    uint8_t     protocol_version = 1; ///< AirPlay protocol version
    bool        supports_audio = true;
    bool        supports_video = false;
    bool        supports_photo = false;
    bool        requires_encryption = false; ///< Require FairPlay encryption
    std::string pin_code;             ///< If set, pairing requires this PIN code
};

/*!
 * @brief Server runtime configuration
 */
struct ServerConfig {
    DeviceInfo device;
    AudioConfig  audio;
    std::string  bind_address = "0.0.0.0"; ///< Bind address (0.0.0.0 for all interfaces)
    uint16_t     control_port = 7000;      ///< HTTP/RTSP control port
    uint16_t     rtp_port_min = 5000;      ///< RTP port range start
    uint16_t     rtp_port_max = 5020;      ///< RTP port range end
    bool         publish_mdns = true;      ///< Advertise via mDNS/Bonjour
    size_t       max_sessions = 8;         ///< Max concurrent sessions
    uint32_t     buffer_ms = 2000;         ///< Audio buffer size in milliseconds
    bool         enable_logging = true;
    int          log_level = 2;            ///< 0=error, 1=warn, 2=info, 3=debug, 4=trace
};

/*!
 * @brief Session statistics
 */
struct SessionStats {
    uint64_t packets_received = 0;
    uint64_t packets_lost = 0;
    uint64_t bytes_received = 0;
    uint32_t current_latency_ms = 0;
    uint32_t jitter_ms = 0;
    uint64_t audio_frames_decoded = 0;
    double   session_duration_sec = 0.0;
    std::string client_ip;
    std::string client_name;
};

/*!
 * @brief Library-wide status codes
 */
enum class Status : int {
    OK = 0,
    ERROR_INVALID_ARGUMENT = -1,
    ERROR_NOT_INITIALIZED = -2,
    ERROR_ALREADY_RUNNING = -3,
    ERROR_NETWORK = -4,
    ERROR_BIND_FAILED = -5,
    ERROR_MDNS = -6,
    ERROR_SESSION_LIMIT = -7,
    ERROR_AUTH_FAILED = -8,
    ERROR_CODEC = -9,
    ERROR_IO = -10,
    ERROR_OUT_OF_MEMORY = -11,
    ERROR_UNSUPPORTED = -12,
    ERROR_TIMEOUT = -13,
    ERROR_UNKNOWN = -99
};

} // namespace airplay2

#endif // AIRPLAY2_AIRPLAY_CONFIG_H
