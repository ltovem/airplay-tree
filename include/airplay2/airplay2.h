/*!
 * @file airplay2.h
 * @brief Master include for airplay2lib
 *
 * Include this single header to use the library.
 */
#ifndef AIRPLAY2_AIRPLAY2_H
#define AIRPLAY2_AIRPLAY2_H

#include "version.h"
#include "airplay_config.h"
#include "audio_renderer.h"
#include "airplay_session.h"
#include "airplay_server.h"

/*!
 * @mainpage airplay2lib - Cross-platform AirPlay 2 Server Library
 *
 * @section intro Introduction
 * airplay2lib is a C++17 cross-platform library implementing an
 * AirPlay 2-compatible audio receiver server. It supports:
 *   - mDNS/Bonjour service discovery
 *   - RTSP/HTTP control protocol
 *   - RTP audio streaming (ALAC, AAC, PCM)
 *   - Multi-session management
 *   - PIN-code based pairing
 *
 * @section platforms Supported Platforms
 *   - macOS 10.13+
 *   - iOS 11+
 *   - Windows 10+
 *   - Linux (kernel 3.10+)
 *   - Android 5.0+ (NDK)
 *
 * @section quickstart Quick Start
 * @code
 *   #include <airplay2/airplay2.h>
 *   using namespace airplay2;
 *
 *   AirPlayServer::global_init();
 *
 *   ServerConfig cfg;
 *   cfg.device.name = "My Speaker";
 *   cfg.device.port = 7000;
 *   MemoryAudioRenderer renderer;
 *
 *   AirPlayServer server(cfg, {}, &renderer);
 *   server.start();
 *   // ... server runs on background threads
 *   server.stop();
 *
 *   AirPlayServer::global_cleanup();
 * @endcode
 */

#endif // AIRPLAY2_AIRPLAY2_H
