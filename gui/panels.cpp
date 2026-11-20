// lancall_gui's panels (Person 2, gui/, phase 9). See panels.hpp.
#include "gui/panels.hpp"

#include <cstdio>
#include <cstring>

#include "audio/app_util.hpp"
#include "call/commands.hpp"
#include "call/ui_logic.hpp"
#include "common/platform.hpp"
#include "control/discovery.hpp"
#include "imgui.h"

namespace gui {

namespace {

const ImVec4 kChat(0.55f, 0.80f, 1.00f, 1.0f);
const ImVec4 kIncoming(1.00f, 0.85f, 0.30f, 1.0f);
const ImVec4 kTyped(0.60f, 0.60f, 0.60f, 1.0f);
const ImVec4 kWarn(1.00f, 0.45f, 0.40f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);

// a window with a fixed place: the layout is ours, not the person's to drag around
void begin_panel(const char* title, const ImVec2& pos, const ImVec2& size) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
}

// text the person or a peer wrote: never as a format string
void text(const std::string& s) {
    ImGui::TextUnformatted(s.c_str());
}

void text_colored(const ImVec4& color, const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

// the field's text, trimmed of spaces at both ends
std::string field(const char* buf) {
    std::string s(buf);
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

std::string who(const call::App& app) {
    return app.peer_name().empty() ? app.peer_ip() : app.peer_name();
}

// mesh / relay / multicast: for the next call when idle, for this call when we host it
void draw_media_choice(call::App& app, Ui& ui, bool in_call) {
    bool mesh = false, relay_here = false, relay_at = false, multicast = false;
    std::string relay_ip;
    if (in_call) {
        const control::MediaMode mode = app.media_mode();
        relay_ip = app.media_address();
        multicast = mode == control::MediaMode::Multicast;
        relay_here = mode == control::MediaMode::Relay && platform::is_local_address(relay_ip);
        relay_at = mode == control::MediaMode::Relay && !relay_here;
        mesh = mode == control::MediaMode::Mesh;
    } else {
        relay_ip = app.relay_choice();
        multicast = app.multicast_choice();
        relay_here = relay_ip == "me";
        relay_at = !relay_ip.empty() && !relay_here;
        mesh = !multicast && relay_ip.empty();
    }
    if (ImGui::RadioButton("Mesh (everyone to everyone)", mesh) && !mesh) app.mesh();
    if (ImGui::RadioButton("Relay on this laptop", relay_here) && !relay_here) app.relay("me");
    ImGui::RadioButton("Relay at", relay_at);
    ImGui::SameLine();
    if (relay_at && ui.relay_ip[0] == '\0')
        std::snprintf(ui.relay_ip, sizeof ui.relay_ip, "%s", relay_ip.c_str());
    ImGui::SetNextItemWidth(130);
    const bool enter = ImGui::InputText("##relay_ip", ui.relay_ip, sizeof ui.relay_ip,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Use##relay") || enter) && !field(ui.relay_ip).empty())
        app.relay(field(ui.relay_ip));
    if (ImGui::RadioButton("Multicast (this floor only)", multicast) && !multicast) app.multicast();
}

}  // namespace

// ---- messages ------------------------------------------------------------------------------

void add_line(Ui& ui, Line::Kind kind, const std::string& text) {
    ui.lines.push_back({kind, text});
    while (ui.lines.size() > Ui::kMaxLines) ui.lines.pop_front();
    ui.new_lines = true;
}

bool take_notices(call::App& app, Ui& ui) {
    bool incoming = false;
    for (const call::Notice& n : app.take_notices()) {
        Line::Kind kind = Line::Kind::Info;
        if (n.kind == call::Notice::Kind::Chat) kind = Line::Kind::Chat;
        if (n.kind == call::Notice::Kind::Incoming) {
            kind = Line::Kind::Incoming;
            incoming = true;
        }
        add_line(ui, kind, n.text);
        std::printf("%s\n", n.text.c_str());  // started from a terminal: there too
    }
    std::fflush(stdout);
    return incoming;
}

void draw_messages(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size) {
    begin_panel("Messages", pos, size);
    const float box = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##lines", ImVec2(0, -box));
    const bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    for (const Line& l : ui.lines) {
        switch (l.kind) {
            case Line::Kind::Chat:
                text_colored(kChat, l.text);
                break;
            case Line::Kind::Incoming:
                text_colored(kIncoming, l.text);
                break;
            case Line::Kind::Typed:
                text_colored(kTyped, l.text);
                break;
            default:
                ImGui::TextWrapped("%s", l.text.c_str());
        }
    }
    if (ui.new_lines && at_end) ImGui::SetScrollHereY(1.0f);  // follow, unless scrolled up
    ui.new_lines = false;
    ImGui::EndChild();

    // any lancall command, as in the terminal (text, broadcast, watch, stats, ...)
    ImGui::TextUnformatted(">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##command", "a lancall command, e.g. text all hello (help)",
                                 ui.command, sizeof ui.command,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string line = ui.command;
        ui.command[0] = '\0';
        if (!line.empty()) {
            add_line(ui, Line::Kind::Typed, "> " + line);
            const call::Command cmd = call::parse_command(line);
            if (cmd.kind == call::CommandKind::Quit) {
                ui.quit = true;
            } else {
                app.run(cmd);
            }
        }
        ImGui::SetKeyboardFocusHere(-1);  // keep typing
    }
    ImGui::End();
}

// ---- peers ---------------------------------------------------------------------------------

void draw_peers(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size) {
    begin_panel("Peers", pos, size);
    const std::string problem = app.control_error();
    if (!problem.empty()) text_colored(kWarn, problem);

    const control::CallState state = app.state();
    const std::vector<control::Peer> peers = app.peers();
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 2;
    ImGui::BeginChild("##peer_list", ImVec2(0, -bottom));
    if (peers.empty()) {
        ImGui::TextDisabled("no peers yet (beacons every 2 s;");
        ImGui::TextDisabled("other floors: --directory IP)");
    } else if (ImGui::BeginTable("##peers", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("who", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("do", ImGuiTableColumnFlags_WidthFixed);
        for (const control::Peer& p : peers) {
            ImGui::PushID((p.ip + ":" + std::to_string(p.signal_port)).c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(p.name);
            ImGui::TextDisabled("%s%s", p.ip.c_str(), p.remote ? "  other floor" : "");
            if (call::is_broadcasting(p)) text_colored(kGood, "live: " + p.broadcast_title);

            ImGui::TableNextColumn();
            const call::PeerActions a =
                call::peer_actions(p, state, app.is_host(), app.broadcasting(), app.watching());
            if (a.call && ImGui::SmallButton("Call")) app.call_peer(p);
            if (a.add && ImGui::SmallButton("Add")) app.add_peer(p);
            if (call::is_broadcasting(p) && app.watching() && app.watching_name() == p.name) {
                if (ImGui::SmallButton("Stop")) app.unwatch();
            } else if (a.watch && ImGui::SmallButton("Watch")) {
                app.watch(call::watch_target(p, peers));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // by IP: someone on another floor that discovery doesn't list
    const bool adding = state == control::CallState::InCall && app.is_host();
    ImGui::TextDisabled(adding ? "add by name or IP:" : "call by name or IP:");
    ImGui::SetNextItemWidth(-60);
    const bool enter = ImGui::InputText("##call_ip", ui.call_ip, sizeof ui.call_ip,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(adding ? "Add" : "Call") || enter) && !field(ui.call_ip).empty()) {
        if (adding) {
            app.add(field(ui.call_ip));
        } else {
            app.call(field(ui.call_ip));
        }
        ui.call_ip[0] = '\0';
    }
    ImGui::End();
}

// ---- call ----------------------------------------------------------------------------------

void draw_call(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size) {
    begin_panel("Call", pos, size);
    const control::CallState state = app.state();

    if (state == control::CallState::Idle) {
        if (app.broadcasting()) {
            text_colored(kGood, "Broadcasting \"" + app.broadcast_title() + "\"");
            if (ImGui::Button("Stop broadcasting")) app.unbroadcast();
        } else if (app.watching()) {
            text_colored(kGood,
                         "Watching " + app.watching_name() + ": \"" + app.watching_title() + "\"");
            if (ImGui::Button("Stop watching")) app.unwatch();
        } else {
            ImGui::TextUnformatted("Not in a call.");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Calls you start use:");
        draw_media_choice(app, ui, false);
    } else if (state == control::CallState::Calling) {
        text("Calling " + who(app) + "...");
        if (ImGui::Button("Cancel")) app.hang_up();
    } else if (state == control::CallState::Ringing) {
        text_colored(kIncoming, who(app) + " is calling");
    } else {  // InCall
        const std::vector<control::Member> members = app.members();
        if (members.size() > 2) {
            text("Group call, " + std::to_string(members.size()) + " people");
        } else {
            text("In call with " + who(app));
        }
        ImGui::TextDisabled("%s", app.media_parts().c_str());
        ImGui::SameLine();
        if (app.encrypted()) {
            ImGui::TextColored(kGood, "encrypted");
        } else {
            ImGui::TextColored(kWarn, "NOT encrypted");
        }
        if (app.rtt_ms() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("rtt %.1f ms", app.rtt_ms());
        }

        // who's in it, and the code to compare with each (different = someone in between)
        if (!members.empty() &&
            ImGui::BeginTable("##members", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("ip");
            ImGui::TableSetupColumn("safety code");
            ImGui::TableHeadersRow();
            for (const control::Member& m : members) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                text(call::member_label(m) + (m.me ? " (you)" : "") + (m.id == 1 ? " (host)" : ""));
                ImGui::TableNextColumn();
                text(m.ip);
                ImGui::TableNextColumn();
                const std::string code = m.me ? std::string() : app.safety_code(m.id);
                text(code.empty() ? "-" : code);
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (app.is_host()) {
            ImGui::TextDisabled("Media (you're the host):");
            draw_media_choice(app, ui, true);
        } else {
            ImGui::TextDisabled("Media: %s (the host chooses)",
                                call::mode_label(app.media_mode(), app.media_address()).c_str());
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Hang up")) app.hang_up();
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void draw_incoming(call::App& app) {
    const char* kTitle = "Incoming call";
    const bool ringing = app.state() == control::CallState::Ringing;
    if (ringing && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!ringing) {  // answered, declined, or they gave up
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const std::string ip = app.peer_ip();
    text_colored(kIncoming,
                 who(app) + (ip.empty() || who(app) == ip ? "" : " (" + ip + ")") + " is calling");
    if (app.broadcasting()) {
        text_colored(kWarn, "You're broadcasting: stop it before you accept.");
    } else if (app.watching()) {
        ImGui::TextDisabled("Accepting stops watching %s.", app.watching_name().c_str());
    }
    if (ImGui::Button("Accept", ImVec2(110, 0))) app.accept();
    ImGui::SameLine();
    if (ImGui::Button("Decline", ImVec2(110, 0))) app.decline();
    ImGui::EndPopup();
}

// ---- video (9.4 draws the pictures; for now the status line) --------------------------------

void draw_video(call::App& app, const ImVec2& pos, const ImVec2& size) {
    begin_panel("Video", pos, size);
    for (const std::string& s : app.screen().status) text(s);
    ImGui::Spacing();
    ImGui::TextDisabled("(the video shows here from phase 9.4)");
    ImGui::End();
}

// ---- start screen --------------------------------------------------------------------------

bool draw_start(call::AppOptions& opt, const ImVec2& size) {
    static char name[40] = "";
    static char directory[64] = "";
    static bool filled = false;
    static std::string error;
    if (!filled) {  // whatever the command line already gave
        std::snprintf(directory, sizeof directory, "%s", opt.directory.c_str());
        filled = true;
    }
    const ImVec2 box(440, 0);
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.4f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(box);
    ImGui::Begin("Start LanCall", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Your name, as others see it in their list:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool go = ImGui::InputText("name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("Directory laptop (optional, for other floors):");
    go = ImGui::InputText("directory IP", directory, sizeof directory,
                          ImGuiInputTextFlags_EnterReturnsTrue) ||
         go;
    go = ImGui::Button("Start", ImVec2(120, 0)) || go;
    if (!error.empty()) text_colored(kWarn, error);
    ImGui::End();
    if (!go) return false;

    const std::string n = field(name), d = field(directory);
    if (!control::valid_name(n)) {
        error = "name: use 1-32 letters, digits, '-' or '_'";
        return false;
    }
    if (!d.empty() && !audio::app::is_ipv4(d)) {
        error = "directory: give the IPv4 address of the lancall_directory laptop";
        return false;
    }
    opt.name = n;
    opt.directory = d;
    return true;
}

}  // namespace gui
