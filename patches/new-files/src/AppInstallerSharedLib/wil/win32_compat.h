#pragma once
#include <cstdint>
#include <unistd.h>

typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef uint8_t BYTE;
typedef int BOOL;
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef unsigned short WORD;
typedef unsigned long ULONG;
typedef long LONG;
typedef void* LPVOID;
typedef const char* LPCSTR;
typedef const wchar_t* PCWSTR;
typedef wchar_t* PWSTR;
typedef unsigned long LCID;
struct FILETIME { uint32_t dwLowDateTime; uint32_t dwHighDateTime; };
typedef void* HANDLE;
typedef void* HWND;

// Windows Registry value-type constants: real registry access is a genuine Windows-only
// concept (uninstall-key enumeration, group policy). Stubbed as values only so headers parse.
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_DWORD_LITTLE_ENDIAN 4
#define REG_DWORD_BIG_ENDIAN 5
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define REG_QWORD_LITTLE_ENDIAN 11
#define REG_OPTION_NON_VOLATILE 0
#define KEY_READ 0x20019
#define KEY_ALL_ACCESS 0xF003F
#define KEY_WOW64_64KEY 0x0100
inline void* const HKEY_LOCAL_MACHINE = reinterpret_cast<void*>(1);

#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_DIRECTORY 267L
#define ERROR_DATA_CHECKSUM_ERROR 323L
#define MB_ERR_INVALID_CHARS 8L
#define ERROR_NO_UNICODE_TRANSLATION 1113L
#define ERROR_DIRECTORY_NOT_SUPPORTED 336L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_INVALID_STATE 5023L
#define ERROR_NOT_FOUND 1168L
#define MAX_PATH 260
// MSVC __declspec(selectany) lets a variable be defined identically in multiple TUs and merged
// by the linker; C++17 `inline` variables give the same one-definition-across-TUs guarantee.
#define __declspec_selectany_ inline
#define SPAPI_E_FILE_HASH_NOT_IN_CATALOG ((HRESULT)0x800F0008L)
inline unsigned long GetCurrentThreadId() { return static_cast<unsigned long>(gettid()); }

typedef void* HKEY;
typedef unsigned long REGSAM;

// MSI/Windows Installer exit codes: only meaningful when a real Windows MSI installer runs.
// On Android that path never executes; stubbed with real winerror.h values for completeness.
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_INSTALL_ALREADY_RUNNING 1618L
#define ERROR_DISK_FULL 112L
#define ERROR_INSTALL_SERVICE_FAILURE 1601L
#define ERROR_SUCCESS_REBOOT_REQUIRED 3010L
#define ERROR_SUCCESS_REBOOT_INITIATED 1641L
#define ERROR_INSTALL_USEREXIT 1602L
#define ERROR_PRODUCT_VERSION 1638L
#define ERROR_INSTALL_REJECTED 1654L
#define ERROR_INSTALL_PACKAGE_REJECTED 1625L
#define ERROR_INSTALL_TRANSFORM_REJECTED 1624L
#define ERROR_PATCH_PACKAGE_REJECTED 1642L
#define ERROR_PATCH_REMOVAL_DISALLOWED 1646L
#define ERROR_INSTALL_REMOTE_DISALLOWED 1640L
#define ERROR_INVALID_TABLE 1628L
#define ERROR_INVALID_COMMAND_LINE 1639L
#define ERROR_INVALID_PATCH_XML 1635L
#define ERROR_INSTALL_LANGUAGE_UNSUPPORTED 1623L
#define ERROR_INSTALL_PLATFORM_UNSUPPORTED 1633L
#define ERROR_INSTALL_PREREQUISITE_FAILED 1621L
#define ERROR_INSTALL_RESOLVE_DEPENDENCY_FAILED 1635L
#define ERROR_INSTALL_OPTIONAL_PACKAGE_REQUIRES_MAIN_PACKAGE 1636L
#define ERROR_INSTALL_OUT_OF_DISK_SPACE 1653L
#define ERROR_INSTALL_CANCEL 1602L
#define ERROR_PACKAGE_ALREADY_EXISTS 1639L
#define ERROR_INSTALL_PACKAGE_DOWNGRADE 1642L
#define ERROR_DEPLOYMENT_BLOCKED_BY_POLICY 1654L
#define ERROR_INSTALL_POLICY_FAILURE 1652L
#define ERROR_PACKAGES_IN_USE 1653L
#define ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE 1656L
#define ERROR_PACKAGE_NOT_SUPPORTED_ON_FILESYSTEM 1657L
#define ERROR_DEPLOYMENT_OPTION_NOT_SUPPORTED 1658L
#define ERROR_PACKAGE_LACKS_CAPABILITY_TO_DEPLOY_ON_HOST 1659L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ARRAYSIZE(a) (sizeof(a) / sizeof(a[0]))
#define sscanf_s sscanf

typedef unsigned long ULONG_PTR;

struct GUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
    friend bool operator==(const GUID& a, const GUID& b)
    {
        return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
            std::equal(std::begin(a.Data4), std::end(a.Data4), std::begin(b.Data4));
    }
};

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <algorithm>

