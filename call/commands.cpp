#include "call/commands.hpp"

#include <cctype>
#include <sstream>
#include <vector>

namespace call {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Command parse_command(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);

    Command cmd;
    if (words.empty()) return cmd;  // Empty

    const std::string verb = lower(words[0]);
    const size_t args = words.size() - 1;

    struct Simple {
        const char* word;
        CommandKind kind;
    };
    static const Simple kSimple[] = {
        {"help", CommandKind::Help},     {"?", CommandKind::Help},
        {"stats", CommandKind::Stats},   {"list", CommandKind::List},
        {"accept", CommandKind::Accept}, {"decline", CommandKind::Decline},
        {"hangup", CommandKind::Hangup}, {"quit", CommandKind::Quit},
        {"exit", CommandKind::Quit},
    };
    for (const Simple& s : kSimple) {
        if (verb != s.word) continue;
        if (args > 0) {
            cmd.kind = CommandKind::Invalid;
            cmd.error = "'" + verb + "' takes no arguments";
            return cmd;
        }
        cmd.kind = s.kind;
        return cmd;
    }

    if (verb == "call") {
        if (args != 1) {
            cmd.kind = CommandKind::Invalid;
            cmd.error = "usage: call <name|ip>";
            return cmd;
        }
        cmd.kind = CommandKind::Call;
        cmd.arg = words[1];
        return cmd;
    }

    cmd.kind = CommandKind::Invalid;
    cmd.error = "unknown command '" + words[0] + "' (type help)";
    return cmd;
}

const char* help_text() {
    return "  help             this list\n"
           "  stats            media and lip-sync statistics\n"
           "  list             peers found on this floor\n"
           "  call <name|ip>   call a peer\n"
           "  accept           answer an incoming call\n"
           "  decline          reject an incoming call\n"
           "  hangup           end the call\n"
           "  quit             end the call and exit (or press q in the video window)\n";
}

}  // namespace call
