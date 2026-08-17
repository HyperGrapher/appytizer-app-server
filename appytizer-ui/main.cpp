#include "appytizer-ui/ipc/pipe_client.hpp"
#include "common/constants.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr UINT kTrayMessage = WM_APP + 42, kTrayDisplay = 1, kTrayStopExit = 2, kTrayExit = 3;
constexpr int kButtonRadius = 4;
const Fl_Color kBackground = fl_rgb_color(0x20, 0x20, 0x20);
const Fl_Color kSurface = fl_rgb_color(0x2B, 0x2B, 0x2B);
const Fl_Color kControl = fl_rgb_color(0x32, 0x32, 0x32);
const Fl_Color kControlPressed = fl_rgb_color(0x3C, 0x3C, 0x3C);
const Fl_Color kBorder = fl_rgb_color(0x45, 0x45, 0x45);
const Fl_Color kText = fl_rgb_color(0xF2, 0xF2, 0xF2);
const Fl_Color kMuted = fl_rgb_color(0xB5, 0xB5, 0xB5);
const Fl_Color kLink = fl_rgb_color(0x6C, 0xB8, 0xF6);
const Fl_Color kReady = fl_rgb_color(0x6C, 0xCB, 0x5F);
const Fl_Color kAttention = fl_rgb_color(0xF0, 0xB3, 0x5A);
const Fl_Color kFault = fl_rgb_color(0xFF, 0x71, 0x65);

class App;
App* g_app{};

std::filesystem::path executable_directory() {
  std::array<wchar_t, 32768> executable{};
  const DWORD length =
      GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length == executable.size()) {
    return {};
  }
  return std::filesystem::path(executable.data()).parent_path();
}

class AssetCatalog final {
public:
  AssetCatalog() {
    const auto directory = executable_directory() / L"assets";
    window_icon_ = load_png(directory / L"app_icon.png");
    if (window_icon_ != nullptr) {
      brand_icon_.reset(window_icon_->copy(36, 36));
      app_icon_.reset(window_icon_->copy(24, 24));
    }
    html_icon_ = load_scaled(directory / L"html.png", 24);
    nginx_icon_ = load_scaled(directory / L"nginx.png", 24);
    php_icon_ = load_scaled(directory / L"php.png", 24);
    mysql_icon_ = load_scaled(directory / L"mysql.png", 24);
    postgres_icon_ = load_scaled(directory / L"postgres.png", 24);
  }

  [[nodiscard]] const Fl_RGB_Image* windowIcon() const { return window_icon_.get(); }
  [[nodiscard]] Fl_Image* brandIcon() const { return brand_icon_.get(); }

  [[nodiscard]] Fl_Image* serviceIcon(std::string_view id) const {
    if (id == "dns") {
      return app_icon_.get();
    }
    if (id == "nginx") {
      return nginx_icon_.get();
    }
    if (id == "php") {
      return php_icon_.get();
    }
    if (id == "mysql") {
      return mysql_icon_.get();
    }
    if (id == "postgres") {
      return postgres_icon_.get();
    }
    return nullptr;
  }

  [[nodiscard]] Fl_Image* siteIcon(std::string_view type) const {
    if (type == "php") {
      return php_icon_.get();
    }
    if (type == "html") {
      return html_icon_.get();
    }
    return nullptr;
  }

private:
  static std::unique_ptr<Fl_PNG_Image> load_png(const std::filesystem::path& path) {
    auto image = std::make_unique<Fl_PNG_Image>(path.string().c_str());
    if (image->fail() != 0) {
      return nullptr;
    }
    return image;
  }

  static std::unique_ptr<Fl_Image> load_scaled(const std::filesystem::path& path, int size) {
    const auto source = load_png(path);
    return source != nullptr ? std::unique_ptr<Fl_Image>(source->copy(size, size)) : nullptr;
  }

  std::unique_ptr<Fl_PNG_Image> window_icon_;
  std::unique_ptr<Fl_Image> brand_icon_;
  std::unique_ptr<Fl_Image> app_icon_;
  std::unique_ptr<Fl_Image> html_icon_;
  std::unique_ptr<Fl_Image> nginx_icon_;
  std::unique_ptr<Fl_Image> php_icon_;
  std::unique_ptr<Fl_Image> mysql_icon_;
  std::unique_ptr<Fl_Image> postgres_icon_;
};

Fl_Box* make_label(int x, int y, int w, int h, std::string_view value,
                   Fl_Color color = kText, int size = 13, Fl_Font font = FL_HELVETICA) {
  auto* label = new Fl_Box(x, y, w, h);
  label->copy_label(std::string(value).c_str()); // FLTK otherwise retains a dangling char pointer.
  label->box(FL_NO_BOX); label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  label->labelcolor(color); label->labelsize(size); label->labelfont(font); return label;
}

