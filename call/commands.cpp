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
        {"exit", CommandKind::Quit},     {"members", CommandKind::Members},
        {"mesh", CommandKind::Mesh},
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

    struct WithArg {
        const char* word;
        CommandKind kind;
        const char* usage;
    };
    static const WithArg kWithArg[] = {
        {"call", CommandKind::Call, "usage: call <name|ip>"},
        {"add", CommandKind::Add, "usage: add <name|ip>"},
        {"relay", CommandKind::Relay, "usage: relay <ip|me>"},
    };
    for (const WithArg& w : kWithArg) {
        if (verb != w.word) continue;
        if (args != 1) {
            cmd.kind = CommandKind::Invalid;
            cmd.error = w.usage;
            return cmd;
        }
        cmd.kind = w.kind;
        cmd.arg = words[1];
        return cmd;
    }

    // text <all|name|ip> <message>: the message is the rest of the line, spaces and all
    if (verb == "text") {
        if (args < 2) {
            cmd.kind = CommandKind::Invalid;
            cmd.error = "usage: text <all|name|ip> <message>";
            return cmd;
        }
        auto skip_space = [&](size_t i) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            return i;
        };
        auto skip_word = [&](size_t i) {
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            return i;
        };
        size_t start = skip_space(skip_word(skip_space(skip_word(skip_space(0)))));
        size_t end = line.size();
        while (end > start && std::isspace(static_cast<unsigned char>(line[end - 1]))) --end;
        cmd.kind = CommandKind::Text;
        cmd.arg = words[1];
        cmd.text = line.substr(start, end - start);
        return cmd;
    }

    cmd.kind = CommandKind::Invalid;
    cmd.error = "unknown command '" + words[0] + "' (type help)";
    return cmd;
}

const char* help_text() {
    return "  help             this list\n"
           "  stats            media and lip-sync statistics\n"
           "  list             peers on this floor (and other floors with --directory)\n"
           "  call <name|ip>   call a peer\n"
           "  accept           answer an incoming call\n"
           "  decline          reject an incoming call\n"
           "  hangup           end the call\n"
           "  quit             end the call and exit (or press q in the video window)\n"
           " group calls (up to 6 people; the one who started the call is the host):\n"
           "  add <name|ip>    host: ring one more person into the call\n"
           "  members          who is in the call, and how media flows\n"
           "  relay <ip|me>    host: send media through a relay (me = run it on this laptop);\n"
           "                   when idle, use a relay for the next call you start\n"
           "  mesh             host: everyone sends to everyone again (the default)\n"
           " text chat:\n"
           "  text all <msg>   to everyone in the call\n"
           "  text <name|ip> <msg>  to one person, in the call or not\n";
}

}  // namespace call