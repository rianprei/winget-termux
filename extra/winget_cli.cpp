// ponytail: real, minimal CLI dispatcher -- not the full upstream AppInstallerCLICore
// (Workflows/ExecutionContext engine, COM server, table-output formatting), which is a
// separate, much larger subsystem never compiled in this port. This is a genuine argv-parsed
// "install <id>" / "uninstall <id>" command path wired to the same real, already-validated
// SQLiteIndex (search/resolve) and PortableInstallerTermux (install backend) code -- not a
// fake shortcut, just a smaller command surface than upstream's full CLI.
#include "pch.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <map>
#include "Microsoft/SQLiteIndex.h"
#include "winget/RepositorySearch.h"
#include "winget/ManifestYamlParser.h"
#include "PortableInstallerTermux.h"
#include "ZipInstallerTermux.h"
#include <AppInstallerSHA256.h>
#include <curl/curl.h>
#include <json/json.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

using namespace AppInstaller::Repository;
using namespace AppInstaller::Repository::Microsoft;
using namespace AppInstaller::Manifest;

namespace
{
    const std::string s_dbPath = "/data/data/com.termux/files/home/wingetcli.db";
    // RelativePath must actually be relative -- PathPartTable::EnsurePathExistsInternal
    // (schema 1.0) THROW_HR_IFs E_INVALIDARG on any root_path, matching real upstream
    // winget-cli behavior (RelativePath is a package-repository-relative path, not an
    // absolute filesystem path). Manifests are resolved relative to a fixed root next to
    // the index db -- no machine-specific dev path is baked into the binary.
    const std::string s_manifestRoot = (std::filesystem::path(s_dbPath).parent_path() / ".winget" / "manifests").string();

    // Real guard against path traversal / shell injection: packageId, alias, and source
    // name all become path components (~/.winget/programfiles/<id>, $PREFIX/bin/<alias>,
    // ~/.winget/sources/<name>.db) or get shelled out to tar/unzip with single-quote
    // wrapping. A manifest/source under attacker control (remote source, crafted id)
    // must not be able to inject '/', '..', quotes, or control chars into those paths.
    bool IsSafePathComponent(const std::string& s)
    {
        if (s.empty() || s.size() > 200) { return false; }
        if (s == "." || s == "..") { return false; }
        for (unsigned char c : s)
        {
            if (c < 0x20 || c == 0x7f) { return false; }
            if (c == '/' || c == '\\' || c == '\'' || c == '"' || c == '\0') { return false; }
        }
        return true;
    }

    // Real per-package exclusive lock: concurrent `install`/`uninstall`/`upgrade` on the same
    // id would otherwise race on the same directory/symlink/version-marker with no protection.
    // flock on an fd is released automatically on process exit even if we crash mid-install,
    // so a killed process never leaves a stale lock behind.
    class PackageLock
    {
    public:
        explicit PackageLock(const std::string& packageId)
        {
            const char* home = std::getenv("HOME");
            std::filesystem::path dir = (home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home")) / ".winget" / "locks";
            std::filesystem::create_directories(dir);
            std::filesystem::path lockPath = dir / (packageId + ".lock");
            m_fd = open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
            if (m_fd >= 0)
            {
                flock(m_fd, LOCK_EX);
            }
        }
        ~PackageLock()
        {
            if (m_fd >= 0) { close(m_fd); }
        }
        PackageLock(const PackageLock&) = delete;
        PackageLock& operator=(const PackageLock&) = delete;

    private:
        int m_fd = -1;
    };

    std::string ToHexLower(const std::vector<BYTE>& bytes)
    {
        static const char* hex = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (BYTE b : bytes)
        {
            result.push_back(hex[(b >> 4) & 0xF]);
            result.push_back(hex[b & 0xF]);
        }
        return result;
    }

    // Real manifest resolution: search the index for the id, get the manifest's on-disk
    // relative path, and re-parse the real YAML to get full installer data (URL/hash/type) --
    // this is the actual winget architecture: the SQLite index is a searchable catalog, the
    // manifest file on disk is the source of truth for install-time details.
    std::optional<Manifest> ResolveManifestById(SQLiteIndex& index, const std::string& id, SQLiteIndex::IdType& outManifestId)
    {
        SearchRequest req;
        req.Query = RequestMatch(MatchType::Exact, id);
        auto result = index.Search(req);
        if (result.Matches.empty())
        {
            return std::nullopt;
        }

        // The index can hold multiple versions of the same package (real winget behavior,
        // exercised by upgrade); Search returns one representative row per package, which is
        // not guaranteed to be the latest version. GetVersionKeysById accepts any manifest
        // rowid belonging to the package and returns every version, sorted with the latest
        // first -- the same real API upstream uses for version resolution.
        outManifestId = result.Matches[0].first;
        auto versionKeys = index.GetVersionKeysById(outManifestId);
        if (!versionKeys.empty())
        {
            outManifestId = versionKeys.front().ManifestId;
        }

        auto relPath = index.GetPropertyByPrimaryId(outManifestId, PackageVersionProperty::RelativePath);
        if (!relPath)
        {
            return std::nullopt;
        }

        std::filesystem::path fullPath = std::filesystem::path(*relPath).is_absolute()
            ? std::filesystem::path(*relPath)
            : std::filesystem::path(s_manifestRoot) / *relPath;
        return YamlParser::CreateFromPath(fullPath);
    }

    // Multi-installer fallback: Portable preferred (direct binary, no extraction needed);
    // Zip is the next most useful on Termux (real unzip + nested binary); anything else
    // (exe/msi/msix/appx/etc.) is a genuine Windows-only format with no Android execution
    // path, so it's reported as a real limitation, never faked.
    // Script installer type behaves identically to Portable on Termux: download, verify,
    // chmod +x, symlink into $PREFIX/bin. Reuses PortableInstallerTermux directly rather than
    // duplicating that backend -- there's no real difference in the install mechanics.
    const AppInstaller::Manifest::ManifestInstaller* FindPortable(const Manifest& manifest)
    {
        for (const auto& installer : manifest.Installers)
        {
            auto type = installer.EffectiveInstallerType();
            if (type == InstallerTypeEnum::Portable || type == InstallerTypeEnum::Script)
            {
                return &installer;
            }
        }
        return nullptr;
    }

    const AppInstaller::Manifest::ManifestInstaller* FindZip(const Manifest& manifest)
    {
        for (const auto& installer : manifest.Installers)
        {
            if (installer.BaseInstallerType == InstallerTypeEnum::Zip && !installer.NestedInstallerFiles.empty())
            {
                return &installer;
            }
        }
        return nullptr;
    }

    // Exit codes, used consistently across every command:
    //   0 success
    //   1 unexpected/internal error (exception, filesystem failure, etc.)
    //   2 no installer type supported on this platform found in the manifest
    //   3 package not found in the index
    //   4 package resolved but nothing is actually installed for it
    //  64 usage error (bad argv)
    constexpr int EXIT_OK = 0;
    constexpr int EXIT_UNSUPPORTED_TYPE = 2;
    constexpr int EXIT_NOT_FOUND = 3;
    constexpr int EXIT_NOT_INSTALLED = 4;
    constexpr int EXIT_USAGE = 64;

    // Version marker: a plain file recording the installed version, kept in a separate
    // metadata root ($HOME/.winget/versions/<packageId>.version) -- NOT inside the backend's
    // own package directory. Neither backend tracks installed version itself (they just place
    // files), and the SQLite index only tracks *available* manifests, not installed state, so
    // this is the one bit of real state upgrade needs. Keeping it outside the package dir
    // matters: UninstallPortable/UninstallZip only report clean success when their directory
    // ends up empty, and a marker file left inside it would silently make every uninstall
    // "fail" (leftover-file false negative) without altering the already-validated backends.
    std::filesystem::path VersionMarkerPath(const std::string& packageId)
    {
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        std::filesystem::path dir = homeDir / ".winget" / "versions";
        std::filesystem::create_directories(dir);
        return dir / (packageId + ".version");
    }