inline unsigned long GetLastError() { return 0; }

inline HRESULT CoCreateGuid(GUID* g)
{
    static std::mt19937_64 rng{ std::random_device{}() };
    auto* bytes = reinterpret_cast<uint8_t*>(g);
    for (size_t i = 0; i < sizeof(GUID); ++i) { bytes[i] = static_cast<uint8_t>(rng()); }
    return 0;
}

inline DWORD ExpandEnvironmentStringsW(const wchar_t* input, wchar_t* out, DWORD outSize)
{
    // ponytail: real %VAR% expansion is Win32-only syntax; on Android there is nothing
    // meaningful to expand against, so this is a pass-through stub, not a real implementation.
    std::wstring s(input);
    if (out && outSize > 0)
    {
        size_t n = std::min<size_t>(s.size(), static_cast<size_t>(outSize) - 1);
        for (size_t i = 0; i < n; ++i) out[i] = s[i];
        out[n] = 0;
    }
    return static_cast<DWORD>(s.size() + 1);
}

#include <fcntl.h>

#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))

// ponytail: real rewrite. Windows CreateFileW returns an opaque kernel HANDLE; here HANDLE is
// literally a POSIX fd stashed in a void* (see intptr_t cast), so ReadFile/WriteFile/CloseHandle
// in this header operate on the same real fd, not a dead stub, when this constructor is used.
inline HANDLE CreateFileW(const char* path, DWORD /*desiredAccess*/, DWORD /*shareMode*/, void*,
    DWORD creationDisposition, DWORD /*flagsAndAttributes*/, void*)
{
    int flags = O_RDWR;
    if (creationDisposition == CREATE_ALWAYS) { flags |= O_CREAT | O_TRUNC; }
    int fd = open(path, flags, 0600);
    if (fd < 0) { return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
}

inline BOOL CloseHandle(HANDLE handle)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
    return fd >= 0 && close(fd) == 0;
}

inline BOOL ReadFile(HANDLE, void*, DWORD, DWORD* bytesRead, void*)
{
    // ponytail: HANDLE is a Windows kernel object type with no Android equivalent; every
    // caller of this stub already has a path-based fallback (ComputeHashFromFile), so this
    // deliberately always reports EOF rather than half-implementing Win32 file HANDLEs.
    if (bytesRead) { *bytesRead = 0; }
    return 0;
}

#define ERROR_PRIVILEGE_NOT_HELD 1314L

inline BOOL WriteFile(HANDLE handle, const void* buffer, DWORD size, DWORD* written, void*)
{
    // ponytail: HANDLE has no real POSIX fd behind it in this compat layer (see ReadFile).
    // Not called anywhere in the search/index/manifest path this build targets; a real install
    // path would need HANDLE to actually wrap a POSIX fd end-to-end, which is a bigger change.
    (void)handle; (void)buffer; (void)size;
    if (written) { *written = 0; }
    return 0;
}

inline BOOL StringFromGUID2(const GUID& guid, wchar_t* out, int outSize)
{
    // Standard "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" GUID string form, 39 chars + NUL.
    if (outSize < 39) { return 0; }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    for (int i = 0; i < 39; ++i) { out[i] = static_cast<wchar_t>(buf[i]); }
    out[39] = 0;
    return 39;
}

