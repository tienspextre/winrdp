#pragma once

#include "config/profile.hpp"
#include "ui/manager_window.hpp"
#include "ui/session_window.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace winrdp {

struct CliOptions {
    bool show_version = false;
    bool smoke_test = false;
    bool list_monitors = false;
    bool force_fullscreen = false;
    bool force_windowed = false;
    bool force_multimon = false;
    bool force_no_bar = false;
    bool force_pinned_bar = false;
    std::vector<int32_t> selected_monitors;
    std::string target; // .rdp path, rdp:// URI, or host
};

class Application {
public:
    Application();
    ~Application();

    int run(int argc, char** argv);

    static CliOptions parse_cli(int argc, char** argv);

private:
    void load_css();
    void launch_session(ConnectionProfile profile);
    int run_smoke_test();

    AdwApplication* app_ = nullptr;
    ProfileStore store_;
    std::unique_ptr<ManagerWindow> manager_;
    std::vector<std::unique_ptr<SessionWindow>> sessions_;
    CliOptions cli_;
};

} // namespace winrdp
