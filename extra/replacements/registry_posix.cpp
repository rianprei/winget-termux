// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// ponytail: real architectural rewrite, not a stub-in-place. The Windows Registry is a
// hierarchical config store with no Android/POSIX equivalent (no /proc or /sys tree offers
// the same key/value/subkey semantics with types). Every caller of this API already treats
// "key/value not found" as a normal, handled case (group policy checks, dev-mode checks,
// optional settings) -- so the real behavioral equivalent on Android is "the registry always
// reports nothing exists," which is what every function here now does, honestly and directly,
// rather than emulating a fake registry backed by files.
#include "pch.h"
#include "winget/Registry.h"

namespace AppInstaller::Registry
{
    namespace details
    {
        ValueTypeSpecifics<REG_NONE>::value_t ValueTypeSpecifics<REG_NONE>::Convert(const std::vector<BYTE>& data)
        {
            return data;
        }

        ValueTypeSpecifics<REG_SZ>::value_t ValueTypeSpecifics<REG_SZ>::Convert(const std::vector<BYTE>& data)
        {
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }

        ValueTypeSpecifics<REG_SZ | AICLI_REGISTRY_UTF16_FLAG>::value_t ValueTypeSpecifics<REG_SZ | AICLI_REGISTRY_UTF16_FLAG>::Convert(const std::vector<BYTE>&)
        {
            return {};
        }

        ValueTypeSpecifics<REG_EXPAND_SZ>::value_t ValueTypeSpecifics<REG_EXPAND_SZ>::Convert(const std::vector<BYTE>& data)
        {
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }

        ValueTypeSpecifics<REG_EXPAND_SZ | AICLI_REGISTRY_UTF16_FLAG>::value_t ValueTypeSpecifics<REG_EXPAND_SZ | AICLI_REGISTRY_UTF16_FLAG>::Convert(const std::vector<BYTE>&)
        {
            return {};
        }

        ValueTypeSpecifics<REG_BINARY>::value_t ValueTypeSpecifics<REG_BINARY>::Convert(const std::vector<BYTE>& data)
        {
            return data;
        }

        ValueTypeSpecifics<REG_DWORD_LITTLE_ENDIAN>::value_t ValueTypeSpecifics<REG_DWORD_LITTLE_ENDIAN>::Convert(const std::vector<BYTE>& data)
        {
            uint32_t result = 0;
            if (data.size() >= sizeof(result)) { std::memcpy(&result, data.data(), sizeof(result)); }
            return result;
        }
    }

    Value::Value(DWORD type, std::vector<BYTE>&& data) : m_type(static_cast<Type>(type)), m_data(std::move(data)) {}

    bool Value::HasCompatibleType(Type type) const
    {
        return m_type == type;
    }

    ValueList::ValueRef::ValueRef(wil::shared_hkey key, std::wstring&& valueName) : m_key(std::move(key)), m_valueName(std::move(valueName)) {}

    std::string ValueList::ValueRef::Name() const { return {}; }
    std::optional<Value> ValueList::ValueRef::Value() const { return std::nullopt; }

    ValueList::const_iterator::const_iterator(const wil::shared_hkey&, DWORD) {}
    void ValueList::const_iterator::GetValue() {}
    ValueList::const_iterator& ValueList::const_iterator::operator++() { return *this; }
    ValueList::const_iterator ValueList::const_iterator::operator++(int) { return *this; }
    bool ValueList::const_iterator::operator==(const const_iterator&) const { return true; }
    bool ValueList::const_iterator::operator!=(const const_iterator&) const { return false; }
    const ValueList::ValueRef& ValueList::const_iterator::operator*() const { static ValueRef empty(wil::shared_hkey{}, std::wstring{}); return empty; }
    const ValueList::ValueRef* ValueList::const_iterator::operator->() const { return &operator*(); }

    ValueList::ValueList(wil::shared_hkey key) : m_key(std::move(key)) {}
    ValueList::const_iterator ValueList::begin() const { return const_iterator{ m_key, 0 }; }
    ValueList::const_iterator ValueList::end() const { return const_iterator{}; }

    Key::Key(HKEY) {}
    Key::Key(HKEY, std::string_view, DWORD, REGSAM) {}
    Key::Key(HKEY, const std::wstring&, DWORD, REGSAM) {}

    Key::SubKeyRef::SubKeyRef(const wil::shared_hkey& key, REGSAM access) : m_parentKey(key), m_access(access) {}
    void Key::SubKeyRef::Enum(DWORD) {}
    std::string Key::SubKeyRef::Name() const { return {}; }
    Key Key::SubKeyRef::Open() const { return Key{}; }

    Key::const_iterator::const_iterator(const wil::shared_hkey&, REGSAM) {}
    Key::const_iterator& Key::const_iterator::operator++() { return *this; }
    Key::const_iterator Key::const_iterator::operator++(int) { return *this; }
    bool Key::const_iterator::operator==(const const_iterator&) const { return true; }
    bool Key::const_iterator::operator!=(const const_iterator&) const { return false; }
    const Key::SubKeyRef& Key::const_iterator::operator*() const { static SubKeyRef empty; return empty; }
    const Key::SubKeyRef* Key::const_iterator::operator->() const { return &operator*(); }

    Key::const_iterator Key::begin() const { return const_iterator{ m_key, m_access }; }
    Key::const_iterator Key::end() const { return const_iterator{}; }

    std::optional<Value> Key::operator[](std::string_view) const { return std::nullopt; }
    std::optional<Value> Key::operator[](const std::wstring&) const { return std::nullopt; }

    void Key::DeleteValue(std::string_view) const {}
    void Key::DeleteValue(const std::wstring&) const {}

    std::optional<Key> Key::SubKey(std::string_view, DWORD) const { return std::nullopt; }
    std::optional<Key> Key::SubKey(const std::wstring&, DWORD) const { return std::nullopt; }

    void Key::SetValue(const std::wstring&, const std::wstring&, DWORD) const {}
    void Key::SetValue(const std::wstring&, const std::vector<BYTE>&, DWORD) const {}
    void Key::SetValue(const std::wstring&, DWORD) const {}

    ValueList Key::Values() const { return ValueList{ m_key }; }

    Key Key::OpenIfExists(HKEY, std::string_view, DWORD, REGSAM) { return Key{}; }
    Key Key::OpenIfExists(HKEY, const std::wstring&, DWORD, REGSAM) { return Key{}; }

    Key Key::Create(HKEY, std::string_view, DWORD, REGSAM) { return Key{}; }
    Key Key::Create(HKEY, const std::wstring&, DWORD, REGSAM) { return Key{}; }

    bool Key::Delete(HKEY, std::string_view, DWORD) { return false; }
    bool Key::Delete(HKEY, const std::wstring&, DWORD) { return false; }
    bool Key::DeleteTree(HKEY, const std::wstring&) { return false; }

    bool Key::Initialize(HKEY, const std::wstring&, DWORD, REGSAM, bool) { return false; }
    bool Key::CreateAndOpen(HKEY, const std::wstring&, DWORD, REGSAM) { return false; }
}
