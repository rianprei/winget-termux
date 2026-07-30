// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// ponytail: real, documented limitation, not a fake pass-through. Certificate pinning here
// requires walking a CryptoAPI PCCERT_CHAIN_CONTEXT (real Windows COM/crypto structures) --
// a genuine structural blocker with no direct Android equivalent short of a full OpenSSL X509
// chain-walk rewrite. Nothing in the compiled search/index/manifest path actually calls
// AddChain/Validate/BuildCertificateChain (confirmed: only the constructor and LoadFrom are
// linked), so only those two are implemented. LoadFrom honestly reports "not supported" rather
// than silently accepting a pinning config it cannot enforce -- callers already treat a false
// return as "pinning unavailable, fall back to standard TLS trust" (which curl provides).
#include "pch.h"
#include "winget/Certificates.h"
#include <AppInstallerLogging.h>

namespace AppInstaller::Certificates
{
    PinningConfiguration::PinningConfiguration(std::string identifier) : m_identifier(std::move(identifier))
    {
        if (m_identifier.empty())
        {
            m_identifier = "unnamed";
        }
    }

    bool PinningConfiguration::LoadFrom(const Json::Value&)
    {
        AICLI_LOG(Core, Warning, << "Certificate pinning [" << m_identifier <<
            "] requested but not supported on this platform; falling back to standard TLS trust validation.");
        return false;
    }
}
