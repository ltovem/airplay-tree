/*!
 * @brief airplay2_basic_server - Simple AirPlay 2 audio receiver
 *
 * Usage:
 *   airplay2_basic_server [--name "My Speaker"] [--port 7000] [--pin 1234]
 *
 * Captures audio to memory (MemoryAudioRenderer) and prints stats.
 * Extend with your own IAudioRenderer implementation to play audio on hardware.
 */
#include <airplay2/airplay2.h>
#include <iostream>
#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

using namespace airplay2;

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running.store(false); }

struct Args {
    std::string name = "airplay2lib Speaker";
    uint16_t port = 7000;
    std::string pin;
    bool verbose = false;
};

static void print_help(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  -n, --name <name>   Device display name (default: \"airplay2lib Speaker\")\n"
              << "  -p, --port <port>   Control port (default: 7000)\n"
              << "  -k, --pin  <xxxx>   4-digit PIN required to pair (optional)\n"
              << "  -v, --verbose       Verbose logging\n"
              << "  -h, --help          Show help\n";
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "-n" || arg == "--name")    a.name = next();
        else if (arg == "-p" || arg == "--port")  a.port = (uint16_t)std::atoi(next());
        else if (arg == "-k" || arg == "--pin")   a.pin = next();
        else if (arg == "-v" || arg == "--verbose") a.verbose = true;
        else if (arg == "-h" || arg == "--help") { print_help(argv[0]); std::exit(0); }
    }
    return a;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::cout << "airplay2lib basic server v" << AIRPLAY2_VERSION_STRING << "\n";
    std::cout << "Device: " << args.name << "  Port: " << args.port << "\n";
    if (!args.pin.empty()) std::cout << "PIN: " << args.pin << " (required)\n";

    Status init_rc = AirPlayServer::global_init();
    if (init_rc != Status::OK) {
        std::cerr << "global_init failed: " << (int)init_rc << "\n";
        return 1;
    }

    ServerConfig cfg;
    cfg.device.name = args.name;
    cfg.device.port = args.port;
    cfg.control_port = args.port;
    cfg.device.model = "AirPort4,107";
    cfg.device.features = 0x5A7FFFF7;
    // Generate a stable device id based on hostname hash
    {
        std::hash<std::string> h;
        size_t hv = h(args.name);
        char mac[32];
        std::snprintf(mac, sizeof(mac), "AA:BB:CC:%02X:%02X:%02X",
                      (unsigned)(hv & 0xFF),
                      (unsigned)((hv >> 8) & 0xFF),
                      (unsigned)((hv >> 16) & 0xFF));
        cfg.device.device_id = mac;
    }
    if (!args.pin.empty()) {
        cfg.device.requires_encryption = true;
        cfg.device.pin_code = args.pin;
    }
    cfg.bind_address = "0.0.0.0";
    cfg.rtp_port_min = 5000;
    cfg.rtp_port_max = 5100;
    cfg.max_sessions = 8;
    cfg.log_level = args.verbose ? 3 : 2;
    cfg.enable_logging = true;

    MemoryAudioRenderer renderer;
    ServerCallbacks cbs;
    cbs.on_started = [] { std::cout << "[server] Ready. Choose \""; };
    cbs.on_session_connected = [](AirPlaySession& s) {
        std::cout << "[+] client connected: " << s.client_address() << " ("
                  << s.client_name() << ")\n";
    };
    cbs.on_session_disconnected = [](uint64_t id) {
        std::cout << "[-] session " << id << " disconnected\n";
    };
    cbs.on_error = [](Status code, const std::string& msg) {
        std::cerr << "[error " << (int)code << "] " << msg << "\n";
    };
    cbs.on_log = [](int lvl, const std::string& m) {
        if (lvl <= 2) std::cout << m << "\n"; // info and above
    };
    cbs.on_pin_request = [](const std::string& ip, const std::string& pin) {
        std::cout << "  [PIN] client " << ip << " entered pin='" << pin << "'\n";
        return true;
    };

    AirPlayServer server(cfg, std::move(cbs), &renderer);
    Status rc = server.start();
    if (rc != Status::OK) {
        std::cerr << "start() failed: " << (int)rc << "\n";
        AirPlayServer::global_cleanup();
        return 2;
    }
    std::cout << "[server] ready, advertise as \"" << args.name << "\"\n";
    std::cout << "[server] Press Ctrl+C to stop.\n";

    std::signal(SIGINT, on_sigint);
#ifdef _WIN32
    std::signal(SIGBREAK, on_sigint);
#endif

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto ids = server.active_session_ids();
        if (ids.empty()) continue;
        std::cout << "  sessions=" << ids.size()
                  << "  audio_bytes=" << renderer.total_bytes() << "\n";
    }

    std::cout << "[server] shutting down...\n";
    server.stop();
    AirPlayServer::global_cleanup();
    return 0;
}