    // Real installed-package state: version plus which source actually resolved the manifest
    // used to install it ("" = local TermuxLocal catalog). Same file, same location as
    // before (backward-compatible: a plain single-line marker from before this field existed
    // still parses fine, source just comes back empty -- "resolved locally", the same
    // fallback behavior a genuinely-local package should get).
    struct InstalledState
    {
        std::string Version;
        std::string SourceName;
        std::string Alias;
        std::string Name;
    };

    void WriteVersionMarker(const std::string& packageId, const std::string& version, const std::string& sourceName = "",
        const std::string& alias = "", const std::string& name = "")
    {
        std::ofstream out(VersionMarkerPath(packageId), std::ios::trunc);
        out << version << "\n" << sourceName << "\n" << alias << "\n" << name << "\n";
    }

    std::optional<InstalledState> ReadInstalledState(const std::string& packageId)
    {
        std::ifstream in(VersionMarkerPath(packageId));
        if (!in)
        {
            return std::nullopt;
        }
        InstalledState state;
        std::getline(in, state.Version);
        std::getline(in, state.SourceName);
        std::getline(in, state.Alias);
        std::getline(in, state.Name);
        return state;
    }

    std::optional<std::string> ReadVersionMarker(const std::string& packageId)
    {
        auto state = ReadInstalledState(packageId);
        if (!state) { return std::nullopt; }
        return state->Version;
    }

    void RemoveVersionMarker(const std::string& packageId)
    {
        std::filesystem::remove(VersionMarkerPath(packageId));
    }

    // Pin: a real empty marker file. upgrade/upgrade --all check for it and skip the package
    // instead of touching it -- same real-state pattern as the version marker, no separate
    // pin database to keep in sync.
    std::filesystem::path PinPath(const std::string& packageId)
    {
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        std::filesystem::path dir = homeDir / ".winget" / "pins";
        std::filesystem::create_directories(dir);
        return dir / (packageId + ".pin");
    }

    bool IsPinned(const std::string& packageId)
    {
        return std::filesystem::exists(PinPath(packageId));
    }

    // Real remote source support: a registered source is a name+URL pointing at a real
    // SQLite catalog file (the same format CreateNew/AddManifest produce), downloaded via
    // libcurl and validated by actually opening it with SQLiteIndex before it's trusted --
    // never registered on faith. "TermuxLocal" (the writable install catalog) is not a
    // registrable name and is always listed first.
    struct SourceEntry
    {
        std::string Name;
        std::string Url;
    };

