#pragma once
#include <memory>
#include <string>

namespace wil
{
    template <typename T, typename Pfn, Pfn pfn>
    class unique_any
    {
    public:
        unique_any() = default;
        explicit unique_any(T value) : m_value(value), m_valid(true) {}
        ~unique_any() { reset(); }
        unique_any(const unique_any&) = delete;
        unique_any& operator=(const unique_any&) = delete;
        unique_any(unique_any&& other) noexcept : m_value(other.m_value), m_valid(other.m_valid) { other.m_valid = false; }
        unique_any& operator=(unique_any&& other) noexcept
        {
            if (this != std::addressof(other)) { reset(); m_value = other.m_value; m_valid = other.m_valid; other.m_valid = false; }
            return *this;
        }
        void reset()
        {
            if (m_valid) { pfn(m_value); m_valid = false; }
        }
        void reset(T value)
        {
            reset();
            m_value = value;
            m_valid = true;
        }
        T get() const { return m_value; }
        explicit operator bool() const { return m_valid; }
        T release() { m_valid = false; return m_value; }
        T* operator&() { reset(); m_valid = true; return &m_value; }
        T* addressof() { return &m_value; }
    private:
        T m_value{};
        bool m_valid = false;
    };

    struct shared_cert_context { void* p = nullptr; explicit operator bool() const { return p != nullptr; } void* get() const { return p; } };
    struct unique_cert_chain_context { void* p = nullptr; explicit operator bool() const { return p != nullptr; } };
    struct unique_handle { void* p = nullptr; explicit operator bool() const { return p != nullptr; } void reset() { p = nullptr; } void reset(void* value) { p = value; } void* get() const { return p; } };

    struct shared_hkey
    {
        void* h = nullptr;
        shared_hkey() = default;
        explicit shared_hkey(void* key) : h(key) {}
        explicit operator bool() const { return h != nullptr; }
        void* get() const { return h; }
        static shared_hkey open(void*, const wchar_t*, unsigned long, unsigned long) { return shared_hkey{}; }
        static shared_hkey open(void*, const std::wstring&, unsigned long, unsigned long) { return shared_hkey{}; }
        static shared_hkey create(void*, const wchar_t*, unsigned long, unsigned long, unsigned long) { return shared_hkey{}; }
        static shared_hkey create(void*, const std::wstring&, unsigned long, unsigned long, unsigned long) { return shared_hkey{}; }
    };

    template <typename F>
    struct scope_exit_t
    {
        F f;
        bool active = true;
        ~scope_exit_t() { if (active) { f(); } }
    };
    template <typename F>
    scope_exit_t<F> scope_exit(F&& f) { return scope_exit_t<F>{ std::forward<F>(f) }; }

    struct srwlock
    {
        struct scoped_lock { ~scoped_lock() {} };
        scoped_lock lock_exclusive() { return scoped_lock{}; }
        scoped_lock lock_shared() { return scoped_lock{}; }
    };
}
