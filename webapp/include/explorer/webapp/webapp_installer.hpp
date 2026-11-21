#pragma once
// SOLID WebApp (PWA) Installer: installs any website as a standalone chromeless Linux desktop app.

#include <filesystem>
#include <string>
#include <vector>

namespace explorer::webapp {

struct InstalledWebApp {
    std::string app_id;
    std::string name;
    std::string url;
    std::string icon_name;
    std::filesystem::path desktop_path;
};

class IWebAppInstaller {
public:
    virtual ~IWebAppInstaller() = default;
    virtual InstalledWebApp install(const std::string& name,
                                    const std::string& url,
                                    const std::string& favicon_url) const = 0;
    virtual std::vector<InstalledWebApp> listInstalled() const = 0;
    virtual bool uninstall(const std::string& app_id) const = 0;
    virtual void launch(const InstalledWebApp& app) const = 0;
};

class LinuxWebAppInstaller final : public IWebAppInstaller {
public:
    LinuxWebAppInstaller(std::filesystem::path apps_dir,
                         std::filesystem::path icons_dir,
                         std::filesystem::path browser_bin);

    InstalledWebApp install(const std::string& name,
                            const std::string& url,
                            const std::string& favicon_url) const override;
    std::vector<InstalledWebApp> listInstalled() const override;
    bool uninstall(const std::string& app_id) const override;
    void launch(const InstalledWebApp& app) const override;

    static std::string makeAppId(const std::string& name, const std::string& url);

private:
    std::filesystem::path apps_dir_;
    std::filesystem::path icons_dir_;
    std::filesystem::path browser_bin_;
};

}  // namespace explorer::webapp
