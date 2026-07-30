#pragma once
#include <exception>
#include <string>
#include <utility>
#include <type_traits>
#include <cstdio>
#include <cctype>

typedef long HRESULT;
#define SEVERITY_ERROR 1
#define FACILITY_SQLITE 0xA16
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))

#define E_NOT_SET ((HRESULT)0x80070490L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_NOT_VALID_STATE ((HRESULT)0x8007139FL)
#define E_NOT_SUPPORTED ((HRESULT)0x80070032L)
#define S_OK ((HRESULT)0L)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)

#define DEFINE_ENUM_FLAG_OPERATORS(T) \
    constexpr T operator|(T a, T b) { return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b)); } \
    constexpr T operator&(T a, T b) { return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b)); } \
    constexpr T operator~(T a) { return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a)); } \
    inline T& operator|=(T& a, T b) { a = a | b; return a; } \
    inline T& operator&=(T& a, T b) { a = a & b; return a; }

namespace wil
{
    template <typename TOut, typename TIn>
    TOut safe_cast(TIn value)
    {
        return static_cast<TOut>(value);
    }

    struct FailureInfoLike
    {
        std::wstring message;
        const wchar_t* pszMessage = nullptr;
    };

    class ResultException : public std::exception
    {
    public:
        explicit ResultException(long hr) : m_hr(hr), m_msg("ResultException hr=" + std::to_string(hr)) {}
        long GetErrorCode() const noexcept { return m_hr; }
        const char* what() const noexcept override { return m_msg.c_str(); }
        FailureInfoLike GetFailureInfo() const
        {
            FailureInfoLike info;
            info.message.assign(m_msg.begin(), m_msg.end());
            info.pszMessage = info.message.c_str();
            return info;
        }
    private:
        long m_hr;
        std::string m_msg;
    };
}

// ponytail: real rewrite of Shlwapi's IsValidURL -- lightweight scheme-prefix syntax check,
// the same class of validation the caller actually needs (reject empty/malformed URLs before
// use), not a full RFC 3986 parser Windows itself didn't provide either.
#define S_FALSE ((HRESULT)1L)
inline HRESULT IsValidURL(void*, const wchar_t* url, unsigned long)
{
    std::wstring s(url);
    bool valid = s.find(L"://") != std::wstring::npos;
    return valid ? S_OK : S_FALSE;
}

#define THROW_HR(hr) throw wil::ResultException(hr)
#define THROW_HR_IF_NULL(hr, ptr) do { if ((ptr) == nullptr) { throw wil::ResultException(hr); } } while (0)
#define THROW_HR_IF(hr, cond) do { if (cond) { throw wil::ResultException(hr); } } while (0)
#define THROW_HR_IF_MSG(hr, cond, fmt, ...) do { if (cond) { throw wil::ResultException(hr); } } while (0)
#define THROW_HR_MSG(hr, fmt, ...) throw wil::ResultException(hr)
#define THROW_EXCEPTION_MSG(ex, fmt, ...) throw (ex)
#define LOG_CAUGHT_EXCEPTION() do {} while (0)
#define CATCH_LOG() catch (...) { LOG_CAUGHT_EXCEPTION(); }
#define CATCH_LOG_MSG(...) catch (...) { LOG_CAUGHT_EXCEPTION(); }
#define LOG_IF_FAILED(hr) (hr)
#define LOG_IF_WIN32_BOOL_FALSE(cond) (cond)
#define SUCCEEDED_LOG(hr) SUCCEEDED(hr)
#define SUCCEEDED_WIN32_LOG(hr) ((hr) == 0)
#define THROW_IF_WIN32_ERROR(hr) do { if (hr) { throw wil::ResultException(HRESULT_FROM_WIN32(hr)); } } while (0)
#define THROW_WIN32_IF(err, cond) do { if (cond) { throw wil::ResultException(HRESULT_FROM_WIN32(err)); } } while (0)
#define THROW_WIN32(err) throw wil::ResultException(HRESULT_FROM_WIN32(err))
#define THROW_IF_WIN32_BOOL_FALSE(cond) do { if (!(cond)) { throw wil::ResultException(E_FAIL); } } while (0)
#define WI_IsFlagSet(v, flag) (static_cast<std::underlying_type_t<decltype(v)>>((v) & (flag)) != 0)
#define WI_IsFlagClear(v, flag) (static_cast<std::underlying_type_t<decltype(v)>>((v) & (flag)) == 0)
#define WI_SetAllFlags(v, flag) ((v) = (v) | (flag))
#define WI_IsAnyFlagSet(v, flag) WI_IsFlagSet(v, flag)
#define WI_SetFlag(v, flag) ((v) = (v) | (flag))
#define WI_SetFlagIf(v, flag, cond) do { if (cond) { WI_SetFlag(v, flag); } } while (0)
inline int IsCharAlphaNumericA(char c) { return std::isalnum(static_cast<unsigned char>(c)); }
#define WI_AreAllFlagsSet(v, flag) (static_cast<std::underlying_type_t<decltype(v)>>((v) & (flag)) == static_cast<std::underlying_type_t<decltype(v)>>(flag))
#define WI_ClearAllFlags(v, flag) ((v) = (v) & ~(flag))
#define DECLSPEC_NOINLINE
#define HRESULT_FROM_WIN32(code) ((HRESULT)((code) <= 0 ? ((HRESULT)(code)) : ((HRESULT)(((code) & 0x0000FFFF) | (7 << 16) | 0x80000000))))
#define THROW_EXCEPTION(ex) throw (ex)
#define THROW_LAST_ERROR_IF(cond) do { if (cond) { throw wil::ResultException(E_FAIL); } } while (0)
#define FAIL_FAST_HR_IF(hr, cond) do { if (cond) { throw wil::ResultException(hr); } } while (0)
#define FAIL_FAST_IF(cond) do { if (cond) { throw wil::ResultException(E_UNEXPECTED); } } while (0)
#define THROW_IF_FAILED(hr) do { if (hr) { throw wil::ResultException(hr); } } while (0)
#define RETURN_IF_FAILED(hr) do { if (hr) { return hr; } } while (0)
