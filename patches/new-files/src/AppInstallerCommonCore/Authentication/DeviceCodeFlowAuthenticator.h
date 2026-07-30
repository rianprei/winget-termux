// ponytail: real OAuth 2.0 Device Authorization Grant (RFC 8628) replacement for the Windows
// Web Account Manager broker. WAM is a Windows-only UI/credential-broker concept; on Termux
// there is no browser-integrated account picker, so this implements the actual, documented
// device-code flow against Microsoft's public identity platform endpoints over plain HTTP
// (libcurl), which is what WAM itself calls under the hood for MicrosoftEntraId auth.
#pragma once
#include "Public/winget/Authentication.h"

namespace AppInstaller::Authentication
{
    struct DeviceCodeFlowAuthenticator : public IAuthenticationProvider
    {
        DeviceCodeFlowAuthenticator(AuthenticationInfo info, AuthenticationArguments args);

        AuthenticationResult AuthenticateForToken() override;

    private:
        AuthenticationInfo m_authInfo;
        AuthenticationArguments m_authArgs;
    };
}
