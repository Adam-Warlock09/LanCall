// The bottom panel of lancall_gui (Person 2, gui/, phase 9.5). See tabs.hpp.
#include "gui/tabs.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "call/app_args.hpp"
#include "call/ui_logic.hpp"
#include "control/signaling.hpp"
#include "imgui.h"

namespace gui {

namespace {

const ImVec4 kIncoming(0.55f, 0.80f, 1.00f, 1.0f);
const ImVec4 kMine(0.80f, 0.80f, 0.80f, 1.0f);
const ImVec4 kWarn(1.00f, 0.45f, 0.40f, 1.0f);
const ImVec4 kUnread(0.90f, 0.55f, 0.10f, 1.0f);
constexpr double kStatsEveryS = 0.5;

bool idle(const call::App& app) {
    return app.state() == control::CallState::Idle && !app.broadcasting() && !app.watching();
}

// ---- chat ----------------------------------------------------------------------------------

int unread(const call::App& app, const Ui& ui) {
    int n = 0;
    for (const call::ChatLine& c : app.chat()) n += c.seq > ui.chat_seen && !c.mine;
    return n;
}

void chat_tab(call::App& app, Ui& ui) {
    if (app.direct()) {  // chat travels over signaling, which direct mode doesn't start
        ImGui::TextDisabled("No chat in direct mode (--peer): there's no call setup to carry it.");
        return;
    }
    const float bottom = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##chat_lines", ImVec2(0, -bottom));
    const bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (app.chat().empty())
        ImGui::TextDisabled("No messages yet. In a call, \"everyone in the call\" reaches all.");
    for (const call::ChatLine& c : app.chat()) {
        ImGui::TextDisabled("%s", c.time.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, c.failed ? kWarn : c.mine ? kMine : kIncoming);
        ImGui::TextWrapped("%s", call::chat_line_text(c).c_str());
        ImGui::PopStyleColor();
    }
    if (!app.chat().empty() && app.chat().back().seq > ui.chat_seen) {
        ui.chat_seen = app.chat().back().seq;  // looked at
        ui.chat_new = true;
    }
    if (ui.chat_new && at_end) ImGui::SetScrollHereY(1.0f);
    ui.chat_new = false;
    ImGui::EndChild();

    // to whom: the choice is kept by target, so it survives the list changing
    const std::vector<call::Recipient> to = call::chat_recipients(
        app.state() == control::CallState::InCall, app.members(), app.peers());
    if (to.empty()) {
        ImGui::TextDisabled("Nobody to write to yet: no peers found, and not in a call.");
        return;
    }
    size_t pick = 0;
    for (size_t i = 0; i < to.size(); ++i)
        if (to[i].target == ui.chat_target) pick = i;
    ui.chat_target = to[pick].target;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##to", to[pick].label.c_str())) {
        for (size_t i = 0; i < to.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(to[i].label.c_str(), i == pick)) ui.chat_target = to[i].target;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const size_t bytes = std::string(ui.chat_text).size();
    const bool too_long = bytes > control::Signaling::kMaxTextBytes;
    static bool refocus = false;  // after sending: back to the message field
    if (refocus) ImGui::SetKeyboardFocusHere();
    refocus = false;
    ImGui::SetNextItemWidth(-170);
    const bool enter =
        ImGui::InputTextWithHint("##chat_text", "message", ui.chat_text, sizeof ui.chat_text,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextColored(too_long ? kWarn : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%zu/%zu", bytes,
                       control::Signaling::kMaxTextBytes);
    ImGui::SameLine();
    const std::string message = field(ui.chat_text);
    ImGui::BeginDisabled(message.empty() || too_long);
    if ((ImGui::Button("Send") || enter) && !message.empty() && !too_long) {
        app.text(ui.chat_target, message);
        ui.chat_text[0] = '\0';
        refocus = true;
    }
    ImGui::EndDisabled();
}

// ---- stats ---------------------------------------------------------------------------------

void refresh_stats(const call::App& app, Ui& ui) {
    ui.stats.clear();
    std::string state = call::state_name(app.state());
    if (app.state() == control::CallState::InCall) {
        const size_t people = app.members().size();
        if (people > 2) state += " (" + std::to_string(people) + " people)";
        state += ", media " + call::mode_label(app.media_mode(), app.media_address()) +
                 (app.encrypted() ? ", encrypted" : ", NOT encrypted");
    }
    if (app.broadcasting()) state = "broadcasting \"" + app.broadcast_title() + "\"";
    if (app.watching()) state = "watching " + app.watching_name();
    ui.stats.push_back("state: " + state);
    const std::string ip = app.own_ip();
    ui.stats.push_back(
        "this laptop: " + (ip.empty() ? std::string("(address not known yet)") : ip) +
        ", signaling port " + std::to_string(app.options().port));
    ui.stats.push_back("log: " + app.options().log_path);
    for (const std::string& line : app.stats()) ui.stats.push_back("  " + line);
}

void stats_tab(const call::App& app, Ui& ui) {
    const double now = ImGui::GetTime();
    if (ui.stats_at < 0 || now - ui.stats_at >= kStatsEveryS) {
        refresh_stats(app, ui);
        ui.stats_at = now;
    }
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (const std::string& line : ui.stats) all += line + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("refreshed twice a second");
    ImGui::BeginChild("##stats_lines", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : ui.stats) ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();
}

// ---- settings ------------------------------------------------------------------------------

bool same(const call::MediaOptions& a, const call::MediaOptions& b) {
    return a.in_device == b.in_device && a.out_device == b.out_device && a.camera == b.camera &&
           a.buffer_ms == b.buffer_ms && a.fps == b.fps && a.quality == b.quality && a.fec == b.fec;
}

// a microphone or speaker list: "system default", then the devices with channels that way
void device_combo(const char* label, const std::vector<audio::AudioDevice>& devices, bool input,
                  int& chosen) {
    auto name_of = [&](int index) -> std::string {
        if (index < 0) return "system default";
        for (const audio::AudioDevice& d : devices)
            if (d.index == index) return "[" + std::to_string(d.index) + "] " + d.name;
        return "[" + std::to_string(index) + "] (not found)";
    };
    ImGui::SetNextItemWidth(380);
    if (!ImGui::BeginCombo(label, name_of(chosen).c_str())) return;
    if (ImGui::Selectable("system default", chosen < 0)) chosen = -1;
    for (const audio::AudioDevice& d : devices) {
        if ((input ? d.in_channels : d.out_channels) <= 0) continue;
        const bool is_default = input ? d.default_in : d.default_out;
        const std::string item = "[" + std::to_string(d.index) + "] " + d.name + "  (" + d.api +
                                 ")" + (is_default ? "  - default" : "");
        if (ImGui::Selectable(item.c_str(), chosen == d.index)) chosen = d.index;
    }
    ImGui::EndCombo();
}

void settings_tab(call::App& app, Ui& ui) {
    if (!ui.settings_loaded) {
        ui.settings = app.options().media;
        std::snprintf(ui.directory, sizeof ui.directory, "%s", app.options().directory.c_str());
        ui.settings_loaded = true;
    }
    if (!ui.devices_loaded) {
        audio::PaSystem pa;  // PortAudio counts its users: fine while a call runs
        ui.devices = pa.ok() ? audio::PaSystem::devices() : std::vector<audio::AudioDevice>{};
        ui.devices_loaded = true;
    }
    call::MediaOptions& m = ui.settings;

    ImGui::BeginChild("##settings", ImVec2(0, 0));
    ImGui::TextDisabled("Used from the next call, broadcast or watch on:");
    device_combo("microphone", ui.devices, true, m.in_device);
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) ui.devices_loaded = false;
    device_combo("speaker", ui.devices, false, m.out_device);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("camera (0 = the first)", &m.camera);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("audio buffer (ms)", &m.buffer_ms, call::kMinBufferMs, call::kMaxBufferMs);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("frame rate (fps)", &m.fps, call::kMinFps, call::kMaxFps);
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("JPEG quality", &m.quality, call::kMinQuality, call::kMaxQuality);
    ImGui::Checkbox("FEC: a parity packet per video strip (~1 packet in 3 more upload)", &m.fec);

    const std::string problem = call::media_options_problem(m);
    const bool changed = !same(m, app.options().media);
    if (!problem.empty()) ImGui::TextColored(kWarn, "%s", problem.c_str());
    ImGui::BeginDisabled(!problem.empty() || !changed);
    if (ImGui::Button("Apply")) app.set_media_options(m);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Revert")) m = app.options().media;
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Directory laptop (lancall_directory), for peers on other floors:");
    const bool can = idle(app);
    ImGui::BeginDisabled(!can);
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##directory", "IP, or empty", ui.directory, sizeof ui.directory);
    ImGui::SameLine();
    if (ImGui::Button("Use")) app.set_directory(field(ui.directory));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        ui.directory[0] = '\0';
        app.set_directory("");
    }
    ImGui::EndDisabled();
    if (!can) ImGui::TextDisabled("(only when not in a call, broadcasting or watching)");
    ImGui::Text("now: %s", app.options().directory.empty() ? "none (this floor only)"
                                                           : app.options().directory.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Fixed until restart: name %s, signaling port %d, log %s",
                        app.options().name.c_str(), app.options().port,
                        app.options().log_path.c_str());
    ImGui::EndChild();
}

}  // namespace

void draw_bottom(call::App& app, Ui& ui, const ImVec2& pos, const ImVec2& size) {
    begin_panel("##bottom", pos, size, ImGuiWindowFlags_NoTitleBar);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Messages")) {
            draw_messages_tab(app, ui);
            ImGui::EndTabItem();
        }
        const int n = unread(app, ui);
        const std::string chat = n > 0 ? "Chat (" + std::to_string(n) + ")###chat" : "Chat###chat";
        if (n > 0) ImGui::PushStyleColor(ImGuiCol_Tab, kUnread);
        const bool chat_open = ImGui::BeginTabItem(chat.c_str());
        if (n > 0) ImGui::PopStyleColor();
        if (chat_open) {
            chat_tab(app, ui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stats")) {
            stats_tab(app, ui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            settings_tab(app, ui);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace gui
