#define NOMINMAX
#include "appytizer-ui/ipc/pipe_client.hpp"
#include "common/config.hpp"
#include "common/constants.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr UINT kTrayMessage = WM_APP + 42, kTrayDisplay = 1, kTrayStopExit = 2, kTrayExit = 3;
constexpr int kRadiusCard = 12, kRadiusButton = 8;
const Fl_Color kBackground = fl_rgb_color(0x0B, 0x0E, 0x14);
const Fl_Color kPanel = fl_rgb_color(0x14, 0x18, 0x21);
const Fl_Color kPanelHover = fl_rgb_color(0x1C, 0x21, 0x2B);
const Fl_Color kBorder = fl_rgb_color(0x24, 0x2A, 0x35);
const Fl_Color kText = fl_rgb_color(0xE7, 0xEC, 0xF3);
const Fl_Color kMuted = fl_rgb_color(0x87, 0x92, 0xA2);
const Fl_Color kAccent = fl_rgb_color(0x22, 0xD3, 0xB0);
const Fl_Color kSuccess = fl_rgb_color(0x34, 0xD3, 0x99);
const Fl_Color kDanger = fl_rgb_color(0xF8, 0x71, 0x71);

class App;
App* g_app{};

Fl_Box* make_label(int x, int y, int w, int h, std::string_view value,
                   Fl_Color color = kText, int size = 13, Fl_Font font = FL_HELVETICA) {
  auto* label = new Fl_Box(x, y, w, h);
  label->copy_label(std::string(value).c_str()); // FLTK otherwise retains a dangling char pointer.
  label->box(FL_NO_BOX); label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  label->labelcolor(color); label->labelsize(size); label->labelfont(font); return label;
}

class Panel final : public Fl_Box {
public: using Fl_Box::Fl_Box;
  void draw() override { fl_color(color()); fl_rounded_rectf(x(), y(), w(), h(), kRadiusCard); fl_color(kBorder); fl_rounded_rect(x(), y(), w(), h(), kRadiusCard); Fl_Box::draw(); }
};
class Button final : public Fl_Button {
public: using Fl_Button::Fl_Button;
  void draw() override { fl_color(value() ? selection_color() : color()); fl_rounded_rectf(x(), y(), w(), h(), kRadiusButton); fl_color(labelcolor()); fl_font(labelfont(), labelsize()); fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER | FL_ALIGN_CLIP); if (Fl::focus() == this) draw_focus(); }
};
Button* make_button(int x, int y, int w, int h, std::string_view value, Fl_Color fill = kPanelHover) {
  auto* button = new Button(x, y, w, h); button->copy_label(std::string(value).c_str());
  button->color(fill); button->selection_color(kAccent); button->labelcolor(fill == kAccent ? kBackground : kText); button->labelsize(12); return button;
}

class ServiceCard final : public Fl_Group {
public:
  ServiceCard(int x, int y, int w, int h, std::string id, std::string title, App* app);
  void update(const nlohmann::json& service);
  void set_disconnected();
  [[nodiscard]] const std::string& id() const { return id_; }
private:
  static void action_callback(Fl_Widget*, void* data);
  std::string id_; App* app_{}; Fl_Box *status_{}, *details_{}; Button* action_{}; bool running_{};
};

class ServiceRow final : public Fl_Group {
public:
  ServiceRow(int x, int y, int w, const nlohmann::json& service, App* app);
  void update(const nlohmann::json& service);
  void set_disconnected();
  [[nodiscard]] const std::string& id() const { return id_; }
private:
  static void action_callback(Fl_Widget*, void* data);
  static void version_callback(Fl_Widget*, void* data);
  std::string id_; App* app_{}; bool running_{}; Fl_Choice* versions_{}; Fl_Box* status_{}; Button* action_{};
};

class SiteRow final : public Fl_Group {
public:
  SiteRow(int x, int y, int w, const nlohmann::json& site, std::string extension);
private:
  static void copy_callback(Fl_Widget*, void* data);
  static void open_callback(Fl_Widget*, void* data);
  std::string url_;
};

