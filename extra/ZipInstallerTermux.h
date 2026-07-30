#pragma once
#include <string>
#include <filesystem>

namespace AppInstaller::Zip::Termux
{
    struct ZipInstallResult
    {
        bool Success = false;
        std::string Message;
        std::filesystem::path ExtractDir;
        std::filesystem::path BinaryPath;
        std::filesystem::path SymlinkPath;
    };

    // ponytail: extraction shells out to the real, already-installed Termux `unzip` binary
    // (native platform tool, rung 4 of the ladder) instead of vendoring a zip-parsing library.
    ZipInstallResult InstallZip(
        const std::string& packageId,
        const std::string& downloadUrl,
        const std::string& expectedSha256Lowercase,
        const std::string& nestedRelativeFilePath,
        const std::string& commandAlias);

    bool UninstallZip(const std::string& packageId, const std::string& commandAlias);

    bool IsCommandOnPath(const std::string& commandAlias);
}
