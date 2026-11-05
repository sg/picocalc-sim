#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "misc/cpp/imgui_stdlib.h"

#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/sim.h"
#include "ui.h"

namespace {

SDL_Window *window;
SDL_Renderer *renderer;
float content_scale = 1.0f;
float menu_height;
Uint32 dialog_event;

bool show_settings, show_log, show_keys, show_about;
bool ask_erase;

struct access_point {
    std::string ssid, password;
    bool operator==(const access_point &o) const { return ssid == o.ssid && password == o.password; }
};

enum { FLASH_DEFAULT, FLASH_MEMORY, FLASH_FILE };

// The options the machine reads only as it starts.
struct start_options {
    std::string board, rom, flash_file;
    int flash = FLASH_DEFAULT;
    bool fast = false, trace = false, no_wifi = false;
    std::vector<access_point> aps;
    std::vector<std::string> forwards;

    // Rows left empty are not passed on, so they change nothing.
    bool operator==(const start_options &o) const
    {
        return board == o.board && rom == o.rom && flash == o.flash && flash_file == o.flash_file && fast == o.fast &&
               trace == o.trace && no_wifi == o.no_wifi && named(aps) == named(o.aps) &&
               filled(forwards) == filled(o.forwards);
    }

    static std::vector<access_point> named(std::vector<access_point> v)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [](const access_point &a) { return a.ssid.empty(); }), v.end());
        return v;
    }

    static std::vector<std::string> filled(std::vector<std::string> v)
    {
        v.erase(std::remove(v.begin(), v.end(), std::string()), v.end());
        return v;
    }
};

start_options pending, applied;

// What picocalc_sim_opts points at; kept apart from pending, so editing a setting never moves a
// string that the machine may still read.
struct {
    std::string board, rom, flash, card, shots, wav;
    std::string aps[16], forwards[16];
} store;

std::deque<std::pair<unsigned, std::string>> log_lines;
unsigned log_seen, start_mark;
bool log_scroll;

// When changed settings are next written to the settings file, or 0.
Uint64 save_at;

enum pick_kind { PICK_UF2, PICK_CARD, PICK_WAV, PICK_ROM, PICK_FLASH, PICK_SHOTS };
int prompt_kind = -1;
bool ask_path;
std::string prompt_path;

start_options from_opts()
{
    const picocalc_sim_options_t &o = picocalc_sim_opts;
    start_options s;
    if (o.board)
        s.board = o.board;
    if (o.rom_path)
        s.rom = o.rom_path;
    if (o.flash_path && !strcmp(o.flash_path, "none"))
        s.flash = FLASH_MEMORY;
    else if (o.flash_path) {
        s.flash = FLASH_FILE;
        s.flash_file = o.flash_path;
    }
    s.fast = o.fast;
    s.trace = o.trace;
    s.no_wifi = o.no_wifi;
    for (int i = 0; i < o.wifi_ap_count; i++) {
        const char *ap = o.wifi_aps[i], *colon = strchr(ap, ':');
        if (colon)
            s.aps.push_back({std::string(ap, (size_t)(colon - ap)), colon + 1});
        else
            s.aps.push_back({ap, ""});
    }
    for (int i = 0; i < o.forward_count; i++)
        s.forwards.push_back(o.forwards[i]);
    return s;
}

std::string ap_spec(const access_point &ap)
{
    return ap.password.empty() ? ap.ssid : ap.ssid + ":" + ap.password;
}

void save_soon()
{
    // A slider being dragged changes its setting every frame, so the file waits for it to settle.
    save_at = SDL_GetTicks() + 1000;
}

void remember(const char *key, const char *value)
{
    const char *old = picocalc_sim_settings_get(key);
    if (value ? old && !strcmp(old, value) : !old)
        return;
    picocalc_sim_settings_set(key, value);
    save_soon();
}

void remember_on(const char *key, bool on)
{
    remember(key, on ? "on" : "off");
}

void remember_int(const char *key, int n)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", n);
    remember(key, text);
}

void remember_list(const char *key, const std::vector<std::string> &values)
{
    std::vector<const char *> v;
    for (const std::string &s : values)
        v.push_back(s.c_str());
    picocalc_sim_settings_set_list(key, v.data(), (int)v.size());
    save_soon();
}

