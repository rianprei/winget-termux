#include "pch.h"
#include "ZipInstallerTermux.h"
#include <curl/curl.h>
#include <AppInstallerSHA256.h>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

namespace AppInstaller::Zip::Termux
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
            return GetHome() / ".winget" / "ziparchives" / packageId;
        }

        std::filesystem::path SymlinkPath(const std::string& commandAlias)
        {
            return GetPrefix() / "bin" / commandAlias;
        }

        // See PortableInstallerTermux.cpp's IsOurManagedSymlink for why this exists: never
        // remove/overwrite a $PREFIX/bin/<alias> that isn't already our own managed symlink.
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
            // Reject file:// and other non-http(s) schemes -- without this a manifest/source
            // URL pointing at file:///data/... would exfiltrate a local file disguised as a download.
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

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

        // Real exec (no shell) to the real Termux tar/unzip binary; no vendored zip parser.
        // argv-based, not system(), so path components can never be reinterpreted as shell syntax.
        bool RunReal(const std::string& bin, const std::vector<std::string>& args)
        {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(bin.c_str()));
            for (auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0) { return false; }
            if (pid == 0)
            {
                execvp(bin.c_str(), argv.data());
                _exit(127);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }

    ZipInstallResult InstallZip(
        const std::string& packageId,
        const std::string& downloadUrl,
        const std::string& expectedSha256Lowercase,
        const std::string& nestedRelativeFilePath,
        const std::string& commandAlias)
    {
        ZipInstallResult result;

        std::filesystem::path dir = PackageDir(packageId);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            result.Message = "failed to create install directory: " + ec.message();
            return result;
        }

        std::filesystem::path zipPath = dir / "archive.zip";

        // 1. Download real
        if (!DownloadReal(downloadUrl, zipPath))
        {
            result.Message = "download failed";
            return result;
        }

        // 2. Verify SHA256 real (of the zip itself, matching manifest InstallerSha256)
        auto hash = AppInstaller::Utility::SHA256::ComputeHashFromFile(zipPath);
        auto hashStr = ToLower(AppInstaller::Utility::SHA256::ConvertToString(hash));
        if (hashStr != ToLower(expectedSha256Lowercase))
        {
            std::filesystem::remove(zipPath);
            result.Message = "SHA256 mismatch: got " + hashStr + " expected " + ToLower(expectedSha256Lowercase);
            return result;
        }

        // 3. Extract real via real unzip/tar binary. Despite the "Zip" installer type name,
        // most real-world GitHub release assets for CLI tools are actually .tar.gz, not
        // .zip -- detected here by real magic bytes (gzip: 0x1f 0x8b; zip: "PK"), not by
        // trusting the URL's file extension, since a redirect or CDN can serve either
        // without a matching URL suffix.
        std::filesystem::path extractDir = dir / "extracted";
        std::filesystem::remove_all(extractDir, ec);
        std::filesystem::create_directories(extractDir, ec);

        unsigned char magic[2] = { 0, 0 };
        {
            std::ifstream probe(zipPath, std::ios::binary);
            probe.read(reinterpret_cast<char*>(magic), 2);
        }
        bool isGzip = (magic[0] == 0x1f && magic[1] == 0x8b);

        bool ok = isGzip
            ? RunReal("tar", { "-xzf", zipPath.string(), "-C", extractDir.string() })
            : RunReal("unzip", { "-o", "-q", zipPath.string(), "-d", extractDir.string() });
        if (!ok)
        {
            result.Message = std::string(isGzip ? "tar" : "unzip") + " extraction failed";
            return result;
        }

        // 4. Locate nested binary per manifest's NestedInstallerFiles[].RelativeFilePath.
        // Real zip-slip guard: even though the CLI dispatcher already rejects absolute/".."
        // RelativeFilePath before calling here, a symlink planted by a malicious archive
        // entry could still resolve outside extractDir -- weakly_canonical + prefix check
        // catches that too, not just the raw string.
        std::filesystem::path binaryPath = extractDir / nestedRelativeFilePath;
        if (!std::filesystem::exists(binaryPath))
        {
            result.Message = "nested installer file not found after extraction: " + nestedRelativeFilePath;
            return result;
        }
        auto canonExtractDir = std::filesystem::weakly_canonical(extractDir, ec);
        auto canonBinaryPath = std::filesystem::weakly_canonical(binaryPath, ec);
        if (ec || canonBinaryPath.string().rfind(canonExtractDir.string(), 0) != 0)
        {
            result.Message = "nested installer file escapes extraction directory: " + nestedRelativeFilePath;
            return result;
        }

        // 5. chmod +x real
        std::filesystem::permissions(binaryPath,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace, ec);

        // 6. Symlink real in $PREFIX/bin (idempotent: replace if it's already ours; refuse to
        // touch anything else that occupies this name).
        std::filesystem::path link = SymlinkPath(commandAlias);
        if (std::filesystem::exists(link, ec) && !IsOurManagedSymlink(link))
        {
            result.Message = "'" + commandAlias + "' already exists at " + link.string() + " and is not managed by winget-termux; refusing to overwrite it";
            std::filesystem::remove_all(dir, ec);
            return result;
        }
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(binaryPath, link, ec);
        if (ec)
        {
            result.Message = "symlink creation failed: " + ec.message();
            return result;
        }

        result.Success = true;
        result.ExtractDir = extractDir;
        result.BinaryPath = binaryPath;
        result.SymlinkPath = link;
        result.Message = "installed OK";
        return result;
    }

    bool UninstallZip(const std::string& packageId, const std::string& commandAlias)
    {
        std::error_code ec;

        std::filesystem::path link = SymlinkPath(commandAlias);
        if (IsOurManagedSymlink(link))
        {
            std::filesystem::remove(link, ec);
        }

        std::filesystem::path dir = PackageDir(packageId);
        std::filesystem::remove_all(dir, ec);

        return !std::filesystem::exists(link) && !std::filesystem::exists(dir);
    }

    bool IsCommandOnPath(const std::string& commandAlias)
    {
        std::filesystem::path link = SymlinkPath(commandAlias);
        std::error_code ec;
        return std::filesystem::exists(link, ec) && std::filesystem::is_symlink(link, ec) &&
            std::filesystem::exists(std::filesystem::read_symlink(link, ec), ec);
    }
}