Fl_Box* make_image(int x, int y, int w, int h, Fl_Image* image, std::string_view tooltip = {}) {
  auto* box = new Fl_Box(x, y, w, h);
  box->box(FL_NO_BOX);
  box->image(image);
  box->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
  if (!tooltip.empty()) {
    box->copy_tooltip(std::string(tooltip).c_str());
  }
  return box;
}

class LinkLabel final : public Fl_Box {
public:
  LinkLabel(int x, int y, int w, int h, std::string url)
      : Fl_Box(x, y, w, h), url_(std::move(url)) {
    copy_label(url_.c_str());
    box(FL_NO_BOX);
    align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    labelcolor(kLink);
    labelfont(FL_COURIER);
    labelsize(12);
    copy_tooltip(url_.c_str());
  }

  int handle(int event) override {
    if (event == FL_ENTER) {
      is_hovered_ = true;
      window()->cursor(FL_CURSOR_HAND);
      redraw();
      return 1;
    }
    if (event == FL_LEAVE) {
      is_hovered_ = false;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    }
    if (event == FL_RELEASE && Fl::event_inside(this)) {
      ShellExecuteA(nullptr, "open", url_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return 1;
    }
    return Fl_Box::handle(event);
  }

  void draw() override {
    Fl_Box::draw();
    if (!is_hovered_) {
      return;
    }
    fl_font(labelfont(), labelsize());
    const int underline_width =
        std::min(w(), static_cast<int>(fl_width(label())));
    const int baseline = y() + ((h() - labelsize()) / 2) + labelsize();
    fl_color(labelcolor());
    fl_line(x(), baseline + 1, x() + underline_width, baseline + 1);
  }

private:
  std::string url_;
  bool is_hovered_{};
};

class SeparatorBox final : public Fl_Box {
public:
  using Fl_Box::Fl_Box;

  void draw() override {
    fl_color(color());
    fl_rectf(x(), y(), w(), h());
    fl_color(kBorder);
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1);
  }
};

class Button final : public Fl_Button {
public:
  using Fl_Button::Fl_Button;

  void draw() override {
    const Fl_Color fill = value() ? selection_color() : color();
    fl_color(fill);
    fl_rounded_rectf(x(), y(), w(), h(), kButtonRadius);
    fl_color(kBorder);
    fl_rounded_rect(x(), y(), w(), h(), kButtonRadius);
    fl_color(active() ? labelcolor() : kMuted);
    fl_font(labelfont(), labelsize());
    fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER | FL_ALIGN_CLIP);
    if (Fl::focus() == this) {
      draw_focus();
    }
  }
};

Button* make_button(int x, int y, int w, int h, std::string_view value,
                    Fl_Color fill = kControl) {
  auto* button = new Button(x, y, w, h);
  button->copy_label(std::string(value).c_str());
  button->color(fill);
  button->selection_color(kControlPressed);
  button->labelcolor(fill == kLink ? kSurface : kText);
  button->labelsize(12);
  return button;
}

class TabButton final : public Fl_Button {
public:
  using Fl_Button::Fl_Button;

  void selected(bool selected) {
    selected_ = selected;
    redraw();
  }

  void draw() override {
    fl_color(kBackground);
    fl_rectf(x(), y(), w(), h());
    if (selected_) {
      fl_color(kSurface);
      fl_rectf(x(), y(), w(), h());
      fl_color(kLink);
      fl_rectf(x(), y() + h() - 2, w(), 2);
    }
    fl_color(selected_ ? kText : kMuted);
    fl_font(selected_ ? FL_BOLD : FL_HELVETICA, 13);
    fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER | FL_ALIGN_CLIP);
  }

private:
  bool selected_{};
};

class ServiceRow final : public Fl_Group {
public:
  ServiceRow(int x, int y, int w, const nlohmann::json& service, App* app,
             Fl_Image* icon);
  void update(const nlohmann::json& service);
  void set_disconnected();
  [[nodiscard]] const std::string& id() const { return id_; }
private:
  static void action_callback(Fl_Widget*, void* data);
  static void version_callback(Fl_Widget*, void* data);
  std::string id_;
  App* app_{};
  bool running_{};
  bool is_available_{};
  Fl_Choice* versions_{};
  Fl_Box* version_label_{};
  Fl_Box* status_{};
  Fl_Box* installation_{};
  Button* action_{};
  void update_installation(const nlohmann::json& installations,
                           std::string_view active_version);
};

