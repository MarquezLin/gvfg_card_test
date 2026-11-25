#pragma once
#include <filesystem>
#include <string>

struct CardStartupState {
    bool installed = false;
    bool valid = false;
    std::wstring text;
};

class CardStartupManager {
public:
    explicit CardStartupManager(std::filesystem::path executable,
                            std::filesystem::path testFolder = {});
    CardStartupState queryStartup();
    bool installStartup(std::wstring &error);
    bool uninstallStartup(std::wstring &error);
private:
    bool startupFolder(std::filesystem::path &folder, std::wstring &error);
    std::filesystem::path m_dir;
    std::filesystem::path m_folderOverride;
};
