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

// Fills o from argv. false = the options can't be used: the front end prints messages, then
// its usage line, and exits. true may still come with messages (options that have no effect).
// name_required = false lets --name be left out (lancall_gui asks for it); a name that is
// given must still be valid.
bool parse_app_args(int argc, char** argv, AppOptions& o, std::vector<std::string>& messages,
                    bool name_required = true);

}  // namespace call