class SiteRow final : public Fl_Group {
public:
  SiteRow(int x, int y, int w, const nlohmann::json& site, bool https_enabled,
          Fl_Image* type_icon);
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
class App {
public:
  explicit App(bool force_show) : force_show_(force_show) { g_app = this; build_ui(); tray_.create(); client_.subscribe([this](std::string event) { handle_event(std::move(event)); }); refresh_all(); }
  ~App() { g_app = nullptr; }
  void show() {
    window_->show();
    const BOOL use_dark_mode = TRUE;
    const HWND handle = fl_xid(window_);
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
    if (FAILED(DwmSetWindowAttribute(handle, kImmersiveDarkMode, &use_dark_mode,
                                     sizeof(use_dark_mode)))) {
      DwmSetWindowAttribute(handle, kImmersiveDarkModeBefore20H1, &use_dark_mode,
                            sizeof(use_dark_mode));
    }
    constexpr DWORD kBorderColor = 34;
    constexpr DWORD kCaptionColor = 35;
    constexpr DWORD kTextColor = 36;
    const COLORREF border_color = RGB(0x45, 0x45, 0x45);
    const COLORREF caption_color = RGB(0x20, 0x20, 0x20);
    const COLORREF text_color = RGB(0xF2, 0xF2, 0xF2);
    DwmSetWindowAttribute(handle, kBorderColor, &border_color, sizeof(border_color));
    DwmSetWindowAttribute(handle, kCaptionColor, &caption_color, sizeof(caption_color));
    DwmSetWindowAttribute(handle, kTextColor, &text_color, sizeof(text_color));
    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    window_->take_focus();
  }
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
  struct UiConfig { std::filesystem::path root_folder; bool https_enabled{true}; bool run_minimized{}; bool autostart{}; };
  UiConfig config_;
  AssetCatalog assets_;
  appytizer::PipeClient client_;
  TrayIcon tray_;
  Fl_Double_Window* window_{};
  std::array<Fl_Group*, 3> views_{};
  std::array<TabButton*, 3> nav_{};
  std::array<std::pair<App*, int>, 3> nav_callbacks_{};
  std::vector<ServiceRow*> service_rows_;
  Fl_Box *engine_status_{}, *dns_status_{}, *nginx_status_{}, *tls_summary_{};
  Fl_Box *root_summary_{}, *sites_empty_{};
  Fl_Scroll *services_scroll_{}, *sites_scroll_{};
  Button* services_rescan_{};
  Fl_Input* root_input_{};
  Fl_Check_Button *https_{}, *minimized_{}, *autostart_{};
  Fl_Box *tls_status_{}, *settings_result_{};
  bool quitting_{}, force_show_{}, initial_visibility_applied_{}; int active_view_{};

  static void nav_callback(Fl_Widget*, void* data) { auto* pair = static_cast<std::pair<App*, int>*>(data); pair->first->show_view(pair->second); }
  static void rescan_callback(Fl_Widget*, void* data) {
    auto* app = static_cast<App*>(data);
    app->services_rescan_->copy_label("Scanning…");
    app->services_rescan_->deactivate();
    app->services_rescan_->redraw();
    app->client_.request("service.rescan", "{}", [app](std::string) {
      app->services_rescan_->copy_label("Rescan installations");
      app->services_rescan_->activate();
      app->services_rescan_->redraw();
      app->refresh_services(true);
    });
  }
  static void save_callback(Fl_Widget*, void* data) { static_cast<App*>(data)->save_settings(); }
  static void browse_callback(Fl_Widget*, void* data) { static_cast<App*>(data)->browse(); }
  static void repair_tls_callback(Fl_Widget*, void* data) { static_cast<App*>(data)->repair_tls(); }

  void build_ui() {
    Fl::background(0x20, 0x20, 0x20);
    Fl::background2(0x2B, 0x2B, 0x2B);
    Fl::foreground(0xF2, 0xF2, 0xF2);
    Fl::set_font(FL_HELVETICA, "Segoe UI");
    Fl::set_font(FL_BOLD, "Segoe UI Semibold");
    Fl::set_font(FL_COURIER, "Consolas");
    window_ = new Fl_Double_Window(1000, 640, "Appytizer");
    window_->color(kBackground);
    if (assets_.windowIcon() != nullptr) {
      window_->icon(assets_.windowIcon());
    }
    window_->size_range(1000, 640, 1000, 640);
    window_->callback([](Fl_Widget* widget, void*) { widget->hide(); });
    window_->begin();

    auto* header = new SeparatorBox(0, 0, 1000, 58);
    header->color(kBackground);
    make_image(20, 11, 36, 36, assets_.brandIcon(), "Appytizer");
    constexpr std::array<const char*, 3> names{"Sites", "Services", "Settings"};
    constexpr std::array<int, 3> widths{88, 104, 100};
    int tab_x = 76;
    for (int i = 0; i < 3; ++i) {
      nav_[i] = new TabButton(tab_x, 0, widths[i], 58, names[i]);
      nav_[i]->selected(i == 0);
      nav_callbacks_[i] = {this, i};
      nav_[i]->callback(nav_callback, &nav_callbacks_[i]);
      tab_x += widths[i];
    }

    build_sites();
    build_services();
    build_settings();
    for (int i = 1; i < 3; ++i) {
      views_[i]->hide();
    }

    auto* status_bar = new SeparatorBox(0, 608, 1000, 32);
    status_bar->color(kSurface);
    auto* status_separator = new Fl_Box(0, 608, 1000, 1);
    status_separator->box(FL_FLAT_BOX);
    status_separator->color(kBorder);
    engine_status_ = make_label(20, 609, 210, 30, "● Connecting to Engine", kMuted, 11);
    dns_status_ = make_label(238, 609, 160, 30, "○ Routing unavailable", kMuted, 11);
    nginx_status_ = make_label(406, 609, 180, 30, "○ nginx unavailable", kMuted, 11);
    tls_summary_ = make_label(594, 609, 386, 30, "○ Checking HTTPS trust", kMuted, 11);
    window_->end();
  }