// Only what was changed here, so an option given on the command line stays with its own run.
void remember_start_options(const start_options &was, const start_options &now)
{
    if (now.rom != was.rom)
        remember("rom", now.rom.empty() ? nullptr : now.rom.c_str());
    if (now.flash != was.flash || now.flash_file != was.flash_file)
        remember("flash", now.flash == FLASH_MEMORY                          ? "none"
                          : now.flash == FLASH_FILE && !now.flash_file.empty() ? now.flash_file.c_str()
                                                                               : nullptr);
    if (now.fast != was.fast)
        remember_on("fast", now.fast);
    if (now.trace != was.trace)
        remember_on("trace", now.trace);
    if (now.no_wifi != was.no_wifi)
        remember_on("no-wifi", now.no_wifi);
    if (!(start_options::named(now.aps) == start_options::named(was.aps))) {
        std::vector<std::string> specs;
        for (const access_point &ap : start_options::named(now.aps))
            specs.push_back(ap_spec(ap));
        remember_list("wifi-ap", specs);
    }
    if (start_options::filled(now.forwards) != start_options::filled(was.forwards))
        remember_list("forward", start_options::filled(now.forwards));
}

void to_opts(const start_options &s)
{
    picocalc_sim_options_t &o = picocalc_sim_opts;
    store.board = s.board;
    o.board = store.board.empty() ? nullptr : store.board.c_str();
    store.rom = s.rom;
    o.rom_path = store.rom.empty() ? nullptr : store.rom.c_str();
    store.flash = s.flash == FLASH_MEMORY ? "none" : s.flash_file;
    o.flash_path = s.flash == FLASH_DEFAULT || store.flash.empty() ? nullptr : store.flash.c_str();
    o.fast = s.fast;
    o.trace = s.trace;
    o.no_wifi = s.no_wifi;
    o.wifi_ap_count = 0;
    for (const access_point &ap : s.aps) {
        if (ap.ssid.empty() || o.wifi_ap_count == 16)
            continue;
        std::string &spec = store.aps[o.wifi_ap_count];
        spec = ap_spec(ap);
        o.wifi_aps[o.wifi_ap_count++] = spec.c_str();
    }
    o.forward_count = 0;
    for (const std::string &f : s.forwards) {
        if (f.empty() || o.forward_count == 16)
            continue;
        store.forwards[o.forward_count] = f;
        o.forwards[o.forward_count] = store.forwards[o.forward_count].c_str();
        o.forward_count++;
    }
}

void restart(bool erase)
{
    if (picocalc_sim_machine_starts() > 0) {
        // The launch's one-off steps have happened, so from now on a restart is the power switch:
        // the flash keeps what it holds and a recording is not begun again over the old one.
        picocalc_sim_opts.uf2_path = nullptr;
        picocalc_sim_opts.wav_path = nullptr;
    }
    picocalc_sim_opts.erase_flash = erase;
    remember_start_options(applied, pending);
    applied = pending;
    to_opts(applied);
    picocalc_sim_request_restart();
}

const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
    return slash ? slash + 1 : path;
}

void set_battery()
{
    if (picocalc_sim_machine_running())
        southbridge_set_battery(picocalc_sim_picocalc.sb, (uint8_t)picocalc_sim_opts.battery, picocalc_sim_opts.charging);
}

bool card_in()
{
    return picocalc_sim_machine_running() ? sdcard_present(picocalc_sim_picocalc.sd) : picocalc_sim_opts.card_path != nullptr;
}

void insert_card(const char *path)
{
    // A card swapped under the firmware's cached FAT would be written with the old card's tables.
    if (card_in()) {
        picocalc_sim_log("eject the SD card before inserting another");
        return;
    }
    if (picocalc_sim_machine_running() && !sdcard_insert(picocalc_sim_picocalc.sd, path))
        return;
    store.card = path;
    picocalc_sim_opts.card_path = store.card.c_str();
    remember("card", path);
}

void eject_card()
{
    if (picocalc_sim_machine_running())
        sdcard_eject(picocalc_sim_picocalc.sd);
    picocalc_sim_opts.card_path = nullptr;
    remember("card", "none");
    picocalc_sim_log("SD card ejected");
}

void flash_uf2(const char *path)
{
    if (!picocalc_sim_machine_running()) {
        picocalc_sim_log("the machine is not running, so %s cannot be flashed", path);
        return;
    }
    char err[256];
    if (picocalc_sim_flash_uf2(path, err, sizeof(err)))
        picocalc_sim_log("flashing %s", path);
    else
        picocalc_sim_log("%s", err);
}

