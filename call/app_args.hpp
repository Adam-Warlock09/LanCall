#pragma once
// lancall's command-line options (Person 2, call/, phase 9), shared by the terminal lancall
// and lancall_gui so both take the same ones. Moved here from call/main.cpp unchanged; the
// messages are returned instead of printed, and the front end prints them (to stderr).
//
//   --name NAME  --peer IP  --directory IP  --relay IP|me  --log PATH  --in N  --out N
//   --camera N  --buffer-ms MS  --fps N  --quality N  --fec  --port N  --mc-loop
//
// (what each does: the top of call/main.cpp)

#include <string>
#include <vector>

#include "call/app.hpp"

namespace call {

// The limits of the media options, for the command line and the GUI's settings alike.
constexpr int kMaxDevice = 1000;  // --in / --out (PortAudio device numbers; -1 = default)
constexpr int kMaxCamera = 63;
constexpr int kMinBufferMs = 20, kMaxBufferMs = 600;
constexpr int kMinFps = 1, kMaxFps = 30;
constexpr int kMinQuality = 1, kMaxQuality = 100;

// Fills o from argv. false = the options can't be used: the front end prints messages, then
// its usage line, and exits. true may still come with messages (options that have no effect).
// name_required = false lets --name be left out (lancall_gui asks for it); a name that is
// given must still be valid.
bool parse_app_args(int argc, char** argv, AppOptions& o, std::vector<std::string>& messages,
                    bool name_required = true);

// Why these media options can't be used ("" = they can): the same limits as the options.
std::string media_options_problem(const MediaOptions& m);

}  // namespace call