  void begin_view(int index) {
    views_[index] = new Fl_Group(0, 58, 1000, 550);
    views_[index]->begin();
  }

  void build_sites() {
    begin_view(0);
    make_label(20, 72, 90, 30, "Sites", kText, 18, FL_BOLD);
    root_summary_ = make_label(112, 74, 868, 28, "No projects folder configured", kMuted, 12,
                               FL_COURIER);

    auto* table_header = new SeparatorBox(20, 116, 960, 30);
    table_header->color(kControl);
    make_label(32, 116, 94, 29, "STATE", kMuted, 11, FL_BOLD);
    make_label(132, 116, 520, 29, "FOLDER  →  LOCAL ADDRESS", kMuted, 11, FL_BOLD);
    make_label(702, 116, 80, 29, "TYPE", kMuted, 11, FL_BOLD);
    make_label(806, 116, 150, 29, "ACTIONS", kMuted, 11, FL_BOLD);

    sites_scroll_ = new Fl_Scroll(20, 146, 960, 450);
    sites_scroll_->box(FL_NO_BOX);
    sites_scroll_->begin();
    sites_empty_ = make_label(36, 166, 900, 44,
                              "No sites found. Choose a projects folder in Settings.",
                              kMuted, 13);
    sites_scroll_->end();
    views_[0]->end();
  }

  void build_services() {
    begin_view(1);
    make_label(20, 72, 120, 30, "Services", kText, 18, FL_BOLD);
    make_label(136, 74, 646, 28, "Core routing, runtimes, and databases detected on this PC", kMuted, 12);
    services_rescan_ = make_button(820, 72, 160, 32, "Rescan installations");
    services_rescan_->callback(rescan_callback, this);

    auto* table_header = new SeparatorBox(20, 116, 960, 30);
    table_header->color(kControl);
    make_label(32, 116, 100, 29, "STATE", kMuted, 11, FL_BOLD);
    make_label(174, 116, 150, 29, "SERVICE", kMuted, 11, FL_BOLD);
    make_label(334, 116, 112, 29, "VERSION", kMuted, 11, FL_BOLD);
    make_label(454, 116, 370, 29, "DETECTED INSTALLATION", kMuted, 11, FL_BOLD);
    make_label(846, 116, 100, 29, "ACTION", kMuted, 11, FL_BOLD);

    services_scroll_ = new Fl_Scroll(20, 146, 960, 450);
    services_scroll_->box(FL_NO_BOX);
    services_scroll_->begin();
    make_label(36, 166, 900, 44, "Waiting for service status from the Engine…", kMuted, 13);
    services_scroll_->end();
    views_[1]->end();
  }