LRESULT CALLBACK tray_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
class TrayIcon {
public:
  ~TrayIcon() { remove(); }
  void create() {
    const auto instance = GetModuleHandleW(nullptr); WNDCLASSW klass{};
    klass.lpfnWndProc = tray_proc; klass.hInstance = instance; klass.lpszClassName = L"AppytizerTray";
    RegisterClassW(&klass);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, klass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    data_ = {}; data_.cbSize = sizeof(data_); data_.hWnd = hwnd_; data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data_.uCallbackMessage = kTrayMessage; data_.hIcon = icon_;
    wcscpy_s(data_.szTip, L"Appytizer App Server"); Shell_NotifyIconW(NIM_ADD, &data_);
  }
  void remove() { if (hwnd_) Shell_NotifyIconW(NIM_DELETE, &data_); if (icon_) DestroyIcon(icon_); if (hwnd_) DestroyWindow(hwnd_); hwnd_ = {}; icon_ = {}; }
private: HWND hwnd_{}; HICON icon_{}; NOTIFYICONDATAW data_{};
};

bool set_ui_autostart(bool enabled) {
  constexpr wchar_t key_name[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key_name, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  LONG result{};
  if (enabled) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::wstring command = L"\"" + std::wstring(executable) + L"\"";
    result = RegSetValueExW(key, appytizer::kApplicationId, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else result = RegDeleteValueW(key, appytizer::kApplicationId);
  RegCloseKey(key); return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}
bool ui_autostart_enabled() {
  constexpr wchar_t key_name[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; HKEY key{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, key_name, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  const LONG result = RegQueryValueExW(key, appytizer::kApplicationId, nullptr, nullptr, nullptr, nullptr); RegCloseKey(key); return result == ERROR_SUCCESS;
}

class App {
public:
  App() : config_(store_.load()) { g_app = this; build_ui(); tray_.create(); client_.subscribe([this](std::string event) { handle_event(std::move(event)); }); refresh_all(); }
  ~App() { g_app = nullptr; }
  void show() { window_->show(); window_->take_focus(); }
  void quit() { quitting_ = true; window_->hide(); }
  [[nodiscard]] bool running() const { return !quitting_; }
  void service_action(const std::string& id, bool running, std::string version = {}) {
    const std::string command = id == "dns" ? (running ? "dns.stop" : "dns.start") : (running ? "service.stop" : "service.start");
    nlohmann::json params = nlohmann::json::object(); if (id != "dns") { params["service"] = id; params["version"] = version; }
    client_.request(command, params.dump(), [this](std::string response) { apply_command_state(std::move(response)); });
  }
  void select_version(const std::string& id, const std::string& version) {
    client_.request("service.set_version", nlohmann::json{{"service", id}, {"version", version}}.dump(), [this](std::string response) { apply_command_state(std::move(response)); });
  }
  void stop_and_exit() { client_.request("stop_all", "{}", [this](std::string) { quit(); }); }

private:
  appytizer::ConfigStore store_; appytizer::AppConfig config_; appytizer::PipeClient client_; TrayIcon tray_;
  Fl_Double_Window* window_{}; std::array<Fl_Group*, 4> views_{}; std::array<Button*, 4> nav_{};
  std::vector<ServiceCard*> dashboard_cards_; std::vector<ServiceRow*> service_rows_; Fl_Box *engine_status_{}, *sites_empty_{};
  Fl_Scroll *services_scroll_{}, *sites_scroll_{}; Fl_Input *root_input_{}, *extension_input_{};
  Fl_Check_Button *minimized_{}, *autostart_{}; bool quitting_{}; int active_view_{};

  static void nav_callback(Fl_Widget*, void* data) { auto* pair = static_cast<std::pair<App*, int>*>(data); pair->first->show_view(pair->second); }
  static void rescan_callback(Fl_Widget*, void* data) { auto* app = static_cast<App*>(data); app->client_.request("service.rescan", "{}", [app](std::string) { app->refresh_services(true); }); }
  static void sites_rescan_callback(Fl_Widget*, void* data) { auto* app = static_cast<App*>(data); app->client_.request("sites.rescan", "{}", [app](std::string) { app->refresh_sites(); }); }
  static void save_callback(Fl_Widget*, void* data) { static_cast<App*>(data)->save_settings(); }
  static void browse_callback(Fl_Widget*, void* data) { static_cast<App*>(data)->browse(); }

  void build_ui() {
    Fl::background(0x0B, 0x0E, 0x14); Fl::foreground(0xE7, 0xEC, 0xF3); Fl::set_font(FL_FREE_FONT, "Segoe UI");
    window_ = new Fl_Double_Window(1000, 640, "Appytizer App Server"); window_->color(kBackground); window_->resizable(window_);
    window_->callback([](Fl_Widget* widget, void*) { widget->hide(); });
    window_->begin();
    auto* sidebar = new Fl_Group(0, 0, 88, 640); sidebar->box(FL_FLAT_BOX); sidebar->color(kPanel); sidebar->begin();
    make_label(16, 18, 56, 28, "AP", kAccent, 18, FL_BOLD);
    constexpr std::array<const char*, 4> names{"Home", "Services", "Sites", "Settings"};
    for (int i = 0; i < 4; ++i) {
      nav_[i] = make_button(8, 76 + i * 54, 72, 40, names[i], i == 0 ? kPanelHover : kPanel);
      nav_[i]->labelcolor(i == 0 ? kAccent : kMuted);
      nav_[i]->callback(nav_callback, new std::pair<App*, int>(this, i));
    }
    sidebar->end();
    build_dashboard(); build_services(); build_sites(); build_settings();
    for (int i = 1; i < 4; ++i) views_[i]->hide();
    window_->end();
  }
  void begin_view(int index, std::string_view title, std::string_view subtitle) {
    views_[index] = new Fl_Group(88, 0, 912, 640); views_[index]->begin();
    make_label(120, 24, 840, 32, title, kText, 22, FL_BOLD); make_label(120, 58, 840, 22, subtitle, kMuted, 12);
  }
  void build_dashboard() {
    begin_view(0, "Dashboard", "Live status from the Appytizer Engine");
    engine_status_ = make_label(120, 86, 840, 24, "Connecting to Appytizer Engine…", kMuted, 12);
    const std::array<std::pair<const char*, const char*>, 6> services{{{"dns","Local DNS"},{"nginx","nginx"},{"php","PHP"},{"mysql","MySQL"},{"postgres","PostgreSQL"},{"mongodb","MongoDB"}}};
    for (std::size_t i = 0; i < services.size(); ++i)
      dashboard_cards_.push_back(new ServiceCard(120 + static_cast<int>(i % 3) * 280, 124 + static_cast<int>(i / 3) * 206, 256, 178, services[i].first, services[i].second, this));
    views_[0]->end();
  }
  void build_services() {
    begin_view(1, "Services", "Detected from Windows services, registry install locations, common folders, and PATH");
    auto* rescan = make_button(800, 28, 160, 34, "Rescan now"); rescan->callback(rescan_callback, this);
    services_scroll_ = new Fl_Scroll(120, 96, 840, 512); services_scroll_->box(FL_NO_BOX); services_scroll_->end();
    views_[1]->end();
  }
  void build_sites() {
    begin_view(2, "Sites", "Every direct subfolder of the configured root becomes a local site");
    auto* rescan = make_button(800, 28, 160, 34, "Rescan folders"); rescan->callback(sites_rescan_callback, this);
    sites_scroll_ = new Fl_Scroll(120, 96, 840, 512); sites_scroll_->box(FL_NO_BOX); sites_scroll_->begin();
    sites_empty_ = make_label(144, 128, 760, 56, "No sites found. Choose a root folder in Settings, then rescan.", kMuted, 13);
    sites_scroll_->end(); views_[2]->end();
  }
  void build_settings() {
    begin_view(3, "Settings", "Changes are sent to the Engine and applied immediately");
    make_label(120, 104, 180, 22, "Projects root folder", kMuted, 12, FL_BOLD);
    root_input_ = new Fl_Input(120, 132, 650, 38); root_input_->color(kPanel); root_input_->textcolor(kText); root_input_->value(config_.root_folder.string().c_str());
    auto* browse_button = make_button(786, 132, 174, 38, "Choose folder"); browse_button->callback(browse_callback, this);
    make_label(120, 198, 180, 22, "Local domain suffix", kMuted, 12, FL_BOLD);
    extension_input_ = new Fl_Input(120, 226, 280, 38); extension_input_->color(kPanel); extension_input_->textcolor(kText); extension_input_->value(config_.extension.c_str());
    autostart_ = new Fl_Check_Button(120, 298, 360, 28, "Run Appytizer UI when I sign in"); autostart_->labelcolor(kText); autostart_->value(ui_autostart_enabled());
    minimized_ = new Fl_Check_Button(120, 338, 360, 28, "Start the UI minimized to tray"); minimized_->labelcolor(kText); minimized_->value(config_.run_minimized);
    make_label(120, 378, 700, 36, "The Engine is a separate Windows Service; these options only affect this UI.", kMuted, 11);
    auto* save = make_button(120, 440, 280, 40, "Save and apply", kAccent); save->callback(save_callback, this);
    views_[3]->end();
  }
  void show_view(int index) {
    active_view_ = index;
    for (int i = 0; i < 4; ++i) { if (i == index) views_[i]->show(); else views_[i]->hide(); nav_[i]->color(i == index ? kPanelHover : kPanel); nav_[i]->labelcolor(i == index ? kAccent : kMuted); nav_[i]->redraw(); }
    if (index == 1) refresh_services(true); if (index == 2) refresh_sites(); window_->redraw();
  }
  void refresh_all() { refresh_services(); refresh_sites(); }
  void apply_service_status(const nlohmann::json& services) {
    engine_status_->copy_label("● Engine connected"); engine_status_->labelcolor(kSuccess);
    for (const auto& service : services)
      for (auto* card : dashboard_cards_)
        if (card->id() == service.value("id", "")) card->update(service);
    for (const auto& service : services)
      for (auto* row : service_rows_)
        if (row->id() == service.value("id", "")) row->update(service);
    engine_status_->redraw();
  }
  void apply_command_state(std::string response) {
    try {
      const auto document = nlohmann::json::parse(response);
      if (document.value("ok", false) && document.contains("result") && document["result"].is_array())
        apply_service_status(document["result"]);
    } catch (...) {}
  }
  void handle_event(std::string event) {
    try {
      const auto document = nlohmann::json::parse(event);
      const std::string type = document.value("event", "");
      if (type == "engine.connected") refresh_all();
      else if (type == "engine.disconnected") {
        engine_status_->copy_label("● Engine disconnected"); engine_status_->labelcolor(kDanger); engine_status_->redraw();
        for (auto* card : dashboard_cards_) card->set_disconnected();
        for (auto* row : service_rows_) row->set_disconnected();
      } else if (type == "status.update") apply_service_status(document.at("services"));
      else if (type == "sites.changed" && active_view_ == 2) populate_sites(document.at("sites"));
    } catch (...) {}
  }
  void refresh_services(bool rebuild_services = false) {
    client_.request("service.list", "{}", [this, rebuild_services](std::string response) {
      try {
        const auto document = nlohmann::json::parse(response);
        if (!document.value("ok", false)) throw std::runtime_error(document.value("error", "Engine unavailable"));
        const auto& services = document.at("result");
        apply_service_status(services);
        if (rebuild_services) populate_services(services);
      } catch (const std::exception& error) {
        const std::string message = std::string("● Engine disconnected — start AppytizerEngine.exe (details: ") + error.what() + ")";
        engine_status_->copy_label(message.c_str()); engine_status_->labelcolor(kDanger); engine_status_->redraw();
      }
    });
  }
  void populate_services(const nlohmann::json& services) {
    services_scroll_->clear(); service_rows_.clear(); services_scroll_->begin(); int y = 104;
    for (const auto& service : services) { if (service.value("id", "") == "dns") continue; service_rows_.push_back(new ServiceRow(128, y, 808, service, this)); y += 112; }
    services_scroll_->end(); services_scroll_->redraw();
  }
  void refresh_sites() {
    client_.request("sites.list", "{}", [this](std::string response) {
      try {
        const auto document = nlohmann::json::parse(response); if (!document.value("ok", false)) return;
        populate_sites(document.at("result"));
      } catch (...) {}
    });
  }
  void populate_sites(const nlohmann::json& sites) {
    sites_scroll_->clear(); sites_scroll_->begin(); int y = 104;
    if (sites.empty()) sites_empty_ = make_label(144, 128, 760, 56, "No sites found. Choose a root folder in Settings, then rescan.", kMuted, 13);
    else for (const auto& site : sites) { new SiteRow(128, y, 808, site, config_.extension); y += 82; }
    sites_scroll_->end(); sites_scroll_->redraw();
  }
  void save_settings() {
    appytizer::AppConfig updated = config_;
    updated.root_folder = std::filesystem::u8path(root_input_->value());
    updated.extension = extension_input_->value();
    if (updated.extension.empty() || updated.extension.front() != '.') updated.extension.insert(updated.extension.begin(), '.');
    updated.run_minimized = minimized_->value() != 0;
    updated.autostart = autostart_->value() != 0;
    const auto params = nlohmann::json{{"root_folder", updated.root_folder.string()}, {"extension", updated.extension},
        {"run_minimized", updated.run_minimized}, {"autostart", updated.autostart}}.dump();
    client_.request("config.set", params, [this, updated = std::move(updated)](std::string response) mutable {
      try {
        const auto document = nlohmann::json::parse(response);
        if (!document.value("ok", false)) throw std::runtime_error(document.value("error", "Settings could not be applied"));
        config_ = std::move(updated);
        set_ui_autostart(config_.autostart);
        extension_input_->value(config_.extension.c_str());
        refresh_all();
      } catch (const std::exception& error) {
        const std::string message = std::string("● Settings not applied: ") + error.what();
        engine_status_->copy_label(message.c_str());
        engine_status_->labelcolor(kDanger);
        engine_status_->redraw();
      }
    });
  }
  void browse() {
    IFileOpenDialog* dialog{}; if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(nullptr))) { IShellItem* item{}; if (SUCCEEDED(dialog->GetResult(&item))) { PWSTR path{}; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { root_input_->value(std::filesystem::path(path).string().c_str()); CoTaskMemFree(path); } item->Release(); } }
    dialog->Release();
  }
};

ServiceCard::ServiceCard(int x, int y, int w, int h, std::string id, std::string title, App* app)
    : Fl_Group(x, y, w, h), id_(std::move(id)), app_(app) {
  begin(); auto* panel = new Panel(x, y, w, h); panel->color(kPanel);
  make_label(x + 18, y + 16, w - 36, 26, title, kText, 16, FL_BOLD);
  status_ = make_label(x + 18, y + 50, w - 36, 22, "● Waiting for Engine", kMuted, 12);
  details_ = make_label(x + 18, y + 78, w - 36, 36, "Detection pending", kMuted, 11);
  action_ = make_button(x + 18, y + h - 46, w - 36, 30, "Start"); action_->callback(action_callback, this); end();
}
void ServiceCard::update(const nlohmann::json& service) {
  running_ = service.value("running", false); const auto& installations = service.value("installations", nlohmann::json::array());
  status_->copy_label(running_ ? "● Running" : "● Stopped"); status_->labelcolor(running_ ? kSuccess : kDanger);
  std::string details;
  if (id_ == "dns") details = service.value("version", ".local") + "  ·  loopback port 53";
  else if (installations.empty()) details = "Not detected — see Services";
  else details = std::to_string(installations.size()) + " installation" + (installations.size() == 1 ? "" : "s") + "  ·  " + service.value("version", "");
  details_->copy_label(details.c_str()); action_->copy_label(running_ ? "Stop" : "Start"); action_->activate();
  status_->redraw(); details_->redraw(); action_->redraw();
}
void ServiceCard::set_disconnected() {
  running_ = false; status_->copy_label("● Engine offline"); status_->labelcolor(kDanger);
  details_->copy_label("Status unavailable"); action_->deactivate();
  status_->redraw(); details_->redraw(); action_->redraw();
}
void ServiceCard::action_callback(Fl_Widget*, void* data) { auto* card = static_cast<ServiceCard*>(data); card->app_->service_action(card->id_, card->running_); }

ServiceRow::ServiceRow(int x, int y, int w, const nlohmann::json& service, App* app)
    : Fl_Group(x, y, w, 96), id_(service.value("id", "")), app_(app), running_(service.value("running", false)) {
  begin(); auto* panel = new Panel(x, y, w, 96); panel->color(kPanel);
  make_label(x + 16, y + 10, 150, 24, service.value("name", id_), kText, 14, FL_BOLD);
  status_ = make_label(x + 16, y + 38, 150, 20, running_ ? "● Running" : "● Stopped", running_ ? kSuccess : kDanger, 11);
  const auto installations = service.value("installations", nlohmann::json::array()); std::string location = "No executable or Windows service detected";
  if (!installations.empty()) { const auto& first = installations.front(); location = first.value("windows_service", false) ? "Windows service: " + first.value("service_name", "") : first.value("path", ""); }
  make_label(x + 180, y + 12, 390, 42, location, installations.empty() ? kDanger : kMuted, 11);
  versions_ = new Fl_Choice(x + 580, y + 12, 110, 30); versions_->color(kPanelHover); versions_->textcolor(kText);
  for (const auto& version : service.value("available_versions", nlohmann::json::array())) versions_->add(version.get<std::string>().c_str());
  const std::string active_version = service.value("version", "");
  int selected_version = 0;
  for (int index = 0; index < versions_->size(); ++index) {
    const char* candidate = versions_->text(index);
    if (candidate != nullptr && active_version == candidate) { selected_version = index; break; }
  }
  if (versions_->size()) versions_->value(selected_version); versions_->callback(version_callback, this);
  action_ = make_button(x + 700, y + 12, 90, 30, running_ ? "Stop" : "Start"); action_->callback(action_callback, this); end();
}
void ServiceRow::update(const nlohmann::json& service) {
  running_ = service.value("running", false);
  status_->copy_label(running_ ? "● Running" : "● Stopped");
  status_->labelcolor(running_ ? kSuccess : kDanger);
  action_->copy_label(running_ ? "Stop" : "Start"); action_->activate(); versions_->activate();
  const std::string active_version = service.value("version", "");
  for (int index = 0; index < versions_->size(); ++index) {
    const char* candidate = versions_->text(index);
    if (candidate != nullptr && active_version == candidate) { versions_->value(index); break; }
  }
  status_->redraw(); action_->redraw(); versions_->redraw();
}
void ServiceRow::set_disconnected() {
  running_ = false; status_->copy_label("● Engine offline"); status_->labelcolor(kDanger);
  action_->deactivate(); versions_->deactivate(); status_->redraw(); action_->redraw(); versions_->redraw();
}
void ServiceRow::action_callback(Fl_Widget*, void* data) { auto* row = static_cast<ServiceRow*>(data); const char* selected = row->versions_->text(); row->app_->service_action(row->id_, row->running_, selected ? selected : ""); }
void ServiceRow::version_callback(Fl_Widget*, void* data) { auto* row = static_cast<ServiceRow*>(data); if (const char* selected = row->versions_->text()) row->app_->select_version(row->id_, selected); }

SiteRow::SiteRow(int x, int y, int w, const nlohmann::json& site, std::string extension)
    : Fl_Group(x, y, w, 66), url_("http://" + site.value("name", "") + extension) {
  begin(); auto* panel = new Panel(x, y, w, 66); panel->color(kPanel);
  make_label(x + 16, y + 8, 190, 22, site.value("name", ""), kText, 13, FL_BOLD);
  make_label(x + 16, y + 32, 190, 18, site.value("type", ""), kMuted, 11);
  make_label(x + 220, y + 20, 350, 22, url_, kAccent, 11);
  auto* copy = make_button(x + 590, y + 16, 94, 32, "Copy URL"); copy->callback(copy_callback, this);
  auto* open = make_button(x + 696, y + 16, 94, 32, "Open"); open->callback(open_callback, this); end();
}
void SiteRow::copy_callback(Fl_Widget*, void* data) {
  const auto& url = static_cast<SiteRow*>(data)->url_; if (!OpenClipboard(nullptr)) return; EmptyClipboard();
  const int count = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0); HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(count) * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, static_cast<wchar_t*>(GlobalLock(memory)), count); GlobalUnlock(memory); SetClipboardData(CF_UNICODETEXT, memory); CloseClipboard();
}
void SiteRow::open_callback(Fl_Widget*, void* data) { ShellExecuteA(nullptr, "open", static_cast<SiteRow*>(data)->url_.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

LRESULT CALLBACK tray_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kTrayMessage) {
    if (lparam == WM_LBUTTONUP) { if (g_app) g_app->show(); return 0; }
    if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
      HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, kTrayDisplay, L"Display UI"); AppendMenuW(menu, MF_STRING, kTrayStopExit, L"Stop all and exit"); AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");
      POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd); const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr); DestroyMenu(menu);
      if (g_app) { if (command == kTrayDisplay) g_app->show(); else if (command == kTrayStopExit) g_app->stop_and_exit(); else if (command == kTrayExit) g_app->quit(); } return 0;
    }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); appytizer::ConfigStore store; App app;
  const bool force_show = argc > 1 && std::string_view(argv[1]) == "--show";
  if (force_show || !store.load().run_minimized) app.show(); while (app.running() && Fl::wait()) {}
  CoUninitialize(); return 0;
}