// Real UTF-8 <-> UTF-16 conversion replacing the Win32 codepage API. wchar_t on Linux/bionic
// is UCS-4, so this treats it as a UTF-16-compatible code-unit store (BMP-correct; supplementary
// planes are approximated 1:1 like the rest of this compat layer -- see Normalize() note).
inline int WideCharToMultiByte(UINT /*codePage*/, DWORD /*flags*/, const wchar_t* wideStr, int wideLen,
    char* out, int outSize, const char* /*defaultChar*/, BOOL* /*usedDefault*/)
{
    if (wideLen < 0) { wideLen = static_cast<int>(std::wstring(wideStr).size()); }
    std::string result;
    result.reserve(wideLen * 3);
    for (int i = 0; i < wideLen; ++i)
    {
        uint32_t cp = static_cast<uint32_t>(static_cast<uint16_t>(wideStr[i]));
        if (cp <= 0x7F) { result.push_back(static_cast<char>(cp)); }
        else if (cp <= 0x7FF)
        {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    if (outSize == 0) { return static_cast<int>(result.size()); }
    int n = std::min<int>(outSize, static_cast<int>(result.size()));
    std::copy(result.begin(), result.begin() + n, out);
    return n;
}

inline int MultiByteToWideChar(UINT /*codePage*/, DWORD /*flags*/, const char* str, int strLen,
    wchar_t* out, int outSize)
{
    if (strLen < 0) { strLen = static_cast<int>(std::string(str).size()); }
    std::wstring result;
    result.reserve(strLen);
    int i = 0;
    while (i < strLen)
    {
        uint8_t b0 = static_cast<uint8_t>(str[i]);
        uint32_t cp; int extra;
        if (b0 < 0x80) { cp = b0; extra = 0; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
        else { cp = b0 & 0x07; extra = 3; }
        ++i;
        for (int k = 0; k < extra && i < strLen; ++k, ++i)
        {
            cp = (cp << 6) | (static_cast<uint8_t>(str[i]) & 0x3F);
        }
        result.push_back(static_cast<wchar_t>(cp));
    }
    if (outSize == 0) { return static_cast<int>(result.size()); }
    int n = std::min<int>(outSize, static_cast<int>(result.size()));
    std::copy(result.begin(), result.begin() + n, out);
    return n;
}

typedef GUID KNOWNFOLDERID;

// Real Windows known-folder GUIDs (SHGetKnownFolderPath) have no Android equivalent path
// registry. Each constant here just needs a distinct, stable identity so GetKnownFolderPath()
// (real rewrite in Filesystem.cpp) can switch on which folder is being asked for and map it
// to a real Termux path (see that function for the actual HOME-based mapping).
inline const GUID FOLDERID_Profile          { 0x00000001, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_LocalAppData     { 0x00000002, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_ProgramData      { 0x00000003, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_ProgramFiles     { 0x00000004, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_ProgramFilesX86  { 0x00000005, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_Downloads        { 0x00000006, 0, 0, {0,0,0,0,0,0,0,0} };
inline const GUID FOLDERID_Fonts            { 0x00000007, 0, 0, {0,0,0,0,0,0,0,0} };

#define KF_FLAG_NO_ALIAS 0
#define KF_FLAG_DONT_VERIFY 0
#define KF_FLAG_NO_PACKAGE_REDIRECTION 0

#define CP_UTF8 65001
#define E_BOUNDS ((HRESULT)0x8000000BL)

typedef long LRESULT;

// Real Windows Installer (msi.h) API enums -- mechanical constants, only ever used to build a
// command-line/argument struct (MsiParsedArguments) that's never executed on Android anyway.
enum INSTALLUILEVEL
{
    INSTALLUILEVEL_NOCHANGE = 0,
    INSTALLUILEVEL_DEFAULT = 1,
    INSTALLUILEVEL_NONE = 2,
    INSTALLUILEVEL_BASIC = 3,
    INSTALLUILEVEL_REDUCED = 4,
    INSTALLUILEVEL_FULL = 5,
    INSTALLUILEVEL_ENDDIALOG = 0x80,
    INSTALLUILEVEL_PROGRESSONLY = 0x40,
    INSTALLUILEVEL_HIDECANCEL = 0x20,
    INSTALLUILEVEL_SOURCERESONLY = 0x100,
    INSTALLUILEVEL_UACONLY = 0x200,
};

enum INSTALLLOGMODE
{
    INSTALLLOGMODE_FATALEXIT = (1 << 0),
    INSTALLLOGMODE_ERROR = (1 << 1),
    INSTALLLOGMODE_WARNING = (1 << 2),
    INSTALLLOGMODE_USER = (1 << 3),
    INSTALLLOGMODE_INFO = (1 << 4),
    INSTALLLOGMODE_RESOLVESOURCE = (1 << 5),
    INSTALLLOGMODE_OUTOFDISKSPACE = (1 << 6),
    INSTALLLOGMODE_ACTIONSTART = (1 << 7),
    INSTALLLOGMODE_ACTIONDATA = (1 << 8),
    INSTALLLOGMODE_COMMONDATA = (1 << 9),
    INSTALLLOGMODE_PROPERTYDUMP = (1 << 10),
    INSTALLLOGMODE_VERBOSE = (1 << 11),
    INSTALLLOGMODE_EXTRADEBUG = (1 << 12),
    INSTALLLOGMODE_LOGONLYONERROR = (1 << 13),
    INSTALLLOGMODE_PROGRESS = (1 << 10),
    INSTALLLOGMODE_INITIALIZE = (1 << 14),
    INSTALLLOGMODE_TERMINATE = (1 << 15),
    INSTALLLOGMODE_SHOWDIALOG = (1 << 16),
    INSTALLLOGMODE_PERFORMANCE = (1 << 17),
};

enum INSTALLLOGATTRIBUTES
{
    INSTALLLOGATTRIBUTES_APPEND = (1 << 0),
    INSTALLLOGATTRIBUTES_FLUSHEACHLINE = (1 << 1),
};
typedef uint64_t WPARAM;
typedef int64_t LPARAM;
#define WINAPI

// CryptoAPI opaque stand-ins: real cert-chain validation is a genuine Windows blocker
// (installer signature pinning). Stubbed here only so headers parse for the search/index path.
typedef void* PCCERT_CONTEXT;
typedef void* PCCERT_CHAIN_CONTEXT;
typedef void* HCERTCHAINENGINE;
typedef void* HCERTSTORE;
#define CERT_CHAIN_REVOCATION_CHECK_CHAIN 0x20000000

namespace winrt::Windows::System
{
    enum class ProcessorArchitecture
    {
        X86 = 0,
        Arm = 5,
        X64 = 9,
        Neutral = 11,
        Arm64 = 12,
        X86OnArm64 = 14,
        Unknown = 0xFFFF,
    };
}

enum NORM_FORM
{
    NormalizationC = 0x1,
    NormalizationD = 0x2,
    NormalizationKC = 0x5,
    NormalizationKD = 0x6,
};