  void build_settings() {
    begin_view(2);
    make_label(20, 72, 120, 30, "Settings", kText, 18, FL_BOLD);

    make_label(20, 122, 220, 24, "LOCAL SITES", kMuted, 11, FL_BOLD);
    auto* local_sites_separator = new SeparatorBox(20, 148, 960, 1);
    local_sites_separator->color(kBorder);
    make_label(20, 164, 180, 28, "Projects folder", kText, 13);
    root_input_ = new Fl_Input(200, 160, 580, 36);
    root_input_->box(FL_BORDER_BOX);
    root_input_->color(kSurface);
    root_input_->textcolor(kText);
    root_input_->textfont(FL_COURIER);
    root_input_->textsize(12);
    root_input_->value(config_.root_folder.string().c_str());
    auto* browse_button = make_button(796, 160, 184, 36, "Choose folder");
    browse_button->callback(browse_callback, this);
    make_label(200, 198, 700, 24, "Each direct child becomes https://<folder>.test.", kMuted, 11);

    make_label(20, 244, 220, 24, "HTTPS AND TRUST", kMuted, 11, FL_BOLD);
    auto* https_separator = new SeparatorBox(20, 270, 960, 1);
    https_separator->color(kBorder);
    https_ = new Fl_Check_Button(20, 284, 480, 28, "  Use trusted HTTPS for local sites");
    https_->color(kBackground);
    https_->selection_color(kLink);
    https_->labelcolor(kText);
    https_->labelsize(13);
    https_->value(1);
    tls_status_ = make_label(44, 314, 730, 42, "Checking certificate health…", kMuted, 11);
    auto* repair = make_button(796, 306, 184, 36, "Repair certificates");
    repair->callback(repair_tls_callback, this);

    make_label(20, 374, 220, 24, "STARTUP", kMuted, 11, FL_BOLD);
    auto* startup_separator = new SeparatorBox(20, 400, 960, 1);
    startup_separator->color(kBorder);
    autostart_ = new Fl_Check_Button(20, 414, 420, 28, "  Run Appytizer UI when I sign in");
    autostart_->color(kBackground);
    autostart_->selection_color(kLink);
    autostart_->labelcolor(kText);
    autostart_->labelsize(13);
    minimized_ = new Fl_Check_Button(20, 450, 420, 28,
                                     "  Start minimized to the notification area");
    minimized_->color(kBackground);
    minimized_->selection_color(kLink);
    minimized_->labelcolor(kText);
    minimized_->labelsize(13);

    settings_result_ = make_label(20, 526, 750, 38, "", kMuted, 12);
    auto* save = make_button(796, 526, 184, 38, "Save and apply", kLink);
    save->callback(save_callback, this);
    views_[2]->end();
  }
  void show_view(int index) {
    active_view_ = index;
    for (int i = 0; i < 3; ++i) {
      if (i == index) {
        views_[i]->show();
      } else {
        views_[i]->hide();
      }
      nav_[i]->selected(i == index);
    }
    if (index == 0) {
      refresh_sites();
    } else if (index == 1) {
      refresh_services(true);
    } else if (index == 2) {
      refresh_config();
      refresh_tls();
    }
    window_->redraw();
  }
  void refresh_all() {
    refresh_services(true);
    refresh_config();
    refresh_tls();
  }
  void apply_service_status(const nlohmann::json& services) {
    engine_status_->copy_label("● Engine connected");
    for (const auto& service : services) {
      const std::string id = service.value("id", "");
      const bool running = service.value("running", false);
      if (id == "dns") {
        dns_status_->copy_label(running ? "● Routing running" : "● Routing stopped");
        dns_status_->labelcolor(running ? kReady : kFault);
        dns_status_->redraw();
      } else if (id == "nginx") {
        nginx_status_->copy_label(running ? "● nginx running" : "● nginx stopped");
        nginx_status_->labelcolor(running ? kReady : kFault);
        nginx_status_->redraw();
      }
      for (auto* row : service_rows_) {
        if (row->id() == id) {
          row->update(service);
        }
      }
    }
    engine_status_->labelcolor(kReady);
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
        engine_status_->copy_label("● Engine disconnected");
        engine_status_->labelcolor(kFault);
        dns_status_->copy_label("● Routing unavailable");
        dns_status_->labelcolor(kFault);
        nginx_status_->copy_label("● nginx unavailable");
        nginx_status_->labelcolor(kFault);
        tls_summary_->copy_label("○ HTTPS status unavailable");
        tls_summary_->labelcolor(kMuted);
        engine_status_->redraw();
        dns_status_->redraw();
        nginx_status_->redraw();
        tls_summary_->redraw();
        for (auto* row : service_rows_) {
          row->set_disconnected();
        }
      } else if (type == "status.update") apply_service_status(document.at("services"));
      else if (type == "sites.changed" && active_view_ == 0) populate_sites(document.at("sites"));
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
      } catch (const std::exception&) {
        engine_status_->copy_label("● Engine disconnected");
        engine_status_->labelcolor(kFault);
        engine_status_->redraw();
        for (auto* row : service_rows_) {
          row->set_disconnected();
        }
      }
    });
  }
  void populate_services(const nlohmann::json& services) {
    services_scroll_->clear();
    service_rows_.clear();
    services_scroll_->begin();
    int y = 150;

    const auto find_service = [&services](std::string_view id) -> const nlohmann::json* {
      for (const auto& service : services) {
        if (service.value("id", "") == id) {
          return &service;
        }
      }
      return nullptr;
    };
    const auto add_group = [this, &find_service, &y](
                               std::string_view title,
                               std::initializer_list<std::string_view> ids) {
      make_label(32, y, 900, 26, title, kMuted, 11, FL_BOLD);
      y += 26;
      for (const auto id : ids) {
        if (const auto* service = find_service(id)) {
          service_rows_.push_back(
              new ServiceRow(20, y, 942, *service, this, assets_.serviceIcon(id)));
          y += 46;
        }
      }
      y += 8;
    };

    add_group("CORE ROUTING", {"dns", "nginx"});
    add_group("RUNTIME", {"php"});
    add_group("DATABASES", {"mysql", "postgres"});
    services_scroll_->end();
    services_scroll_->redraw();
  }
  void refresh_config() {
    client_.request("config.get", "{}", [this](std::string response) {
      try {
        const auto document = nlohmann::json::parse(response);
        if (!document.value("ok", false)) throw std::runtime_error(document.value("error", "Engine unavailable"));
        const auto& result = document.at("result");
        config_.root_folder = std::filesystem::path(result.value("root_folder", ""));
        config_.https_enabled = result.value("https_enabled", true);
        config_.run_minimized = result.value("run_minimized", false);
        config_.autostart = result.value("autostart", false);
        root_input_->value(config_.root_folder.string().c_str());
        const std::string root_summary = config_.root_folder.empty()
                                             ? "No projects folder configured"
                                             : config_.root_folder.string();
        root_summary_->copy_label(root_summary.c_str());
        root_summary_->redraw();
        https_->value(config_.https_enabled ? 1 : 0);
        minimized_->value(config_.run_minimized ? 1 : 0);
        autostart_->value(config_.autostart ? 1 : 0);
        refresh_tls();
        refresh_sites();
        if (!initial_visibility_applied_) {
          initial_visibility_applied_ = true;
          if (force_show_ || !config_.run_minimized) show();
        }
      } catch (...) {
        if (!initial_visibility_applied_) { initial_visibility_applied_ = true; show(); }
      }
    });
  }
  void refresh_tls() {
    if (!config_.https_enabled) {
      tls_status_->copy_label("○ HTTPS is disabled. Existing certificates are retained.");
      tls_status_->labelcolor(kMuted);
      tls_summary_->copy_label("○ HTTPS disabled");
      tls_summary_->labelcolor(kMuted);
      tls_status_->redraw();
      tls_summary_->redraw();
      return;
    }
    client_.request("tls.status", "{}", [this](std::string response) {
      try {
        const auto document = nlohmann::json::parse(response);
        if (!document.value("ok", false)) throw std::runtime_error(document.value("error", "Engine unavailable"));
        const auto& status = document.at("result");
        const bool ready = status.value("ready", false);
        std::string text;
        if (ready) {
          text = "● Trusted · " + std::to_string(status.value("site_certificate_count", 0)) + " site certificate(s)";
          const auto expiry = status.value("earliest_expiry", "");
          if (!expiry.empty()) {
            text += " · earliest expiry " + expiry;
          }
        } else {
          text = "● " + status.value("error", "HTTPS trust needs attention");
        }
        tls_status_->copy_label(text.c_str());
        tls_status_->labelcolor(ready ? kReady : kFault);
        const std::string summary = ready
                                        ? "● HTTPS trusted · " +
                                              std::to_string(status.value("site_certificate_count", 0)) +
                                              " certificates"
                                        : "● HTTPS trust needs attention";
        tls_summary_->copy_label(summary.c_str());
        tls_summary_->labelcolor(ready ? kReady : kFault);
        tls_status_->redraw();
        tls_summary_->redraw();
      } catch (const std::exception& error) {
        const std::string text = std::string("● Certificate status unavailable: ") + error.what();
        tls_status_->copy_label(text.c_str());
        tls_status_->labelcolor(kFault);
        tls_summary_->copy_label("● HTTPS status unavailable");
        tls_summary_->labelcolor(kFault);
        tls_status_->redraw();
        tls_summary_->redraw();
      }
    });
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
    sites_scroll_->clear();
    sites_scroll_->begin();
    int y = 150;
    if (sites.empty()) {
      sites_empty_ = make_label(
          36, 166, 900, 44,
          "No sites found. Choose a projects folder in Settings.", kMuted, 13);
    } else {
      const auto add_rows = [this, &sites, &y](bool valid) {
        for (const auto& site : sites) {
          if (site.value("valid", false) != valid) {
            continue;
          }
          new SiteRow(20, y, 942, site, config_.https_enabled,
                      assets_.siteIcon(site.value("type", "")));
          y += valid ? 44 : 56;
        }
      };
      add_rows(false);
      add_rows(true);
    }
    sites_scroll_->end();
    sites_scroll_->redraw();
  }
  void save_settings() {
    UiConfig updated = config_;
    updated.root_folder = std::filesystem::path(root_input_->value());
    updated.https_enabled = https_->value() != 0;
    updated.run_minimized = minimized_->value() != 0;
    updated.autostart = autostart_->value() != 0;
    const auto params = nlohmann::json{{"root_folder", updated.root_folder.string()}, {"https_enabled", updated.https_enabled},
        {"run_minimized", updated.run_minimized}, {"autostart", updated.autostart}}.dump();
    settings_result_->copy_label("● Applying settings…");
    settings_result_->labelcolor(kLink);
    settings_result_->redraw();
    client_.request("config.set", params, [this, updated = std::move(updated)](std::string response) mutable {
      try {
        const auto document = nlohmann::json::parse(response);
        if (!document.value("ok", false)) throw std::runtime_error(document.value("error", "Settings could not be applied"));
        config_ = std::move(updated);
        set_ui_autostart(config_.autostart);
        settings_result_->copy_label("● Settings applied");
        settings_result_->labelcolor(kReady);
        settings_result_->redraw();
        refresh_all();
      } catch (const std::exception& error) {
        const std::string message = std::string("● Settings not applied: ") + error.what();
        settings_result_->copy_label(message.c_str());
        settings_result_->labelcolor(kFault);
        settings_result_->redraw();
      }
    });
  }
  void repair_tls() {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return;
    const auto engine = std::filesystem::path(executable).parent_path() / L"AppytizerEngine.exe";
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas";
    launch.lpFile = engine.c_str();
    launch.lpParameters = L"--provision-tls";
    launch.nShow = SW_HIDE;
    tls_status_->copy_label("● Repairing certificate trust…");
    tls_status_->labelcolor(kLink);
    tls_status_->redraw();
    if (!ShellExecuteExW(&launch) || !launch.hProcess) {
      tls_status_->copy_label("● Certificate repair was cancelled or could not start.");
      tls_status_->labelcolor(kFault); tls_status_->redraw(); return;
    }
    WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD exit_code{}; GetExitCodeProcess(launch.hProcess, &exit_code); CloseHandle(launch.hProcess);
    if (exit_code != 0) {
      tls_status_->copy_label("● Certificate repair failed. See the Engine log for details.");
      tls_status_->labelcolor(kFault); tls_status_->redraw(); return;
    }
    client_.request("sites.rescan", "{}", [this](std::string) { refresh_tls(); refresh_sites(); });
  }
  void browse() {
    IFileOpenDialog* dialog{}; if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(nullptr))) { IShellItem* item{}; if (SUCCEEDED(dialog->GetResult(&item))) { PWSTR path{}; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { root_input_->value(std::filesystem::path(path).string().c_str()); CoTaskMemFree(path); } item->Release(); } }
    dialog->Release();
  }
};

