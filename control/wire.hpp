#pragma once
// Text-line helpers shared by discovery and signaling (Person 1, control/). Not frozen.
//
// A line is a verb followed by space-separated words. Words with '=' are key=value fields,
// the rest are plain arguments:
//
//   "LANCALL 1 id=3fa9c2d1 name=Aric sig=5011"  ->  verb LANCALL, args {1}, fields {id,name,sig}
//   "PONG t1=10 t2=20 t3=30"                     ->  verb PONG, fields {t1,t2,t3}
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace control::wire {

struct Line {
    std::string verb;
    std::vector<std::string> args;
    std::map<std::string, std::string> fields;
};

// false for an empty line. a trailing '\r' (telnet, some nc builds) is ignored
inline bool parse(std::string text, Line& out) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    out = Line{};
    std::istringstream in(text);
    if (!(in >> out.verb)) return false;
    std::string w;
    while (in >> w) {
        auto eq = w.find('=');
        if (eq == std::string::npos)
            out.args.push_back(w);
        else
            out.fields[w.substr(0, eq)] = w.substr(eq + 1);
    }
    return true;
}

inline std::string get(const Line& l, const std::string& key) {
    auto it = l.fields.find(key);
    return it == l.fields.end() ? std::string() : it->second;
}

// a whole unsigned decimal number that fits in 32 bits
inline bool get_u32(const Line& l, const std::string& key, uint32_t& out) {
    std::string s = get(l, key);
    if (s.empty() || s.size() > 10) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

// Phase 6: free text inside one field. Letters, digits and "-_.~" stay as they are,
// every other byte becomes %XX (so spaces, '=', newlines and UTF-8 all survive the line).
inline std::string escape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// the reverse; false on a broken %XX
inline bool unescape(const std::string& s, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return false;  // needs two hex digits after the %
        int hi = val(s[i + 1]), lo = val(s[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

}  // namespace control::wire