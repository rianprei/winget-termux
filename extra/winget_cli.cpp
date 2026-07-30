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
#include <curl/curl.h>

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

    int CmdInstall(SQLiteIndex& index, const std::string& id)
    {
        std::string sourceName;
        auto manifest = ResolveManifestAnywhere(index, id, sourceName);
        if (!manifest)
        {
            std::cout << "No package found matching input criteria: " << id << std::endl;
            return EXIT_NOT_FOUND;
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

            std::cout << "This is a Zip package; extracting via native Termux backend (real unzip)..." << std::endl;
            auto result = AppInstaller::Zip::Termux::InstallZip(manifest->Id, zip->Url, sha256Hex, nested.RelativeFilePath, alias);
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
    int CmdList(SQLiteIndex& index)
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
}

namespace
{
    void PrintUsage()
    {
        std::cerr << "usage: winget_cli <install|uninstall|show> <PackageIdentifier>" << std::endl;
        std::cerr << "       winget_cli upgrade <PackageIdentifier>|--all" << std::endl;
        std::cerr << "       winget_cli search [<query>]" << std::endl;
        std::cerr << "       winget_cli list" << std::endl;
        std::cerr << "       winget_cli source <list|add <name> <url>|remove <name>|update <name>>" << std::endl;
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

    try
    {
        if (command == "source")
        {
            std::string sub = argc >= 3 ? argv[2] : "";
            if (sub == "list")
            {
                return CmdSourceList();
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
            return CmdList(index);
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