void record(const char *path)
{
    if (picocalc_sim_machine_running() && picocalc_audio_record(picocalc_sim_picocalc.audio, path)) {
        store.wav = path;
        picocalc_sim_log("recording the sound to %s", path);
    }
}

void stop_recording()
{
    picocalc_audio_close(picocalc_sim_picocalc.audio);
    picocalc_sim_log("saved %s", store.wav.c_str());
}

void set_shots(const char *dir)
{
    store.shots = dir;
    picocalc_sim_opts.screenshot_dir = store.shots.c_str();
}

void choose_shots(const char *dir)
{
    set_shots(dir);
    remember("screenshot-dir", dir);
}

void chose(int kind, const char *path)
{
    switch (kind) {
    case PICK_UF2: flash_uf2(path); break;
    case PICK_CARD: insert_card(path); break;
    case PICK_WAV: record(path); break;
    case PICK_ROM: pending.rom = path; break;
    case PICK_FLASH:
        pending.flash = FLASH_FILE;
        pending.flash_file = path;
        break;
    case PICK_SHOTS: choose_shots(path); break;
    }
}

// SDL may call this on another thread, so the answer goes back through the event queue.
void SDLCALL dialog_done(void *kind, const char *const *files, int filter)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = dialog_event;
    e.user.code = (Sint32)(intptr_t)kind;
    if (!files)
        e.user.data2 = (void *)1;
    else if (files[0])
        e.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&e);
}

const char *pick_title(int kind)
{
    switch (kind) {
    case PICK_UF2: return "UF2 file to flash";
    case PICK_CARD: return "SD card image to insert";
    case PICK_WAV: return "WAV file to record to";
    case PICK_ROM: return "RP2040 boot ROM (b2.elf or a 16 KB image)";
    case PICK_FLASH: return "Flash image";
    case PICK_SHOTS: return "Folder for screenshots";
    }
    return "";
}