ServiceRow::ServiceRow(int x, int y, int w, const nlohmann::json& service, App* app,
                       Fl_Image* icon)
    : Fl_Group(x, y, w, 46),
      id_(service.value("id", "")),
      app_(app),
      running_(service.value("running", false)) {
  const auto installations = service.value("installations", nlohmann::json::array());
  is_available_ = id_ == "dns" || !installations.empty();
  const std::string active_version = service.value("version", "");

  begin();
  auto* background = new SeparatorBox(x, y, w, 46);
  background->color(kSurface);
  const std::string initial_status =
      running_ ? "● Running" : is_available_ ? "● Stopped" : "● Not detected";
  status_ = make_label(x + 12, y, 100, 45, initial_status,
                       running_ ? kReady : kFault, 11);
  make_image(x + 122, y + 11, 24, 24, icon, service.value("name", id_));
  make_label(x + 154, y, 150, 45, service.value("name", id_), kText, 13, FL_BOLD);

  if (id_ == "dns" || id_ == "nginx") {
    version_label_ = make_label(x + 314, y, 108, 45, active_version,
                                 kMuted, 12, FL_COURIER);
  } else {
    versions_ = new Fl_Choice(x + 314, y + 8, 108, 30);
    versions_->box(FL_BORDER_BOX);
    versions_->color(kControl);
    versions_->textcolor(kText);
    versions_->textfont(FL_COURIER);
    versions_->textsize(12);
    for (const auto& version : service.value("available_versions", nlohmann::json::array())) {
      versions_->add(version.get<std::string>().c_str());
    }
    int selected_version = 0;
    for (int index = 0; index < versions_->size(); ++index) {
      const char* candidate = versions_->text(index);
      if (candidate != nullptr && active_version == candidate) {
        selected_version = index;
        break;
      }
    }
    if (versions_->size() > 0) {
      versions_->value(selected_version);
    }
    versions_->callback(version_callback, this);
  }

  installation_ = make_label(x + 434, y, 370, 45, "", kMuted, 11, FL_COURIER);
  update_installation(installations, active_version);
  action_ = make_button(x + 826, y + 8, 88, 30, running_ ? "Stop" : "Start");
  action_->callback(action_callback, this);
  if (!is_available_) {
    action_->deactivate();
    if (versions_ != nullptr) {
      versions_->deactivate();
    }
  }
  end();
}

