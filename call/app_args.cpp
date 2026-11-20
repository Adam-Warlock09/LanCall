// lancall's command-line options (Person 2, call/, phase 9). See app_args.hpp.
#include "call/app_args.hpp"

#include "audio/app_util.hpp"
#include "control/discovery.hpp"

namespace call {

bool parse_app_args(int argc, char** argv, AppOptions& o, std::vector<std::string>& messages,
                    bool name_required) {
    using audio::app::parse_int;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--name" && has_value) {
            o.name = argv[++i];
        } else if (a == "--peer" && has_value) {
            o.peer = argv[++i];
        } else if (a == "--directory" && has_value) {
            o.directory = argv[++i];
        } else if (a == "--relay" && has_value) {
            o.relay = argv[++i];
        } else if (a == "--log" && has_value) {
            o.log_path = argv[++i];
        } else if (a == "--in" && has_value) {
            if (!parse_int(argv[++i], 0, 1000, o.media.in_device)) return false;
        } else if (a == "--out" && has_value) {
            if (!parse_int(argv[++i], 0, 1000, o.media.out_device)) return false;
        } else if (a == "--camera" && has_value) {
            if (!parse_int(argv[++i], 0, 63, o.media.camera)) return false;
        } else if (a == "--buffer-ms" && has_value) {
            if (!parse_int(argv[++i], 20, 600, o.media.buffer_ms)) return false;
        } else if (a == "--fps" && has_value) {
            if (!parse_int(argv[++i], 1, 30, o.media.fps)) return false;
        } else if (a == "--quality" && has_value) {
            if (!parse_int(argv[++i], 1, 100, o.media.quality)) return false;
        } else if (a == "--fec") {
            o.media.fec = true;
        } else if (a == "--port" && has_value) {
            if (!parse_int(argv[++i], 1024, 65535, o.port)) return false;
        } else if (a == "--mc-loop") {
            o.mc_loop = true;
        } else {
            return false;
        }
    }
    if ((name_required || !o.name.empty()) && !control::valid_name(o.name)) {
        messages.push_back("--name: use 1-32 letters, digits, '-' or '_'");
        return false;
    }
    if (!o.peer.empty() && !audio::app::is_ipv4(o.peer)) {
        messages.push_back("--peer: give the other laptop's IPv4 address (or 127.0.0.1)");
        return false;
    }
    if (!o.directory.empty() && !audio::app::is_ipv4(o.directory)) {
        messages.push_back("--directory: give the IPv4 address of the lancall_directory laptop");
        return false;
    }
    if (!o.relay.empty() && o.relay != "me" && !audio::app::is_ipv4(o.relay)) {
        messages.push_back("--relay: give the relay laptop's IPv4 address, or me");
        return false;
    }
    if (!o.directory.empty() && !o.peer.empty())
        messages.push_back("--directory has no effect with --peer (direct mode has no discovery)");
    if (!o.relay.empty() && !o.peer.empty())
        messages.push_back("--relay has no effect with --peer (direct mode is always direct)");
    return true;
}

}  // namespace call