void pick(int kind)
{
    static const SDL_DialogFileFilter uf2[] = {{"UF2 firmware", "uf2"}, {"All files", "*"}};
    static const SDL_DialogFileFilter card[] = {{"SD card images", "img;bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter wav[] = {{"WAV audio", "wav"}};
    static const SDL_DialogFileFilter rom[] = {{"RP2040 boot ROM", "elf;bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter flash[] = {{"Flash images", "flash;bin"}, {"All files", "*"}};
    void *ud = (void *)(intptr_t)kind;
    switch (kind) {
    case PICK_UF2: SDL_ShowOpenFileDialog(dialog_done, ud, window, uf2, 2, nullptr, false); break;
    case PICK_CARD: SDL_ShowOpenFileDialog(dialog_done, ud, window, card, 2, picocalc_sim_opts.card_path, false); break;
    case PICK_WAV: SDL_ShowSaveFileDialog(dialog_done, ud, window, wav, 1, nullptr); break;
    case PICK_ROM:
        SDL_ShowOpenFileDialog(dialog_done, ud, window, rom, 2, pending.rom.empty() ? nullptr : pending.rom.c_str(),
                               false);
        break;
    case PICK_FLASH:
        SDL_ShowSaveFileDialog(dialog_done, ud, window, flash, 2,
                               pending.flash_file.empty() ? nullptr : pending.flash_file.c_str());
        break;
    case PICK_SHOTS: SDL_ShowOpenFolderDialog(dialog_done, ud, window, picocalc_sim_opts.screenshot_dir, false); break;
    }
}

void poll_log()
{
    char line[512];
    for (unsigned n = picocalc_sim_log_count(); log_seen != n; log_seen++) {
        if (!picocalc_sim_log_line(log_seen, line, sizeof(line)))
            continue;
        log_lines.emplace_back(log_seen, line);
        if (log_lines.size() > 1000)
            log_lines.pop_front();
        log_scroll = true;
    }
}

const char *machine_status()
{
    static char text[64];
    if (!picocalc_sim_machine_running())
        return "not running";
    switch (picocalc_sim_state()) {
    case PICOCALC_SIM_HALTED: return "halted";
    case PICOCALC_SIM_BOOTSEL: return "BOOTSEL";
    case PICOCALC_SIM_POWERED_OFF: return "off";
    case PICOCALC_SIM_RUNNING: break;
    }
    if (picocalc_sim_speed() < 0.95) {
        snprintf(text, sizeof(text), "%.0f%% speed", picocalc_sim_speed() * 100);
        return text;
    }
    return "";
}

void menu_bar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    menu_height = ImGui::GetWindowSize().y;
    bool running = picocalc_sim_machine_running();
    if (ImGui::BeginMenu("Machine")) {
        if (ImGui::MenuItem("Restart"))
            restart(false);
        ImGui::SetItemTooltip("Switch off and on again, with any changed settings");
        if (ImGui::MenuItem("Reset", "F12", false, running)) {
            picocalc_sim_bootsel_button(false);
            picocalc_sim_reboot("reset button");
        }
        if (ImGui::MenuItem("Reset into BOOTSEL", "Shift+F12", false, running)) {
            picocalc_sim_bootsel_button(true);
            picocalc_sim_reboot("reset button");
        }
        if (ImGui::MenuItem("Power button", nullptr, false, running))
            picocalc_sim_power_button();
        ImGui::Separator();
        if (ImGui::MenuItem("Flash UF2...", nullptr, false, running))
            pick(PICK_UF2);
        if (ImGui::MenuItem("Erase flash and restart..."))
            ask_erase = true;
        ImGui::Separator();
        ImGui::MenuItem("Settings...", nullptr, &show_settings);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
            picocalc_sim_request_quit(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("SD card")) {
        if (card_in()) {
            ImGui::TextDisabled("%s", picocalc_sim_opts.card_path ? base_name(picocalc_sim_opts.card_path) : "card inserted");
            if (ImGui::MenuItem("Eject"))
                eject_card();
        } else if (ImGui::MenuItem("Insert image...")) {
            pick(PICK_CARD);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Sound")) {
        if (ImGui::MenuItem("Mute", nullptr, &picocalc_sim_opts.mute))
            remember_on("mute", picocalc_sim_opts.mute);
        ImGui::SetNextItemWidth(120 * content_scale);
        if (ImGui::SliderInt("Volume", &picocalc_sim_opts.volume, 0, 100, "%d%%"))
            remember_int("volume", picocalc_sim_opts.volume);
        ImGui::Separator();
        if (running && picocalc_audio_recording(picocalc_sim_picocalc.audio)) {
            if (ImGui::MenuItem("Stop recording"))
                stop_recording();
        } else if (ImGui::MenuItem("Record to WAV...", nullptr, false, running)) {
            pick(PICK_WAV);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int n = 1; n <= 4; n++) {
            char label[16];
            snprintf(label, sizeof(label), "%dx", n);
            if (ImGui::MenuItem(label, nullptr, picocalc_sim_opts.scale == n)) {
                picocalc_sim_opts.scale = n;
                ui_fit_window(n);
                remember_int("scale", n);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F11", false, running))
            picocalc_sim_screenshot_auto();
        ImGui::MenuItem("Log", nullptr, &show_log);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Keyboard", nullptr, &show_keys);
        ImGui::MenuItem("About picocalc-sim", nullptr, &show_about);
        ImGui::EndMenu();
    }
    const char *status = machine_status();
    float w = ImGui::CalcTextSize(status).x + ImGui::GetStyle().ItemSpacing.x;
    if (*status && ImGui::GetContentRegionAvail().x > w) {
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status);
    }
    ImGui::EndMainMenuBar();
}

void note(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void place_window(float width)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float margin = 8 * content_scale;
    width = std::min(width * content_scale, vp->Size.x - 2 * margin);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + (vp->Size.y + menu_height) / 2),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->Size.y - menu_height - 2 * margin));
}

bool path_field(const char *label, const char *hint, std::string *path, int kind)
{
    ImGui::PushID(label);
    float button = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemInnerSpacing.x);
    bool changed = ImGui::InputTextWithHint("##path", hint, path);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("Browse..."))
        pick(kind);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

std::string board_label(const picocalc_sim_board_desc_t &d)
{
    char text[128];
    snprintf(text, sizeof(text), "%s (%s, %u MB flash%s%s)", d.name, d.rp2040 ? "RP2040" : "RP2350", d.flash_mb,
             d.psram_mb ? ", PSRAM" : "", d.wifi ? ", WiFi" : "");
    return text;
}

void machine_tab()
{
    ImGui::SeparatorText("At the next restart");
    picocalc_sim_board_desc_t d, chosen = {};
    std::string preview = "(chosen from the UF2)";
    for (int i = 0; picocalc_sim_board_desc(i, &d); i++)
        if (pending.board == d.name) {
            preview = board_label(d);
            chosen = d;
        }
    if (ImGui::BeginCombo("Board", preview.c_str())) {
        for (int i = 0; picocalc_sim_board_desc(i, &d); i++)
            if (ImGui::Selectable(board_label(d).c_str(), pending.board == d.name))
                pending.board = d.name;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Each board keeps its own flash, as if the Pico in the PicoCalc were swapped");

    char def[4096];
    picocalc_sim_data_path(def, sizeof(def), "rp2040-rom.elf", false);
    ImGui::BeginDisabled(!chosen.rp2040 && !pending.board.empty());
    path_field("Boot ROM", def, &pending.rom, PICK_ROM);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The RP2040's own boot ROM, which picocalc-sim cannot include: b2.elf from "
                          "github.com/raspberrypi/pico-bootrom-rp2040/releases");

    static const char *const places[] = {"In the data folder", "In memory until picocalc-sim quits", "In a file"};
    ImGui::Combo("Flash", &pending.flash, places, 3);
    if (pending.flash == FLASH_FILE)
        path_field("Flash image", "a file that holds the whole flash", &pending.flash_file, PICK_FLASH);
    else if (pending.flash == FLASH_DEFAULT) {
        char file[64];
        snprintf(file, sizeof(file), "%s.flash", pending.board.empty() ? "BOARD" : pending.board.c_str());
        picocalc_sim_data_path(def, sizeof(def), file, false);
        note(def);
    }
    ImGui::Checkbox("Instant SPI, I2C and UART transfers", &pending.fast);
    ImGui::Checkbox("Log faults, unknown ROM calls and bus errors", &pending.trace);

    ImGui::SeparatorText("Now");
    if (ImGui::Checkbox("Run as fast as the host allows", &picocalc_sim_opts.unthrottled))
        remember_on("unthrottled", picocalc_sim_opts.unthrottled);
    if (ImGui::Checkbox("Print what the firmware sends on UART0", &picocalc_sim_opts.console))
        remember_on("console", picocalc_sim_opts.console);
}

void wifi_tab()
{
    ImGui::SeparatorText("At the next restart");
    ImGui::Checkbox("Leave the radio out, as if it never answered", &pending.no_wifi);
    ImGui::BeginDisabled(pending.no_wifi);
    ImGui::TextUnformatted("Access points");
    int remove = -1;
    if (!pending.aps.empty() &&
        ImGui::BeginTable("aps", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Network name");
        ImGui::TableSetupColumn("Password");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < pending.aps.size(); i++) {
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##ssid", &pending.aps[i].ssid);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##password", "none: an open network", &pending.aps[i].password);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove"))
                remove = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0)
        pending.aps.erase(pending.aps.begin() + remove);
    if (pending.aps.size() < 16 && ImGui::Button("Add access point"))
        pending.aps.push_back({});
    if (pending.aps.empty())
        note("With none listed, any network name and password the firmware tries will join.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Port forwards");
    remove = -1;
    for (size_t i = 0; i < pending.forwards.size(); i++) {
        ImGui::PushID((int)i + 100);
        float button = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##forward", "[ADDR:]HOSTPORT:PORT", &pending.forwards[i]);
        ImGui::SameLine();
        if (ImGui::Button("Remove"))
            remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0)
        pending.forwards.erase(pending.forwards.begin() + remove);
    if (pending.forwards.size() < 16 && ImGui::Button("Add port forward"))
        pending.forwards.push_back({});
    note("Connections to HOSTPORT on the host (127.0.0.1 unless ADDR is given, * for every interface) reach "
         "the firmware's TCP PORT, as in 2121:21.");
    ImGui::EndDisabled();
}

void devices_tab()
{
    ImGui::SeparatorText("Now");
    if (ImGui::SliderInt("Battery", &picocalc_sim_opts.battery, 0, 100, "%d%%")) {
        set_battery();
        remember_int("battery", picocalc_sim_opts.battery);
    }
    if (ImGui::Checkbox("Charging", &picocalc_sim_opts.charging)) {
        set_battery();
        remember_on("charging", picocalc_sim_opts.charging);
    }
    if (ImGui::SliderInt("Volume", &picocalc_sim_opts.volume, 0, 100, "%d%%"))
        remember_int("volume", picocalc_sim_opts.volume);
    if (ImGui::Checkbox("Mute", &picocalc_sim_opts.mute))
        remember_on("mute", picocalc_sim_opts.mute);
    std::string shots = store.shots;
    if (path_field("Screenshots", "folder for F11's screenshots", &shots, PICK_SHOTS))
        choose_shots(shots.c_str());
}

void settings_window()
{
    place_window(520);
    if (!ImGui::Begin("Settings", &show_settings, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    // Fields fill the window up to their labels, the longest of which is this.
    ImGui::PushItemWidth(-(ImGui::CalcTextSize("Screenshots").x + ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Machine")) {
            machine_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("WiFi")) {
            wifi_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Battery and sound")) {
            devices_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::Separator();
    if (pending == applied) {
        ImGui::TextDisabled("Nothing waits for a restart.");
    } else {
        if (ImGui::Button("Restart now"))
            restart(false);
        ImGui::SameLine();
        if (ImGui::Button("Undo"))
            pending = applied;
        ImGui::SameLine();
        ImGui::TextUnformatted("Some changes wait for a restart.");
    }
    ImGui::End();
}

void log_window()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float margin = 8 * content_scale;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y - margin), ImGuiCond_Appearing,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(560 * content_scale, vp->Size.x - 2 * margin), 200 * content_scale),
                             ImGuiCond_Appearing);
    if (!ImGui::Begin("Log", &show_log, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Copy")) {
        std::string all;
        for (auto &l : log_lines)
            all += l.second + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto &l : log_lines)
        ImGui::TextUnformatted(l.second.c_str());
    if (log_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 2)
        ImGui::SetScrollHereY(1.0f);
    log_scroll = false;
    ImGui::EndChild();
    ImGui::End();
}

void keys_window()
{
    static const char *const rows[][2] = {
        {"F1 to F5", "F1 to F5"},
        {"F6 to F10", "Shift+F1 to Shift+F5"},
        {"Page Up, Page Down", "Shift+Up, Shift+Down"},
        {"Home", "Shift+Tab"},
        {"End", "Shift+Del"},
        {"Insert", "Shift+Enter"},
        {"Pause", "Shift+Esc (Brk)"},
        {"F11", "a screenshot, into the screenshot folder"},
        {"F12", "the reset button"},
        {"Shift+F12", "reset into BOOTSEL"},
    };
    place_window(480);
    if (!ImGui::Begin("Keyboard", &show_keys, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    note("Keys go to the PicoCalc unless one of picocalc-sim's text fields has them.");
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Host key");
        ImGui::TableSetupColumn("PicoCalc");
        ImGui::TableHeadersRow();
        for (auto &r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    note("Dropping a .uf2 file on the window flashes it; dropping a .img file inserts it as the SD card.");
    ImGui::End();
}

void about_window()
{
    place_window(420);
    if (!ImGui::Begin("About picocalc-sim", &show_about, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    int v = SDL_GetVersion();
    ImGui::TextWrapped("picocalc-sim emulates a PicoCalc with an RP2350 or RP2040 board.");
    ImGui::Spacing();
    ImGui::Text("SDL %d.%d.%d (zlib license)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                SDL_VERSIONNUM_MICRO(v));
    ImGui::Text("Dear ImGui %s (MIT license)", IMGUI_VERSION);
    ImGui::TextUnformatted("lwIP (BSD license)");
    ImGui::End();
}

void erase_popup()
{
    if (ask_erase) {
        ImGui::OpenPopup("Erase flash?");
        ask_erase = false;
    }
    if (!ImGui::BeginPopupModal("Erase flash?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const char *board = !pending.board.empty() ? pending.board.c_str() : "the board's";
    ImGui::Text("Everything in %s flash is lost,\nand the machine restarts with it erased.", board);
    if (ImGui::Button("Erase and restart")) {
        restart(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Where the desktop has no file dialog to offer, the path is typed instead.
void path_popup()
{
    if (ask_path) {
        ImGui::OpenPopup("Choose a file");
        ask_path = false;
    }
    if (!ImGui::BeginPopupModal("Choose a file", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(pick_title(prompt_kind));
    ImGui::TextDisabled("No file dialog could be opened, so type the path.");
    ImGui::SetNextItemWidth(420 * content_scale);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##path", &prompt_path, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("OK") || enter) {
        if (!prompt_path.empty())
            chose(prompt_kind, prompt_path.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void stopped_panel()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - menu_height));
    ImGui::Begin("##stopped", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextUnformatted("The machine could not start:");
    ImGui::Spacing();
    for (auto &l : log_lines)
        if (l.first >= start_mark)
            ImGui::TextWrapped("%s", l.second.c_str());
    ImGui::Spacing();
    note("Change what it needs in the settings, then try again.");
    if (ImGui::Button("Settings..."))
        show_settings = true;
    ImGui::SameLine();
    if (ImGui::Button("Try again"))
        restart(false);
    ImGui::End();
}

} // namespace

void ui_init(SDL_Window *w, SDL_Renderer *r)
{
    window = w;
    renderer = r;
    content_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (content_scale < 1.0f)
        content_scale = 1.0f;
    dialog_event = SDL_RegisterEvents(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    // The LCD's text showing through would make the windows hard to read.
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.ScaleAllSizes(content_scale);
    style.FontScaleDpi = content_scale;
    io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForSDLRenderer(w, r);
    ImGui_ImplSDLRenderer3_Init(r);

    pending = applied = from_opts();
    to_opts(applied);
    set_shots(picocalc_sim_opts.screenshot_dir);
    if (picocalc_sim_opts.card_path) {
        store.card = picocalc_sim_opts.card_path;
        picocalc_sim_opts.card_path = store.card.c_str();
    }
    if (picocalc_sim_opts.wav_path)
        store.wav = picocalc_sim_opts.wav_path;

    // The menu bar's height is only known from a frame laid out with the fonts.
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    menu_height = ImGui::GetFrameHeight();
    ImGui::EndFrame();
}

void ui_shutdown(void)
{
    if (save_at)
        picocalc_sim_settings_save();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ui_fit_window(int scale)
{
    float side = PICOCALC_SIM_LCD_WIDTH * scale * content_scale;
    SDL_SetWindowSize(window, (int)side, (int)(side + menu_height));
}

bool ui_event(const SDL_Event *e)
{
    if (e->type == dialog_event) {
        char *path = (char *)e->user.data1;
        if (path) {
            chose(e->user.code, path);
            SDL_free(path);
        } else if (e->user.data2) {
            prompt_kind = e->user.code;
            prompt_path.clear();
            ask_path = true;
        }
        return true;
    }
    bool wanted = ImGui::GetIO().WantCaptureKeyboard;
    // ImGui takes in one key change a frame, so keys typed at the PicoCalc would queue up ahead of
    // the next click on a menu; letting every key-up through clears any key it saw go down.
    if (e->type == SDL_EVENT_KEY_DOWN && !wanted)
        return false;
    ImGui_ImplSDL3_ProcessEvent(e);
    bool key = e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_TEXT_INPUT;
    return key && wanted;
}

void ui_drop_file(const char *path)
{
    size_t n = strlen(path);
    if (n > 4 && SDL_strcasecmp(path + n - 4, ".img") == 0)
        insert_card(path);
    else
        flash_uf2(path);
}

void ui_restarting(void)
{
    start_mark = picocalc_sim_log_count();
}

float ui_frame(void)
{
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    poll_log();
    if (picocalc_sim_machine_running() && applied.board.empty()) {
        // Pinned, so that a restart without the UF2 that chose the board stays on it.
        applied.board = pending.board = picocalc_sim_board_name();
    }
    static unsigned seen_starts;
    if (picocalc_sim_machine_running() && picocalc_sim_machine_starts() != seen_starts) {
        // The next launch comes back to it, as the PicoCalc keeps the Pico it was last given.
        seen_starts = picocalc_sim_machine_starts();
        remember("board", picocalc_sim_board_name());
    }
    if (save_at && SDL_GetTicks() >= save_at) {
        save_at = 0;
        picocalc_sim_settings_save();
    }
    menu_bar();
    if (!picocalc_sim_machine_running())
        stopped_panel();
    if (show_settings)
        settings_window();
    if (show_log)
        log_window();
    if (show_keys)
        keys_window();
    if (show_about)
        about_window();
    erase_popup();
    path_popup();
    ImGui::Render();
    return menu_height * SDL_GetWindowPixelDensity(window);
}

void ui_render(void)
{
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}