void ServiceRow::update_installation(const nlohmann::json& installations,
                                     std::string_view active_version) {
  if (id_ == "dns") {
    installation_->copy_label("Built in · loopback port 53");
    installation_->redraw();
    return;
  }

  const nlohmann::json* selected{};
  for (const auto& installation : installations) {
    if (installation.value("version", "") == active_version) {
      selected = &installation;
      break;
    }
  }
  if (selected == nullptr && !installations.empty()) {
    selected = &installations.front();
  }

  std::string location = "No installation detected";
  if (selected != nullptr) {
    location = selected->value("windows_service", false)
                   ? "Windows service: " + selected->value("service_name", "")
                   : selected->value("path", "");
  }
  installation_->copy_label(location.c_str());
  installation_->copy_tooltip(location.c_str());
  installation_->redraw();
}

void ServiceRow::update(const nlohmann::json& service) {
  running_ = service.value("running", false);
  const auto installations = service.value("installations", nlohmann::json::array());
  is_available_ = id_ == "dns" || !installations.empty();
  status_->copy_label(
      running_ ? "● Running" : is_available_ ? "● Stopped" : "● Not detected");
  status_->labelcolor(running_ ? kReady : kFault);
  action_->copy_label(running_ ? "Stop" : "Start");
  if (is_available_) {
    action_->activate();
  } else {
    action_->deactivate();
  }
  const std::string active_version = service.value("version", "");
  if (versions_ != nullptr) {
    if (is_available_) {
      versions_->activate();
    } else {
      versions_->deactivate();
    }
    for (int index = 0; index < versions_->size(); ++index) {
      const char* candidate = versions_->text(index);
      if (candidate != nullptr && active_version == candidate) {
        versions_->value(index);
        break;
      }
    }
    versions_->redraw();
  } else if (version_label_ != nullptr) {
    version_label_->copy_label(active_version.c_str());
    version_label_->redraw();
  }
  update_installation(installations, active_version);
  status_->redraw();
  action_->redraw();
}
void ServiceRow::set_disconnected() {
  running_ = false;
  status_->copy_label("● Engine offline");
  status_->labelcolor(kFault);
  action_->deactivate();
  if (versions_ != nullptr) {
    versions_->deactivate();
    versions_->redraw();
  }
  status_->redraw();
  action_->redraw();
}
void ServiceRow::action_callback(Fl_Widget*, void* data) {
  auto* row = static_cast<ServiceRow*>(data);
  const char* selected = row->versions_ != nullptr ? row->versions_->text() : nullptr;
  row->app_->service_action(row->id_, row->running_, selected ? selected : "");
}
void ServiceRow::version_callback(Fl_Widget*, void* data) {
  auto* row = static_cast<ServiceRow*>(data);
  if (row->versions_ != nullptr) {
    if (const char* selected = row->versions_->text()) {
      row->app_->select_version(row->id_, selected);
    }
  }
}

