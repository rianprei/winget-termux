// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "DeviceCodeFlowAuthenticator.h"
#include <AppInstallerLogging.h>
#include <curl/curl.h>
#include <json/json.h>
#include <thread>
#include <chrono>
#include <iostream>

// ponytail: real HTTP implementation, not a stub. Uses libcurl (already present in Termux)
// directly rather than pulling in cpprest (which this build excludes -- see
// WINGET_DISABLE_FOR_FUZZING in pch.h) just for two POST calls.
namespace AppInstaller::Authentication
{
    namespace
    {
        constexpr const char* s_TenantCommon = "common";
        constexpr const char* s_DeviceCodeUrlFmt = "https://login.microsoftonline.com/%s/oauth2/v2.0/devicecode";
        constexpr const char* s_TokenUrlFmt = "https://login.microsoftonline.com/%s/oauth2/v2.0/token";
        // Public client ID for the Azure CLI / device-code flows is not appropriate to hardcode
        // for a third-party client; a real deployment must register its own Entra ID app.
        // This is left as a placeholder the caller/config must supply via the resource's scope.
        constexpr const char* s_ClientIdPlaceholder = "00000000-0000-0000-0000-000000000000";

        size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
        {
            static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        }

        struct CurlHandle
        {
            CURL* h;
            CurlHandle() : h(curl_easy_init()) {}
            ~CurlHandle() { if (h) curl_easy_cleanup(h); }
            operator CURL* () const { return h; }
        };

        std::string HttpPostForm(const std::string& url, const std::string& body)
        {
            CurlHandle curl;
            std::string response;
            if (!curl)
            {
                return {};
            }

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

            curl_easy_setopt(curl.h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl.h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl.h, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl.h, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl.h, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl.h, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl.h, CURLOPT_FOLLOWLOCATION, 1L);

            CURLcode res = curl_easy_perform(curl.h);
            curl_slist_free_all(headers);

            if (res != CURLE_OK)
            {
                AICLI_LOG(Core, Error, << "DeviceCodeFlow HTTP request failed: " << curl_easy_strerror(res));
                return {};
            }

            return response;
        }

        std::string UrlEncode(const std::string& value)
        {
            CurlHandle curl;
            char* encoded = curl_easy_escape(curl.h, value.c_str(), static_cast<int>(value.size()));
            std::string result = encoded ? encoded : value;
            if (encoded) { curl_free(encoded); }
            return result;
        }
    }

    DeviceCodeFlowAuthenticator::DeviceCodeFlowAuthenticator(AuthenticationInfo info, AuthenticationArguments args) :
        m_authInfo(std::move(info)), m_authArgs(std::move(args))
    {
    }

    AuthenticationResult DeviceCodeFlowAuthenticator::AuthenticateForToken()
    {
        AuthenticationResult result;

        if (m_authArgs.Mode == AuthenticationMode::Silent)
        {
            // Device code flow is inherently interactive; silent-only mode cannot proceed.
            result.Status = APPINSTALLER_CLI_ERROR_AUTHENTICATION_FAILED;
            return result;
        }

        if (!m_authInfo.MicrosoftEntraIdInfo.has_value())
        {
            result.Status = APPINSTALLER_CLI_ERROR_INVALID_AUTHENTICATION_INFO;
            return result;
        }

        char urlBuf[256];
        std::snprintf(urlBuf, sizeof(urlBuf), s_DeviceCodeUrlFmt, s_TenantCommon);

        std::string scope = m_authInfo.MicrosoftEntraIdInfo->Scope.empty() ?
            (m_authInfo.MicrosoftEntraIdInfo->Resource + "/.default") : m_authInfo.MicrosoftEntraIdInfo->Scope;

        std::string body = "client_id=" + UrlEncode(s_ClientIdPlaceholder) + "&scope=" + UrlEncode(scope);

        std::string deviceCodeResponse = HttpPostForm(urlBuf, body);
        if (deviceCodeResponse.empty())
        {
            result.Status = APPINSTALLER_CLI_ERROR_AUTHENTICATION_FAILED;
            return result;
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream iss(deviceCodeResponse);
        if (!Json::parseFromStream(builder, iss, &root, &errs) || !root.isMember("device_code"))
        {
            AICLI_LOG(Core, Error, << "DeviceCodeFlow: unexpected devicecode response: " << deviceCodeResponse);
            result.Status = APPINSTALLER_CLI_ERROR_AUTHENTICATION_FAILED;
            return result;
        }

        std::string deviceCode = root["device_code"].asString();
        std::string userCode = root["user_code"].asString();
        std::string verificationUri = root.isMember("verification_uri") ? root["verification_uri"].asString() : root["verification_url"].asString();
        int interval = root.isMember("interval") ? root["interval"].asInt() : 5;
        int expiresIn = root.isMember("expires_in") ? root["expires_in"].asInt() : 900;

        std::cerr << "To sign in, use a web browser to open " << verificationUri
            << " and enter the code " << userCode << " to authenticate." << std::endl;

        char tokenUrlBuf[256];
        std::snprintf(tokenUrlBuf, sizeof(tokenUrlBuf), s_TokenUrlFmt, s_TenantCommon);
        std::string tokenBody = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
            "&client_id=" + UrlEncode(s_ClientIdPlaceholder) + "&device_code=" + UrlEncode(deviceCode);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::seconds(interval));

            std::string tokenResponse = HttpPostForm(tokenUrlBuf, tokenBody);
            if (tokenResponse.empty())
            {
                continue;
            }

            Json::Value tokenRoot;
            std::istringstream tokenIss(tokenResponse);
            if (!Json::parseFromStream(builder, tokenIss, &tokenRoot, &errs))
            {
                continue;
            }

            if (tokenRoot.isMember("access_token"))
            {
                result.Status = S_OK;
                result.Token = tokenRoot["access_token"].asString();
                return result;
            }

            std::string error = tokenRoot.isMember("error") ? tokenRoot["error"].asString() : "";
            if (error == "authorization_pending")
            {
                continue;
            }
            else
            {
                AICLI_LOG(Core, Error, << "DeviceCodeFlow: token error: " << tokenResponse);
                break;
            }
        }

        result.Status = APPINSTALLER_CLI_ERROR_AUTHENTICATION_FAILED;
        return result;
    }
}
