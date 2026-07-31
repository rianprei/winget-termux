#pragma once
#include <string>
#include <filesystem>

// ponytail: real Portable install backend, not a stub. Ports the exact manual bash sequence
// already validated (download -> verify -> place -> chmod +x -> symlink in $PREFIX/bin) into
// the winget C++ codebase. This is the specification now, not a prototype.
namespace AppInstaller::Portable::Termux
{
    struct InstallResult
    {
        bool Success = false;
        std::string Message;
        std::filesystem::path PayloadPath;
        std::filesystem::path SymlinkPath;
    };

    // Installs a portable package: downloads the URL, verifies SHA256 against expectedSha256,
    // writes the payload to $HOME/.winget/programfiles/<packageId>/<fileName>, chmod +x, and
    // symlinks it to $PREFIX/bin/<commandAlias>.
    InstallResult InstallPortable(
        const std::string& packageId,
        const std::string& fileName,
        const std::string& commandAlias,
        const std::string& downloadUrl,
        const std::string& expectedSha256Lowercase);

    // Same as InstallPortable, but the payload is already sitting at sourceFile (already
    // downloaded and hashed by the caller) instead of being downloaded here -- avoids a
    // second network round-trip whose content isn't guaranteed to match the first.
    // sourceFile is moved (not copied) into place; it must not be reused after this call.
    InstallResult InstallPortableFromLocalFile(
        const std::string& packageId,
        const std::string& fileName,
        const std::string& commandAlias,
        const std::filesystem::path& sourceFile,
        const std::string& expectedSha256Lowercase);

    // Uninstalls a portable package: removes the symlink, the payload file, and the (now empty)
    // package directory. Safe to call even if some parts are already missing (idempotent).
    bool UninstallPortable(const std::string& packageId, const std::string& fileName, const std::string& commandAlias);

    // Returns true if the command alias currently resolves to a real, existing target via the
    // symlink in $PREFIX/bin (equivalent of `command -v <alias>` succeeding).
    bool IsCommandOnPath(const std::string& commandAlias);
}