SiteRow::SiteRow(int x, int y, int w, const nlohmann::json& site, bool https_enabled,
                 Fl_Image* type_icon)
    : Fl_Group(x, y, w, site.value("valid", false) ? 44 : 56),
      url_((https_enabled ? "https://" : "http://") + site.value("hostname", "")) {
  const bool valid = site.value("valid", false);
  const bool has_index = site.value("has_index", false);
  const int row_height = valid ? 44 : 56;
  begin();
  auto* background = new SeparatorBox(x, y, w, row_height);
  background->color(kSurface);
  auto* state_icon = make_label(x + 12, y, 94, row_height,
                                !valid ? "✕" : has_index ? "✓" : "⚠",
                                !valid ? kFault : has_index ? kReady : kAttention, 18,
                                FL_BOLD);
  state_icon->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
  if (!valid) {
    state_icon->copy_tooltip(site.value("error", "Invalid site name").c_str());
  } else if (!has_index) {
    state_icon->copy_tooltip(
        "No index: this folder does not contain index.php or index.html.");
  } else {
    state_icon->copy_tooltip("Site is ready.");
  }
  make_label(x + 112, y, 160, row_height, site.value("name", ""), kText, 13, FL_BOLD);
  make_label(x + 276, y, 26, row_height, valid ? "→" : "↛",
              valid ? kMuted : kFault, 13);
  if (valid) {
    new LinkLabel(x + 310, y, 360, row_height, url_);
    if (type_icon != nullptr) {
      make_image(x + 710, y + 10, 24, 24, type_icon,
                 site.value("type", "") == "php" ? "PHP site" : "HTML site");
    }
    auto* copy = make_button(x + 786, y + 7, 70, 30, "Copy");
    copy->callback(copy_callback, this);
    auto* open = make_button(x + 864, y + 7, 70, 30, "Open");
    open->callback(open_callback, this);
  } else {
    auto* error = make_label(x + 310, y + 4, 462, row_height - 8,
                             site.value("error", "Invalid DNS label"), kFault, 11);
    error->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    make_label(x + 786, y, 148, row_height, "Rename the folder", kMuted, 11);
  }
  end();
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
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool force_show = argc > 1 && std::string_view(argv[1]) == "--show";
  App app(force_show);
  while (app.running()) { Fl::wait(0.25); }
  CoUninitialize(); return 0;
}
