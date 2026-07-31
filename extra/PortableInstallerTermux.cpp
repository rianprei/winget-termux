#include "pch.h"
#include "PortableInstallerTermux.h"
#include <curl/curl.h>
#include <AppInstallerSHA256.h>
#include <fstream>
#include <unistd.h>
#include <cstdlib>
#include <sys/stat.h>

namespace AppInstaller::Portable::Termux
{
    namespace
    {
        std::filesystem::path GetHome()
        {
            const char* home = std::getenv("HOME");
            return home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        }

        std::filesystem::path GetPrefix()
        {
            const char* prefix = std::getenv("PREFIX");
            return prefix ? std::filesystem::path(prefix) : std::filesystem::path("/data/data/com.termux/files/usr");
        }

        std::filesystem::path PackageDir(const std::string& packageId)
        {
            return GetHome() / ".winget" / "programfiles" / packageId;
        }

        std::filesystem::path SymlinkPath(const std::string& commandAlias)
        {
            return GetPrefix() / "bin" / commandAlias;
        }

        // Real safety check: never remove/overwrite $PREFIX/bin/<alias> unless it's already
        // our own symlink (pointing somewhere under ~/.winget/). A manifest whose alias
        // collides with an existing real command (a pkg-installed binary, another package's
        // symlink) must not destroy it -- this exact bug once deleted a real `gh` install.
        bool IsOurManagedSymlink(const std::filesystem::path& link)
        {
            std::error_code ec;
            if (!std::filesystem::is_symlink(link, ec))
            {
                return false;
            }
            auto target = std::filesystem::read_symlink(link, ec);
            if (ec)
            {
                return false;
            }
            auto managedRoot = (GetHome() / ".winget").string();
            return target.string().rfind(managedRoot, 0) == 0;
        }

        size_t CurlWrite(void* contents, size_t size, size_t nmemb, void* userp)
        {
            std::ofstream* out = static_cast<std::ofstream*>(userp);
            out->write(static_cast<char*>(contents), static_cast<std::streamsize>(size * nmemb));
            return size * nmemb;
        }

        bool DownloadReal(const std::string& url, const std::filesystem::path& dest)
        {
            std::ofstream out(dest, std::ios::binary);
            if (!out) { return false; }

            CURL* curl = curl_easy_init();
            if (!curl) { return false; }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);
            out.close();

            return res == CURLE_OK && httpCode == 200;
        }

        std::string ToLower(std::string s)
        {
            for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return s;
        }
    }

    // Shared tail of the install pipeline once the payload is already sitting at
    // payloadPath (either just downloaded, or moved in from a caller who already
    // downloaded and hashed it once -- e.g. install-url, which used to download twice:
    // once to compute the hash, once more here. Real TOCTOU/network-dependency reduction.
    InstallResult FinishInstallFromLocalFile(
        const std::string& packageId,
        const std::filesystem::path& dir,
        const std::filesystem::path& payloadPath,
        const std::string& commandAlias,
        const std::string& expectedSha256Lowercase)
    {
        InstallResult result;
        std::error_code ec;

        // 2. Verify SHA256 real
        auto hash = AppInstaller::Utility::SHA256::ComputeHashFromFile(payloadPath);
        auto hashStr = ToLower(AppInstaller::Utility::SHA256::ConvertToString(hash));
        if (hashStr != ToLower(expectedSha256Lowercase))
        {
            std::filesystem::remove(payloadPath);
            result.Message = "SHA256 mismatch: got " + hashStr + " expected " + ToLower(expectedSha256Lowercase);
            return result;
        }

        // 3. chmod +x real
        std::filesystem::permissions(payloadPath,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace, ec);
        if (ec)
        {
            result.Message = "chmod +x failed: " + ec.message();
            return result;
        }

        // 4. Symlink real in $PREFIX/bin (idempotent: replace if it's already ours; refuse to
        // touch anything else that occupies this name).
        std::filesystem::path link = SymlinkPath(commandAlias);
        if (std::filesystem::exists(link, ec) && !IsOurManagedSymlink(link))
        {
            result.Message = "'" + commandAlias + "' already exists at " + link.string() + " and is not managed by winget-termux; refusing to overwrite it";
            std::filesystem::remove_all(dir, ec);
            return result;
        }
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(payloadPath, link, ec);
        if (ec)
        {
            result.Message = "symlink creation failed: " + ec.message();
            return result;
        }

        result.Success = true;
        result.PayloadPath = payloadPath;
        result.SymlinkPath = link;
        result.Message = "installed OK";
        return result;
    }

    InstallResult InstallPortable(
        const std::string& packageId,
        const std::string& fileName,
        const std::string& commandAlias,
        const std::string& downloadUrl,
        const std::string& expectedSha256Lowercase)
    {
        InstallResult result;

        std::filesystem::path dir = PackageDir(packageId);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            result.Message = "failed to create install directory: " + ec.message();
            return result;
        }

        std::filesystem::path payloadPath = dir / fileName;
        if (!DownloadReal(downloadUrl, payloadPath))
        {
            result.Message = "download failed";
            return result;
        }

        return FinishInstallFromLocalFile(packageId, dir, payloadPath, commandAlias, expectedSha256Lowercase);
    }

    InstallResult InstallPortableFromLocalFile(
        const std::string& packageId,
        const std::string& fileName,
        const std::string& commandAlias,
        const std::filesystem::path& sourceFile,
        const std::string& expectedSha256Lowercase)
    {
        InstallResult result;

        std::filesystem::path dir = PackageDir(packageId);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            result.Message = "failed to create install directory: " + ec.message();
            return result;
        }

        std::filesystem::path payloadPath = dir / fileName;
        std::filesystem::rename(sourceFile, payloadPath, ec);
        if (ec)
        {
            // cross-device rename (e.g. tmp on a different mount) -- fall back to copy.
            std::filesystem::copy_file(sourceFile, payloadPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(sourceFile);
            if (ec)
            {
                result.Message = "failed to move downloaded file into place: " + ec.message();
                return result;
            }
        }

        return FinishInstallFromLocalFile(packageId, dir, payloadPath, commandAlias, expectedSha256Lowercase);
    }

    bool UninstallPortable(const std::string& packageId, const std::string& fileName, const std::string& commandAlias)
    {
        std::error_code ec;
        bool ok = true;

        std::filesystem::path link = SymlinkPath(commandAlias);
        if (IsOurManagedSymlink(link))
        {
            std::filesystem::remove(link, ec); // no error if missing
        }
        // else: not ours (or already gone) -- never touch it.

        std::filesystem::path dir = PackageDir(packageId);
        std::filesystem::path payloadPath = dir / fileName;
        std::filesystem::remove(payloadPath, ec);

        std::filesystem::remove(dir, ec); // only succeeds if empty; that's intended (no leftover check)

        // Confirm no leftovers
        ok = !std::filesystem::exists(link) && !std::filesystem::exists(payloadPath) && !std::filesystem::exists(dir);
        return ok;
    }

    bool IsCommandOnPath(const std::string& commandAlias)
    {
        std::filesystem::path link = SymlinkPath(commandAlias);
        std::error_code ec;
        return std::filesystem::exists(link, ec) && std::filesystem::is_symlink(link, ec) &&
            std::filesystem::exists(std::filesystem::read_symlink(link, ec), ec);
    }
}