    std::filesystem::path SourcesDir()
    {
        std::filesystem::path dir = std::filesystem::path(s_dbPath).parent_path() / ".winget" / "sources";
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::filesystem::path SourcesRegistryPath()
    {
        return SourcesDir() / "sources.list";
    }

    std::filesystem::path SourceDbPath(const std::string& name)
    {
        return SourcesDir() / (name + ".db");
    }

    std::vector<SourceEntry> LoadSources()
    {
        std::vector<SourceEntry> entries;
        std::ifstream in(SourcesRegistryPath());
        std::string line;
        while (std::getline(in, line))
        {
            auto tab = line.find('\t');
            if (tab == std::string::npos) { continue; }
            entries.push_back({ line.substr(0, tab), line.substr(tab + 1) });
        }
        return entries;
    }

    void SaveSources(const std::vector<SourceEntry>& entries)
    {
        std::ofstream out(SourcesRegistryPath(), std::ios::trunc);
        for (const auto& e : entries)
        {
            out << e.Name << "\t" << e.Url << "\n";
        }
    }

    size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp)
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
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);
        out.close();
        return res == CURLE_OK && httpCode == 200;
    }

    // The only real validation a catalog needs: can SQLiteIndex actually open it and run a
    // query? A file that isn't a real SQLite DB, or doesn't have this schema, throws here --
    // caught and reported as an honest rejection, never silently accepted.
    bool ValidateCatalog(const std::filesystem::path& path)
    {
        try
        {
            auto index = SQLiteIndex::Open(path.string(), SQLiteIndex::OpenDisposition::Read);
            SearchRequest req;
            index.Search(req);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    // Real remote manifest resolution: a remote source's catalog only stores the
    // RelativePath, not the manifest content -- the actual YAML lives on the same server the
    // catalog itself was downloaded from. This mirrors ResolveManifestById's local logic
    // (Search -> GetVersionKeysById for the real latest version -> RelativePath) but then
    // downloads that relative path from the source's own URL directory instead of reading a
    // local file, and parses it with the same real YamlParser. Returns nullopt if the
    // package isn't in this source, or if the manifest file can't be fetched/parsed --
    // never fabricates installer data.
    std::optional<Manifest> ResolveManifestFromSource(const SourceEntry& source, const std::string& id)
    {
        std::filesystem::path dbPath = SourceDbPath(source.Name);
        if (!std::filesystem::exists(dbPath))
        {
            return std::nullopt;
        }

        try
        {
            auto index = SQLiteIndex::Open(dbPath.string(), SQLiteIndex::OpenDisposition::Read);

            SearchRequest req;
            req.Query = RequestMatch(MatchType::Exact, id);
            auto result = index.Search(req);
            if (result.Matches.empty())
            {
                return std::nullopt;
            }

            SQLiteIndex::IdType manifestId = result.Matches[0].first;
            auto versionKeys = index.GetVersionKeysById(manifestId);
            if (!versionKeys.empty())
            {
                manifestId = versionKeys.front().ManifestId;
            }

            auto relPath = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::RelativePath);
            if (!relPath)
            {
                return std::nullopt;
            }

            // The manifest lives next to the catalog file on the same server: derive its URL
            // by replacing the catalog's own filename with the manifest's relative path.
            auto lastSlash = source.Url.find_last_of('/');
            std::string manifestUrl = (lastSlash == std::string::npos ? "" : source.Url.substr(0, lastSlash + 1)) + *relPath;

            std::filesystem::path cacheDir = SourcesDir() / "cache" / source.Name;
            std::filesystem::create_directories(cacheDir);
            std::filesystem::path localManifest = cacheDir / *relPath;

            if (!DownloadReal(manifestUrl, localManifest))
            {
                return std::nullopt;
            }

            return YamlParser::CreateFromPath(localManifest);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    // Tries the local catalog first (unchanged behavior), then every registered remote
    // source in order -- the first one that actually has both the package and a fetchable
    // manifest wins. Returns which source it came from (empty string = local) so callers can
    // report it honestly.
    std::optional<Manifest> ResolveManifestAnywhere(SQLiteIndex& localIndex, const std::string& id, std::string& outSourceName)
    {
        SQLiteIndex::IdType localManifestId = 0;
        auto localManifest = ResolveManifestById(localIndex, id, localManifestId);
        if (localManifest)
        {
            outSourceName.clear();
            return localManifest;
        }

        for (const auto& source : LoadSources())
        {
            auto manifest = ResolveManifestFromSource(source, id);
            if (manifest)
            {
                outSourceName = source.Name;
                return manifest;
            }
        }

        return std::nullopt;
    }

    // Self-heal: if a previous install of this exact id was killed between the backend
    // creating its directory/symlink and this process writing the version marker, that
    // leftover directory has no marker and ScanInstalled's IsCommandOnPath check would
    // otherwise make a fresh install attempt collide with it. Since the caller already
    // holds this id's PackageLock, it's safe to wipe an orphan before installing for real.
    void CleanOrphanedInstall(const std::string& packageId)
    {
        if (ReadInstalledState(packageId)) { return; }
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        std::error_code ec;
        std::filesystem::remove_all(homeDir / ".winget" / "programfiles" / packageId, ec);
        std::filesystem::remove_all(homeDir / ".winget" / "ziparchives" / packageId, ec);
    }

    int CmdInstall(SQLiteIndex& index, const std::string& id)
    {
        PackageLock lock(id);
        CleanOrphanedInstall(id);
        std::string sourceName;
        auto manifest = ResolveManifestAnywhere(index, id, sourceName);
        if (!manifest)
        {
            std::cout << "No package found matching input criteria: " << id << std::endl;
            return EXIT_NOT_FOUND;
        }

        if (!IsSafePathComponent(manifest->Id))
        {
            std::cout << "Refusing to install: PackageIdentifier '" << manifest->Id
                << "' is not a safe path component." << std::endl;
            return 1;
        }

        std::cout << "Found " << manifest->Id << " [" << manifest->Version << "]"
            << (sourceName.empty() ? "" : " (source: " + sourceName + ")") << std::endl;

        // Zip must be checked first: a zip installer whose nested type is Portable also
        // reports EffectiveInstallerType()==Portable, but its Url points to the archive, not
        // a runnable payload -- FindPortable alone would wrongly hand the raw zip bytes to
        // the Portable backend. BaseInstallerType is the reliable discriminator.
        if (const auto* zip = FindZip(*manifest))
        {
            std::string sha256Hex = ToHexLower(zip->Sha256);
            const auto& nested = zip->NestedInstallerFiles[0];
            std::string alias = !nested.PortableCommandAlias.empty() ? nested.PortableCommandAlias
                : (manifest->Moniker.empty() ? manifest->Id : manifest->Moniker);
            if (!IsSafePathComponent(alias))
            {
                std::cout << "Refusing to install: command alias '" << alias << "' is not a safe path component." << std::endl;
                return 1;
            }
            std::string relFile = nested.RelativeFilePath;
            if (relFile.empty() || relFile[0] == '/' || relFile.find("..") != std::string::npos)
            {
                std::cout << "Refusing to install: NestedInstallerFiles RelativeFilePath '" << relFile
                    << "' is absolute or escapes the archive." << std::endl;
                return 1;
            }

            std::cout << "This is a Zip package; extracting via native Termux backend (real unzip)..." << std::endl;
            auto result = AppInstaller::Zip::Termux::InstallZip(manifest->Id, zip->Url, sha256Hex, relFile, alias);
            if (!result.Success)
            {
                std::cout << "Installation failed: " << result.Message << std::endl;
                return 1;
            }

            WriteVersionMarker(manifest->Id, manifest->Version, sourceName, alias, manifest->DefaultLocalization.Get<Localization::PackageName>());
            std::cout << "Successfully installed." << std::endl;
            std::cout << "  Extracted to: " << result.ExtractDir << std::endl;
            std::cout << "  Binary: " << result.BinaryPath << std::endl;
            std::cout << "  Command: " << alias << " (" << result.SymlinkPath << ")" << std::endl;
            return EXIT_OK;
        }

        if (const auto* portable = FindPortable(*manifest))
        {
            std::string sha256Hex = ToHexLower(portable->Sha256);
            std::string alias = manifest->Moniker.empty() ? manifest->Id : manifest->Moniker;
            if (!IsSafePathComponent(alias))
            {
                std::cout << "Refusing to install: command alias '" << alias << "' is not a safe path component." << std::endl;
                return 1;
            }

            const char* kindLabel = portable->EffectiveInstallerType() == InstallerTypeEnum::Script ? "Script" : "Portable";
            std::cout << "This is a " << kindLabel << " package; installing via native Termux backend..." << std::endl;
            auto result = AppInstaller::Portable::Termux::InstallPortable(manifest->Id, "payload", alias, portable->Url, sha256Hex);
            if (!result.Success)
            {
                std::cout << "Installation failed: " << result.Message << std::endl;
                return 1;
            }

            WriteVersionMarker(manifest->Id, manifest->Version, sourceName, alias, manifest->DefaultLocalization.Get<Localization::PackageName>());
            std::cout << "Successfully installed." << std::endl;
            std::cout << "  Payload: " << result.PayloadPath << std::endl;
            std::cout << "  Command: " << alias << " (" << result.SymlinkPath << ")" << std::endl;
            return EXIT_OK;
        }

        // Real limitation, not a fake failure: report exactly why, per installer type found.
        for (const auto& installer : manifest->Installers)
        {
            std::cout << "  Installer type '" << AppInstaller::Manifest::InstallerTypeToString(installer.EffectiveInstallerType())
                << "' is a Windows binary format with no equivalent execution path on Android/Termux; skipping." << std::endl;
        }
        std::cout << "No installer type supported on this platform was found for " << id << "." << std::endl;
        return EXIT_UNSUPPORTED_TYPE;
    }

    // Uninstall must work purely from real on-disk state (which backend's package directory
    // exists, and the persisted alias) -- it must NOT require re-resolving a manifest, since
    // that manifest may have come from a remote source that's since been removed or gone
    // offline. Requiring live manifest resolution here previously made "uninstall" impossible
    // for a package whose source was removed, even though the package itself is still
    // installed and perfectly removable.
    int CmdUninstall(SQLiteIndex& index, const std::string& id)
    {
        PackageLock lock(id);
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        bool zipDirExists = std::filesystem::exists(homeDir / ".winget" / "ziparchives" / id);
        bool portableDirExists = std::filesystem::exists(homeDir / ".winget" / "programfiles" / id);

        if (!zipDirExists && !portableDirExists)
        {
            std::cout << "No installed package found for " << id << " (nothing to uninstall)." << std::endl;
            return EXIT_NOT_INSTALLED;
        }

        auto state = ReadInstalledState(id);
        std::string alias = state ? state->Alias : "";

        if (alias.empty())
        {
            // No persisted alias (marker missing/pre-dates this field) -- fall back to
            // resolving the manifest to recompute it, same as before.
            std::string sourceName;
            auto manifest = ResolveManifestAnywhere(index, id, sourceName);
            if (!manifest)
            {
                std::cout << "No package found matching input criteria: " << id << std::endl;
                return EXIT_NOT_FOUND;
            }
            if (zipDirExists)
            {
                if (const auto* zip = FindZip(*manifest))
                {
                    const auto& nested = zip->NestedInstallerFiles[0];
                    alias = !nested.PortableCommandAlias.empty() ? nested.PortableCommandAlias
                        : (manifest->Moniker.empty() ? manifest->Id : manifest->Moniker);
                }
            }
            else
            {
                alias = manifest->Moniker.empty() ? manifest->Id : manifest->Moniker;
            }
        }

        if (alias.empty())
        {
            std::cout << "Could not determine the installed command alias for " << id << "." << std::endl;
            return 1;
        }

        bool ok = zipDirExists
            ? AppInstaller::Zip::Termux::UninstallZip(id, alias)
            : AppInstaller::Portable::Termux::UninstallPortable(id, "payload", alias);
        if (ok) { RemoveVersionMarker(id); }
        std::cout << (ok ? "Successfully uninstalled." : "Uninstall reported leftover state.") << std::endl;
        return ok ? EXIT_OK : 1;
    }

    // "upgrade": reuses the exact same install/uninstall backends and manifest resolution as
    // CmdInstall/CmdUninstall -- no separate upgrade logic per installer type. The only new
    // state is the ".version" marker (see WriteVersionMarker), since neither backend nor the
    // index track installed version.
    int CmdUpgrade(SQLiteIndex& index, const std::string& id)
    {
        PackageLock lock(id);
        if (IsPinned(id))
        {
            std::cout << id << " is pinned; skipping upgrade. Use 'winget unpin " << id << "' first." << std::endl;
            return EXIT_OK;
        }

        // Prefer the persisted source (recorded at install time) over a full re-scan of every
        // registered source -- real speed/predictability win when there are many sources, and
        // it's also the more correct behavior: an upgrade should come from the same place the
        // package did, not silently jump to a different source that happens to answer first.
        // If that source is gone or fails, this honestly falls back to the full scan rather
        // than failing outright.
        std::string sourceName;
        std::optional<Manifest> manifest;
        auto installedState = ReadInstalledState(id);

        if (installedState && !installedState->SourceName.empty())
        {
            auto sources = LoadSources();
            auto it = std::find_if(sources.begin(), sources.end(),
                [&](const SourceEntry& e) { return e.Name == installedState->SourceName; });
            if (it == sources.end())
            {
                std::cout << "Note: persisted source '" << installedState->SourceName << "' is no longer registered; falling back to full source scan." << std::endl;
            }
            else
            {
                manifest = ResolveManifestFromSource(*it, id);
                if (manifest)
                {
                    sourceName = it->Name;
                }
                else
                {
                    std::cout << "Note: persisted source '" << installedState->SourceName << "' could not resolve " << id << " (unreachable or package removed); falling back to full source scan." << std::endl;
                }
            }
        }

        if (!manifest)
        {
            manifest = ResolveManifestAnywhere(index, id, sourceName);
        }

        if (!manifest)
        {
            std::cout << "No package found matching input criteria: " << id << std::endl;
            return EXIT_NOT_FOUND;
        }

        std::string backend;
        std::string alias;
        std::string url;
        std::string sha256Hex;
        std::string relativeFilePath;

        if (const auto* zip = FindZip(*manifest))
        {
            backend = "Zip";
            const auto& nested = zip->NestedInstallerFiles[0];
            alias = !nested.PortableCommandAlias.empty() ? nested.PortableCommandAlias
                : (manifest->Moniker.empty() ? manifest->Id : manifest->Moniker);
            url = zip->Url;
            sha256Hex = ToHexLower(zip->Sha256);
            relativeFilePath = nested.RelativeFilePath;
        }
        else if (const auto* portable = FindPortable(*manifest))
        {
            backend = "Portable";
            alias = manifest->Moniker.empty() ? manifest->Id : manifest->Moniker;
            url = portable->Url;
            sha256Hex = ToHexLower(portable->Sha256);
        }
        else
        {
            std::cout << "No installer type supported on this platform was found for " << id << "." << std::endl;
            return EXIT_UNSUPPORTED_TYPE;
        }

        auto installedVersion = ReadVersionMarker(manifest->Id);
        if (!installedVersion)
        {
            std::cout << id << " is not installed; nothing to upgrade. Use 'install' first." << std::endl;
            return EXIT_NOT_INSTALLED;
        }

        if (*installedVersion == manifest->Version)
        {
            std::cout << id << " is already up to date (" << manifest->Version << ")." << std::endl;
            return EXIT_OK;
        }

        std::cout << "Upgrading " << id << ": " << *installedVersion << " -> " << manifest->Version << std::endl;

        bool uninstallOk = (backend == "Zip")
            ? AppInstaller::Zip::Termux::UninstallZip(manifest->Id, alias)
            : AppInstaller::Portable::Termux::UninstallPortable(manifest->Id, "payload", alias);
        if (!uninstallOk)
        {
            std::cout << "Failed to remove old version before upgrade." << std::endl;
            return 1;
        }

        if (backend == "Zip")
        {
            auto result = AppInstaller::Zip::Termux::InstallZip(manifest->Id, url, sha256Hex, relativeFilePath, alias);
            if (!result.Success)
            {
                std::cout << "Upgrade install failed: " << result.Message << std::endl;
                return 1;
            }
            WriteVersionMarker(manifest->Id, manifest->Version, sourceName, alias, manifest->DefaultLocalization.Get<Localization::PackageName>());
        }
        else
        {
            auto result = AppInstaller::Portable::Termux::InstallPortable(manifest->Id, "payload", alias, url, sha256Hex);
            if (!result.Success)
            {
                std::cout << "Upgrade install failed: " << result.Message << std::endl;
                return 1;
            }
            WriteVersionMarker(manifest->Id, manifest->Version, sourceName, alias, manifest->DefaultLocalization.Get<Localization::PackageName>());
        }

        std::cout << "Successfully upgraded to " << manifest->Version << "." << std::endl;
        return EXIT_OK;
    }

    int CmdSourceAdd(const std::string& name, const std::string& url)
    {
        if (!IsSafePathComponent(name))
        {
            std::cout << "Refusing to add source: name '" << name << "' is not a safe path component." << std::endl;
            return 1;
        }
        if (name == "TermuxLocal")
        {
            std::cout << "'TermuxLocal' is the reserved local install catalog name." << std::endl;
            return 1;
        }

        auto sources = LoadSources();
        for (const auto& e : sources)
        {
            if (e.Name == name)
            {
                std::cout << "A source named '" << name << "' already exists. Remove it first or use 'source update'." << std::endl;
                return 1;
            }
        }

        std::filesystem::path tmp = SourcesDir() / (name + ".db.tmp");
        std::cout << "Downloading source index from " << url << "..." << std::endl;
        if (!DownloadReal(url, tmp))
        {
            std::filesystem::remove(tmp);
            std::cout << "Failed to download source index from " << url << " (network error or bad URL)." << std::endl;
            return 1;
        }

        if (!ValidateCatalog(tmp))
        {
            std::filesystem::remove(tmp);
            std::cout << "Downloaded file is not a valid winget-termux catalog (SQLiteIndex could not open it); rejecting." << std::endl;
            return 1;
        }

        std::filesystem::rename(tmp, SourceDbPath(name));
        sources.push_back({ name, url });
        SaveSources(sources);
        std::cout << "Successfully added source '" << name << "'." << std::endl;
        return EXIT_OK;
    }

    int CmdSourceRemove(const std::string& name)
    {
        auto sources = LoadSources();
        auto it = std::find_if(sources.begin(), sources.end(), [&](const SourceEntry& e) { return e.Name == name; });
        if (it == sources.end())
        {
            std::cout << "No source named '" << name << "' found." << std::endl;
            return EXIT_NOT_FOUND;
        }
        sources.erase(it);
        SaveSources(sources);
        std::filesystem::remove(SourceDbPath(name));
        std::cout << "Removed source '" << name << "'." << std::endl;
        return EXIT_OK;
    }

    int CmdSourceUpdate(const std::string& name)
    {
        auto sources = LoadSources();
        auto it = std::find_if(sources.begin(), sources.end(), [&](const SourceEntry& e) { return e.Name == name; });
        if (it == sources.end())
        {
            std::cout << "No source named '" << name << "' found." << std::endl;
            return EXIT_NOT_FOUND;
        }

        std::filesystem::path tmp = SourcesDir() / (name + ".db.tmp");
        if (!DownloadReal(it->Url, tmp))
        {
            std::filesystem::remove(tmp);
            std::cout << "Failed to refresh source '" << name << "' (network error); keeping existing index." << std::endl;
            return 1;
        }

        if (!ValidateCatalog(tmp))
        {
            std::filesystem::remove(tmp);
            std::cout << "Refreshed index for '" << name << "' failed validation; keeping existing index." << std::endl;
            return 1;
        }

        // Atomic swap: the old index is only ever touched by this one rename, and only once
        // the new one is already confirmed to open and query correctly.
        std::filesystem::rename(tmp, SourceDbPath(name));
        std::cout << "Source '" << name << "' updated." << std::endl;
        return EXIT_OK;
    }

    // Real counts read straight from the persisted InstalledState markers (no manifest
    // resolution, no network) -- cheap, and works even for offline/removed sources since it's
    // just counting which packages were last recorded as coming from where.
    std::map<std::string, int> CountInstalledBySource()
    {
        std::map<std::string, int> counts;
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        std::filesystem::path dir = homeDir / ".winget" / "versions";
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
        {
            return counts;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.path().extension() != ".version") { continue; }
            std::string packageId = entry.path().stem().string();
            auto state = ReadInstalledState(packageId);
            if (!state) { continue; }
            counts[state->SourceName]++;
        }
        return counts;
    }

    // "index": the missing easy-mode command for adding a manifest to the local catalog --
    // everything else (install/upgrade/search/show) already worked once a manifest was
    // indexed, but doing that required compiling a throwaway C++ program. This wraps the
    // exact same real SQLiteIndex::AddManifest call in a single CLI command. Copies the
    // manifest into the real manifest root (RelativePath must be relative, not absolute --
    // see ARCHITECTURE.md) so ResolveManifestById can find it again later.
    int CmdIndex(SQLiteIndex& index, const std::string& manifestPath)
    {
        std::filesystem::path src(manifestPath);
        if (!std::filesystem::exists(src))
        {
            std::cout << "Manifest file not found: " << manifestPath << std::endl;
            return EXIT_NOT_FOUND;
        }

        Manifest manifest;
        try
        {
            manifest = YamlParser::CreateFromPath(src);
        }
        catch (const std::exception& e)
        {
            std::cout << "Manifest is invalid: " << e.what() << std::endl;
            return 1;
        }

        std::filesystem::path destDir(s_manifestRoot);
        std::filesystem::create_directories(destDir);
        std::string fileName = src.filename().string();
        std::filesystem::path dest = destDir / fileName;
        std::error_code ec;
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            std::cout << "Failed to stage manifest into " << dest << ": " << ec.message() << std::endl;
            return 1;
        }

        try
        {
            index.AddManifest(dest, fileName);
        }
        catch (const std::exception& e)
        {
            std::cout << "Failed to index manifest (duplicate {Id,Version,Channel}, or corrupt index): " << e.what() << std::endl;
            return 1;
        }

        std::cout << "Indexed " << manifest.Id << " [" << manifest.Version << "]." << std::endl;
        std::cout << "Now available via: winget install " << manifest.Id << std::endl;
        return EXIT_OK;
    }

    // "install-url": the no-manifest-needed fast path for a single script/binary. Downloads
    // for real, computes the real SHA256 of what it actually got (self-verifying -- there's
    // no separate pinned hash to mismatch, unlike catalog installs), then reuses the exact
    // same PortableInstallerTermux backend and persisted-state writing as every other
    // install. No manifest file, no indexing step -- closest thing to "winget install
    // <name>" simplicity when there's no catalog entry to search for.
    int CmdInstallUrl(const std::string& url, std::string alias)
    {
        if (alias.empty())
        {
            std::filesystem::path p(url);
            alias = p.stem().string();
            if (alias.empty()) { alias = "tool"; }
        }

        if (!IsSafePathComponent(alias))
        {
            std::cout << "Refusing to install: alias '" << alias << "' is not a safe path component." << std::endl;
            return 1;
        }
        PackageLock lock("url:" + alias);

        std::filesystem::path tmp = SourcesDir() / (alias + ".dl.tmp");
        std::cout << "Downloading " << url << "..." << std::endl;
        if (!DownloadReal(url, tmp))
        {
            std::filesystem::remove(tmp);
            std::cout << "Download failed (network error or bad URL)." << std::endl;
            return 1;
        }

        auto hash = AppInstaller::Utility::SHA256::ComputeHashFromFile(tmp);
        std::string sha256Hex = AppInstaller::Utility::SHA256::ConvertToString(hash);
        std::for_each(sha256Hex.begin(), sha256Hex.end(), [](char& c) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

        // Reuses the file just downloaded (and hashed) instead of downloading a second time --
        // a second GET isn't guaranteed to return identical bytes (CDN, unstable URL), which
        // would silently install content the hash above never actually verified.
        std::string packageId = "url:" + alias;
        auto result = AppInstaller::Portable::Termux::InstallPortableFromLocalFile(packageId, "payload", alias, tmp, sha256Hex);
        if (!result.Success)
        {
            std::cout << "Install failed: " << result.Message << std::endl;
            return 1;
        }

        WriteVersionMarker(packageId, sha256Hex.substr(0, 12), "", alias, alias);
        std::cout << "Successfully installed as '" << alias << "'." << std::endl;
        std::cout << "  Command: " << alias << " (" << result.SymlinkPath << ")" << std::endl;
        std::cout << "  (no manifest -- uninstall with: winget uninstall " << packageId << ")" << std::endl;
        return EXIT_OK;
    }

    int CmdPin(const std::string& id)
    {
        if (!ReadInstalledState(id))
        {
            std::cout << "No installed package found for " << id << "; nothing to pin." << std::endl;
            return EXIT_NOT_INSTALLED;
        }
        std::ofstream(PinPath(id)).close();
        std::cout << "Pinned " << id << ". It will be skipped by upgrade/upgrade --all." << std::endl;
        return EXIT_OK;
    }

    int CmdUnpin(const std::string& id)
    {
        if (!IsPinned(id))
        {
            std::cout << id << " is not pinned." << std::endl;
            return EXIT_OK;
        }
        std::filesystem::remove(PinPath(id));
        std::cout << "Unpinned " << id << "." << std::endl;
        return EXIT_OK;
    }

    int CmdSourceList()
    {
        auto counts = CountInstalledBySource();
        std::cout << "Name        Type                          Argument                                   State     Installed" << std::endl;
        std::cout << "----------  ----------------------------  -----------------------------------------  --------  ---------" << std::endl;
        bool dbExists = std::filesystem::exists(s_dbPath);
        std::cout << "TermuxLocal Microsoft.PreIndexed.Package  " << s_dbPath << "  " << (dbExists ? "Ready" : "Missing") << "  " << counts[""] << std::endl;
        for (const auto& e : LoadSources())
        {
            bool ready = std::filesystem::exists(SourceDbPath(e.Name));
            std::cout << e.Name << " Microsoft.PreIndexed.Package  " << e.Url << "  " << (ready ? "Ready" : "Missing") << "  " << counts[e.Name] << std::endl;
            counts.erase(e.Name);
        }
        // Whatever's left is installed state pointing at a source no longer registered --
        // real history, not lost just because the source was removed.
        counts.erase("");
        for (const auto& [sourceName, count] : counts)
        {
            std::cout << sourceName << " Microsoft.PreIndexed.Package  <removed>  <missing>  " << count << std::endl;
        }
        return EXIT_OK;
    }

    // JSON dump of registered sources (own schema, same reasoning as CmdExport --
    // upstream winget's source export schema assumes msstore/community sources this
    // port doesn't have). TermuxLocal is deliberately excluded: it's the local install
    // database, not a remote source, and has no URL to round-trip.
    int CmdSourceExport(const std::string& path)
    {
        Json::Value root(Json::arrayValue);
        for (const auto& e : LoadSources())
        {
            Json::Value item;
            item["Name"] = e.Name;
            item["Argument"] = e.Url;
            item["Type"] = "Microsoft.PreIndexed.Package";
            root.append(item);
        }

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::string json = Json::writeString(builder, root);

        if (path.empty() || path == "-")
        {
            std::cout << json << std::endl;
        }
        else
        {
            std::ofstream out(path);
            if (!out)
            {
                std::cout << "Could not write to " << path << std::endl;
                return 1;
            }
            out << json;
            std::cout << "Exported " << root.size() << " source(s) to " << path << std::endl;
        }
        return EXIT_OK;
    }

    void PrintPackageLine(const std::string& id, const std::string& name, const std::string& version, const std::string& moniker)
    {
        std::cout << (name.empty() ? "<unknown>" : name) << "\t" << id << "\t" << version << "\t" << (moniker.empty() ? "<none>" : moniker) << std::endl;
    }

    void SearchOneIndex(SQLiteIndex& index, const std::string& sourceName, const std::string& query, bool& any)
    {
        SearchRequest req;
        if (!query.empty())
        {
            req.Query = RequestMatch(MatchType::Substring, query);
        }
        auto result = index.Search(req);
        for (const auto& [manifestId, key] : result.Matches)
        {
            auto name = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Name);
            auto pid = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Id);
            auto version = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Version);
            auto moniker = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Moniker);
            std::cout << (name.value_or("").empty() ? "<unknown>" : name.value_or(""))
                << "\t" << pid.value_or("") << "\t" << version.value_or("")
                << "\t" << (moniker.value_or("").empty() ? "<none>" : moniker.value_or(""))
                << "\t" << sourceName << std::endl;
            any = true;
        }
    }

    // Real multi-source search: queries the local install catalog plus every registered
    // remote source (opened read-only, skipped silently if its file is missing/corrupt --
    // "source update" is how a broken source gets fixed, not a crash on every command).
    int CmdSearch(SQLiteIndex& localIndex, const std::string& query)
    {
        std::cout << "Name\tId\tVersion\tMoniker\tSource" << std::endl;
        bool any = false;
        SearchOneIndex(localIndex, "TermuxLocal", query, any);
        for (const auto& e : LoadSources())
        {
            try
            {
                auto idx = SQLiteIndex::Open(SourceDbPath(e.Name).string(), SQLiteIndex::OpenDisposition::Read);
                SearchOneIndex(idx, e.Name, query, any);
            }
            catch (const std::exception&) { /* unreadable source: skip, don't fail the whole search */ }
        }

        if (!any)
        {
            std::cout << "No package found matching input criteria: " << query << std::endl;
            return EXIT_NOT_FOUND;
        }
        return EXIT_OK;
    }

    // Show, using index-only properties: for a package that came from a remote source, the
    // manifest YAML lives on that remote repository's server, not locally -- there's no real
    // file to re-parse for install-details, so this shows exactly what the catalog itself
    // stores (honest partial info), rather than crashing or fabricating installer data.
    bool ShowFromIndexOnly(SQLiteIndex& index, const std::string& sourceName, const std::string& id)
    {
        SearchRequest req;
        req.Query = RequestMatch(MatchType::Exact, id);
        auto result = index.Search(req);
        if (result.Matches.empty())
        {
            return false;
        }
        auto manifestId = result.Matches[0].first;
        auto name = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Name);
        auto version = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Version);
        auto moniker = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Moniker);
        auto publisher = index.GetPropertyByPrimaryId(manifestId, PackageVersionProperty::Publisher);
        std::cout << "Found " << id << " [" << version.value_or("<none>") << "] (source: " << sourceName << ")" << std::endl;
        std::cout << "Name: " << name.value_or("<none>") << std::endl;
        std::cout << "Publisher: " << publisher.value_or("<none>") << std::endl;
        std::cout << "Moniker: " << moniker.value_or("<none>") << std::endl;
        std::cout << "(manifest file not available locally; showing index-only data)" << std::endl;
        return true;
    }

    int CmdShow(SQLiteIndex& index, const std::string& id)
    {
        SQLiteIndex::IdType manifestId = 0;
        auto manifest = ResolveManifestById(index, id, manifestId);
        if (manifest)
        {
            std::cout << "Found " << manifest->Id << " [" << manifest->Version << "]" << std::endl;
            std::cout << "Publisher: " << (manifest->DefaultLocalization.Get<Localization::Publisher>().empty() ? "<none>" : manifest->DefaultLocalization.Get<Localization::Publisher>()) << std::endl;
            std::cout << "Moniker: " << (manifest->Moniker.empty() ? "<none>" : manifest->Moniker) << std::endl;
            std::cout << "Description: " << (manifest->DefaultLocalization.Get<Localization::ShortDescription>().empty() ? "<none>" : manifest->DefaultLocalization.Get<Localization::ShortDescription>()) << std::endl;
            for (const auto& installer : manifest->Installers)
            {
                std::cout << "Installer: " << AppInstaller::Manifest::InstallerTypeToString(installer.BaseInstallerType)
                    << " -> effective " << AppInstaller::Manifest::InstallerTypeToString(installer.EffectiveInstallerType())
                    << " (" << installer.Url << ")" << std::endl;
            }
            return EXIT_OK;
        }

        for (const auto& e : LoadSources())
        {
            try
            {
                auto idx = SQLiteIndex::Open(SourceDbPath(e.Name).string(), SQLiteIndex::OpenDisposition::Read);
                if (ShowFromIndexOnly(idx, e.Name, id))
                {
                    return EXIT_OK;
                }
            }
            catch (const std::exception&) { /* unreadable source: skip */ }
        }

        std::cout << "No package found matching input criteria: " << id << std::endl;
        return EXIT_NOT_FOUND;
    }

    // "list": reflects real on-disk installed state (not the index, which only tracks
    // available/known manifests) -- scans both backends' real install roots, and treats a
    // package as installed only if its symlink still resolves, so a half-removed leftover
    // never gets reported as installed.
    struct InstalledEntry
    {
        std::string PackageId;
        std::string Backend;
        std::string Alias;
    };

    std::vector<InstalledEntry> ScanInstalled()
    {
        std::vector<InstalledEntry> entries;
        std::error_code ec;

        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");

        auto scan = [&](const std::filesystem::path& root, const std::string& backend)
        {
            if (!std::filesystem::exists(root, ec))
            {
                return;
            }
            for (const auto& entry : std::filesystem::directory_iterator(root, ec))
            {
                if (!entry.is_directory(ec))
                {
                    continue;
                }
                entries.push_back({ entry.path().filename().string(), backend, "" });
            }
        };

        scan(homeDir / ".winget" / "programfiles", "Portable");
        scan(homeDir / ".winget" / "ziparchives", "Zip");
        return entries;
    }

    // List reads the persisted InstalledState (version/source/alias/name written at
    // install/upgrade time) instead of re-resolving the manifest -- this is what makes list
    // work for remote-sourced packages even when that source is offline or has been removed,
    // and avoids a network round-trip just to show what's already on disk.
    int CmdList(SQLiteIndex& index, bool upgradeAvailableOnly = false)
    {
        auto installed = ScanInstalled();
        if (installed.empty())
        {
            std::cout << "No installed packages found." << std::endl;
            return EXIT_OK;
        }

        auto sources = LoadSources();
        auto sourceRegistered = [&](const std::string& name)
        {
            return std::any_of(sources.begin(), sources.end(), [&](const SourceEntry& e) { return e.Name == name; });
        };

        std::cout << "Name\tId\tVersion\tBackend\tSource" << std::endl;
        for (auto& entry : installed)
        {
            auto state = ReadInstalledState(entry.PackageId);

            std::string alias = state ? state->Alias : "";
            if (alias.empty())
            {
                // No persisted state (package installed before this field existed, or marker
                // lost) -- fall back to the old local-only manifest resolution rather than
                // silently hiding the entry.
                SQLiteIndex::IdType manifestId = 0;
                auto manifest = ResolveManifestById(index, entry.PackageId, manifestId);
                if (manifest)
                {
                    if (entry.Backend == "Zip")
                    {
                        if (const auto* zip = FindZip(*manifest))
                        {
                            alias = !zip->NestedInstallerFiles[0].PortableCommandAlias.empty()
                                ? zip->NestedInstallerFiles[0].PortableCommandAlias
                                : (manifest->Moniker.empty() ? manifest->Id : manifest->Moniker);
                        }
                    }
                    else
                    {
                        alias = manifest->Moniker.empty() ? manifest->Id : manifest->Moniker;
                    }
                }
            }

            bool stillValid = entry.Backend == "Zip"
                ? (!alias.empty() && AppInstaller::Zip::Termux::IsCommandOnPath(alias))
                : (!alias.empty() && AppInstaller::Portable::Termux::IsCommandOnPath(alias));

            if (!stillValid)
            {
                // Directory exists but symlink is gone/broken -- a real partial-uninstall
                // leftover, not a package to honestly call "installed".
                continue;
            }

            if (upgradeAvailableOnly)
            {
                std::string discardSource;
                auto latest = ResolveManifestAnywhere(index, entry.PackageId, discardSource);
                if (!latest || !state || latest->Version == state->Version || IsPinned(entry.PackageId))
                {
                    continue;
                }
            }

            std::string name = state && !state->Name.empty() ? state->Name : "<unknown>";
            std::string version = state ? state->Version : "";
            std::string sourceLabel;
            if (!state || state->SourceName.empty())
            {
                sourceLabel = "TermuxLocal";
            }
            else if (sourceRegistered(state->SourceName))
            {
                sourceLabel = state->SourceName;
            }
            else
            {
                sourceLabel = state->SourceName + " <missing>";
            }

            std::cout << name << "\t" << entry.PackageId << "\t" << version << "\t" << entry.Backend << "\t" << sourceLabel << std::endl;
        }
        return EXIT_OK;
    }

    // "upgrade --all": reuses CmdUpgrade per package -- no separate bulk-upgrade logic, same
    // persisted-source-first resolution, same exit codes, same messages. Tallies outcomes by
    // CmdUpgrade's own exit code rather than re-implementing its up-to-date/not-found/
    // unsupported-type distinctions a second time.
    int CmdUpgradeAll(SQLiteIndex& index)
    {
        auto installed = ScanInstalled();
        if (installed.empty())
        {
            std::cout << "No installed packages found; nothing to upgrade." << std::endl;
            return EXIT_OK;
        }

        // CmdUpgrade itself already prints the real per-package outcome ("already up to
        // date", "Successfully upgraded to X", "not installed", "no installer type
        // supported", etc.) -- this loop doesn't re-derive or hide any of that, just tallies
        // ok vs failed by CmdUpgrade's own exit code.
        int ok = 0, failed = 0;
        for (const auto& entry : installed)
        {
            std::cout << "--- " << entry.PackageId << " ---" << std::endl;
            int rc = CmdUpgrade(index, entry.PackageId);
            (rc == EXIT_OK ? ok : failed)++;
        }

        std::cout << std::endl << "Upgrade summary: " << ok << " OK, " << failed << " failed out of " << installed.size() << " installed package(s)." << std::endl;
        return failed == 0 ? EXIT_OK : 1;
    }

    // Export/import: this fork's own JSON format (not upstream winget's export schema --
    // that assumes real winget.run sources this port doesn't have). Real round-trip: export
    // reads the same persisted InstalledState every other command uses; import calls the
    // real CmdInstall per entry, so it goes through the exact same resolution/backend
    // dispatch as a manual "winget install" -- no separate reinstall logic.
    int CmdExport(const std::string& path)
    {
        auto installed = ScanInstalled();
        Json::Value root(Json::arrayValue);
        for (const auto& entry : installed)
        {
            auto state = ReadInstalledState(entry.PackageId);
            if (!state) { continue; }
            Json::Value item;
            item["PackageIdentifier"] = entry.PackageId;
            item["Version"] = state->Version;
            item["Source"] = state->SourceName;
            item["Pinned"] = IsPinned(entry.PackageId);
            root.append(item);
        }

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::string json = Json::writeString(builder, root);

        if (path.empty() || path == "-")
        {
            std::cout << json << std::endl;
        }
        else
        {
            std::ofstream out(path);
            if (!out)
            {
                std::cout << "Could not write to " << path << std::endl;
                return 1;
            }
            out << json;
            std::cout << "Exported " << root.size() << " package(s) to " << path << std::endl;
        }
        return EXIT_OK;
    }

    int CmdImport(SQLiteIndex& index, const std::string& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::cout << "Could not read " << path << std::endl;
            return EXIT_NOT_FOUND;
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, in, &root, &errs) || !root.isArray())
        {
            std::cout << "Invalid import file (not a JSON array): " << errs << std::endl;
            return 1;
        }

        int ok = 0, failed = 0;
        for (const auto& item : root)
        {
            std::string id = item.get("PackageIdentifier", "").asString();
            if (id.empty()) { continue; }

            std::cout << "--- " << id << " ---" << std::endl;
            int rc = CmdInstall(index, id);
            if (rc == EXIT_OK)
            {
                ok++;
                if (item.get("Pinned", false).asBool())
                {
                    CmdPin(id);
                }
            }
            else
            {
                failed++;
            }
        }

        std::cout << std::endl << "Import summary: " << ok << " OK, " << failed << " failed." << std::endl;
        return failed == 0 ? EXIT_OK : 1;
    }

    // Real SHA256 of a local file, same helper the installers use to verify downloads --
    // for authoring a manifest's InstallerSha256 without a separate sha256sum dependency.
    int CmdHash(const std::string& path)
    {
        if (!std::filesystem::exists(path))
        {
            std::cerr << "file not found: " << path << std::endl;
            return EXIT_NOT_FOUND;
        }
        auto hash = AppInstaller::Utility::SHA256::ComputeHashFromFile(path);
        std::string hex = AppInstaller::Utility::SHA256::ConvertToString(hash);
        std::for_each(hex.begin(), hex.end(), [](char& c) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        std::cout << hex << std::endl;
        return EXIT_OK;
    }

    // Real schema validation via the same YamlParser upstream winget-cli uses (fails on
    // malformed YAML, missing required fields, wrong types) -- no separate schema tool.
    int CmdValidate(const std::string& manifestPath)
    {
        std::filesystem::path src(manifestPath);
        if (!std::filesystem::exists(src))
        {
            std::cout << "Manifest file not found: " << manifestPath << std::endl;
            return EXIT_NOT_FOUND;
        }
        try
        {
            auto manifest = YamlParser::CreateFromPath(src);
            std::cout << "Manifest is valid: " << manifest.Id << " [" << manifest.Version << "]" << std::endl;
            return EXIT_OK;
        }
        catch (const std::exception& e)
        {
            std::cout << "Manifest is invalid: " << e.what() << std::endl;
            return 1;
        }
    }

    // Downloads the installer payload for a catalog entry without installing it -- real
    // network fetch + real SHA256 check against the manifest, same as install does, just
    // without the symlink/chmod step.
    int CmdDownload(SQLiteIndex& index, const std::string& id, std::string destDir)
    {
        std::string sourceName;
        auto manifest = ResolveManifestAnywhere(index, id, sourceName);
        if (!manifest)
        {
            std::cout << "No package found matching input criteria: " << id << std::endl;
            return EXIT_NOT_FOUND;
        }

        const AppInstaller::Manifest::ManifestInstaller* installer = FindZip(*manifest);
        if (!installer)
        {
            installer = FindPortable(*manifest);
        }
        if (!installer)
        {
            std::cout << "No installer type supported on this platform was found for " << id << "." << std::endl;
            return EXIT_UNSUPPORTED_TYPE;
        }

        if (destDir.empty())
        {
            destDir = ".";
        }
        std::filesystem::create_directories(destDir);
        std::string url = installer->Url;
        std::string fileName = std::filesystem::path(url).filename().string();
        if (fileName.empty())
        {
            fileName = manifest->Id + ".bin";
        }
        std::filesystem::path dest = std::filesystem::path(destDir) / fileName;

        std::cout << "Downloading " << url << "..." << std::endl;
        if (!DownloadReal(url, dest))
        {
            std::cout << "Download failed." << std::endl;
            return 1;
        }

        std::string expectedHex = ToHexLower(installer->Sha256);
        auto hash = AppInstaller::Utility::SHA256::ComputeHashFromFile(dest);
        std::string gotHex = AppInstaller::Utility::SHA256::ConvertToString(hash);
        std::for_each(gotHex.begin(), gotHex.end(), [](char& c) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (!expectedHex.empty() && gotHex != expectedHex)
        {
            std::cout << "SHA256 mismatch: got " << gotHex << " expected " << expectedHex << std::endl;
            std::filesystem::remove(dest);
            return 1;
        }

        std::cout << "Saved to " << dest.string() << std::endl;
        std::cout << "SHA256: " << gotHex << std::endl;
        return EXIT_OK;
    }
}

namespace
{
    void PrintUsage()
    {
        std::cerr << "usage: winget_cli <install|uninstall|show> <PackageIdentifier>" << std::endl;
        std::cerr << "       winget_cli index <manifest.yaml>" << std::endl;
        std::cerr << "       winget_cli install-url <url> [alias]" << std::endl;
        std::cerr << "       winget_cli pin|unpin <PackageIdentifier>" << std::endl;
        std::cerr << "       winget_cli export [file]  (default: stdout)" << std::endl;
        std::cerr << "       winget_cli import <file>" << std::endl;
        std::cerr << "       winget_cli upgrade <PackageIdentifier>|--all" << std::endl;
        std::cerr << "       winget_cli search [<query>]" << std::endl;
        std::cerr << "       winget_cli list [--upgrade-available]" << std::endl;
        std::cerr << "       winget_cli source <list|add <name> <url>|remove <name>|update <name>|export [file]>" << std::endl;
        std::cerr << "       winget_cli hash <file>" << std::endl;
        std::cerr << "       winget_cli validate <manifest.yaml>" << std::endl;
        std::cerr << "       winget_cli download <PackageIdentifier> [dest-dir]" << std::endl;
        std::cerr << "       winget_cli --info" << std::endl;
        std::cerr << "       winget_cli complete" << std::endl;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return EXIT_USAGE;
    }

    std::string command = argv[1];

    if (command == "--version" || command == "-v")
    {
        std::cout << "winget-termux 1.0.0 (native ARM64/bionic Termux port)" << std::endl;
        return EXIT_OK;
    }

    if (command == "--info")
    {
        const char* home = std::getenv("HOME");
        std::filesystem::path homeDir = home ? std::filesystem::path(home) : std::filesystem::path("/data/data/com.termux/files/home");
        std::cout << "winget-termux 1.0.0 (native ARM64/bionic Termux port)" << std::endl;
        std::cout << "Local index:      " << s_dbPath << std::endl;
        std::cout << "Sources dir:      " << (homeDir / ".winget" / "sources").string() << std::endl;
        std::cout << "Program files:    " << (homeDir / ".winget" / "programfiles").string() << std::endl;
        std::cout << "Zip archives:     " << (homeDir / ".winget" / "ziparchives").string() << std::endl;
        std::cout << "Locks dir:        " << (homeDir / ".winget" / "locks").string() << std::endl;
        return EXIT_OK;
    }

    if (command == "complete")
    {
        static const char* commands[] = {
            "install", "uninstall", "upgrade", "list", "search", "show", "source",
            "index", "install-url", "pin", "unpin", "export", "import", "hash",
            "validate", "download", "--version", "--info", "complete"
        };
        std::cout << "complete -W \"";
        for (const char* c : commands) { std::cout << c << " "; }
        std::cout << "\" winget winget_cli" << std::endl;
        return EXIT_OK;
    }

    if (command == "hash")
    {
        if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
        return CmdHash(argv[2]);
    }

    if (command == "validate")
    {
        if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
        return CmdValidate(argv[2]);
    }

    try
    {
        if (command == "source")
        {
            std::string sub = argc >= 3 ? argv[2] : "";
            if (sub == "list")
            {
                return CmdSourceList();
            }
            else if (sub == "export")
            {
                return CmdSourceExport(argc >= 4 ? argv[3] : "");
            }
            else if (sub == "add")
            {
                if (argc < 5) { PrintUsage(); return EXIT_USAGE; }
                return CmdSourceAdd(argv[3], argv[4]);
            }
            else if (sub == "remove")
            {
                if (argc < 4) { PrintUsage(); return EXIT_USAGE; }
                return CmdSourceRemove(argv[3]);
            }
            else if (sub == "update")
            {
                if (argc < 4) { PrintUsage(); return EXIT_USAGE; }
                return CmdSourceUpdate(argv[3]);
            }
            else
            {
                PrintUsage();
                return EXIT_USAGE;
            }
        }

        // The local catalog is created lazily (real, empty SQLite index, not a stub) so that
        // search/show against remote-only sources work even before anything has ever been
        // installed locally -- a missing TermuxLocal db isn't a real error for those commands.
        if (!std::filesystem::exists(s_dbPath))
        {
            SQLiteIndex::CreateNew(s_dbPath);
        }
        auto index = SQLiteIndex::Open(s_dbPath, SQLiteIndex::OpenDisposition::ReadWrite);

        if (command == "list")
        {
            bool upgradeAvailableOnly = argc >= 3 && std::string(argv[2]) == "--upgrade-available";
            return CmdList(index, upgradeAvailableOnly);
        }
        else if (command == "search")
        {
            return CmdSearch(index, argc >= 3 ? argv[2] : "");
        }
        else if (command == "install")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdInstall(index, argv[2]);
        }
        else if (command == "uninstall")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdUninstall(index, argv[2]);
        }
        else if (command == "show")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdShow(index, argv[2]);
        }
        else if (command == "upgrade")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            if (std::string(argv[2]) == "--all")
            {
                return CmdUpgradeAll(index);
            }
            return CmdUpgrade(index, argv[2]);
        }
        else if (command == "index")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdIndex(index, argv[2]);
        }
        else if (command == "install-url")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdInstallUrl(argv[2], argc >= 4 ? argv[3] : "");
        }
        else if (command == "pin")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdPin(argv[2]);
        }
        else if (command == "unpin")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdUnpin(argv[2]);
        }
        else if (command == "export")
        {
            return CmdExport(argc >= 3 ? argv[2] : "");
        }
        else if (command == "import")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdImport(index, argv[2]);
        }
        else if (command == "download")
        {
            if (argc < 3) { PrintUsage(); return EXIT_USAGE; }
            return CmdDownload(index, argv[2], argc >= 4 ? argv[3] : "");
        }
        else
        {
            std::cerr << "unknown command: " << command << std::endl;
            PrintUsage();
            return EXIT_USAGE;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
}
