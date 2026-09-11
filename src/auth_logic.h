#pragma once
#include "sync_logic.h"

namespace cloudnav {
inline std::wstring AuthFailureDetails(const std::string& output) {
    // Deliberately return fixed descriptions, never provider output or tokens.
    if (output.find("panic:") != std::string::npos) {
        std::wstring result = L"The bundled rclone engine crashed during account setup.";
        const std::regex frame(R"(github\.com/rclone/rclone/([A-Za-z0-9_./]+))");
        std::smatch match;
        if (std::regex_search(output, match, frame)) {
            const auto symbol = match[1].str();
            result += L" Component: " + std::wstring(symbol.begin(), symbol.end()) + L".";
        }
        return result;
    }
    if (output.find("Failed to read line: EOF") != std::string::npos) return L"rclone unexpectedly requested terminal input.";
    if (output.find("bind:") != std::string::npos) return L"The local browser sign-in port is unavailable. Close other sign-in attempts and reconnect.";
    if (output.find("invalid_client") != std::string::npos) return L"Google rejected the OAuth client. Reconnect with a supported client.";
    if (output.find("invalid_grant") != std::string::npos) return L"The saved authorization has expired or was revoked. Reconnect to sign in again.";
    if (output.find("RATE_LIMIT_EXCEEDED") != std::string::npos ||
        output.find("rateLimitExceeded") != std::string::npos || output.find("Quota exceeded") != std::string::npos)
        return L"Google Drive's API quota is temporarily exhausted. Try again later.";
    if (output.find("unknown flag") != std::string::npos) return L"The bundled engine rejected an account setup option.";
    return {};
}
inline std::map<std::string, std::string> AuthFields(const std::string& config, const std::string& remote) {
    const auto section = SyncConfigSection(config, remote);
    std::map<std::string, std::string> fields;
    if (section.first == std::string::npos) return fields;
    std::istringstream input(config.substr(section.first, section.second));
    const auto trim = [](const std::string& text) {
        const auto first = text.find_first_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    };
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto equals = line.find('=');
        if (equals != std::string::npos) fields[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return fields;
}

inline bool AuthReady(const std::string& config, const std::string& remote, bool oneDrive) {
    auto fields = AuthFields(config, remote);
    if (fields["type"] != (oneDrive ? "onedrive" : "drive")) return false;
    const auto token = SyncJson::parse(fields["token"], nullptr, false);
    if (!token.is_object() || !token.contains("access_token") || !token["access_token"].is_string() ||
        token["access_token"].get<std::string>().empty()) return false;
    // The shared rclone client is retired during 2026. Google connections are
    // ready only after a dedicated desktop client and a per-user token exist.
    if (!oneDrive) return !fields["client_id"].empty() && !fields["client_secret"].empty();
    return !fields["drive_id"].empty() && (fields["drive_type"] == "personal" ||
        fields["drive_type"] == "business" || fields["drive_type"] == "documentLibrary");
}

inline bool AuthOptionHasValue(const SyncJson& option, const std::string& value) {
    if (!option.contains("Examples") || !option["Examples"].is_array()) return false;
    return std::any_of(option["Examples"].begin(), option["Examples"].end(), [&](const auto& example) {
        return example.is_object() && example.value("Value", std::string()) == value;
    });
}

// CloudNav connects personal cloud roots, so provider configuration choices are
// deterministic. Preserve an existing OneDrive drive when it is still offered;
// otherwise prefer a personal/business root and finally rclone's first choice.
inline bool AutomaticAuthAnswer(const SyncJson& option, bool oneDrive,
    const std::string& preferredDriveId, std::string& answer) {
    if (!option.is_object()) return false;
    const auto name = option.value("Name", std::string());
    if (name == "config_is_local" || name == "config_refresh_token" ||
        (oneDrive && name == "config_drive_ok")) {
        answer = "true";
        return true;
    }
    // New Google configurations use My Drive. Existing Shared Drive IDs remain
    // unchanged because rclone asks whether to replace the current choice.
    if (!oneDrive && name == "config_change_team_drive") {
        answer = "false";
        return true;
    }
    if (oneDrive && name == "config_type" && AuthOptionHasValue(option, "onedrive")) {
        answer = "onedrive";
        return true;
    }
    if (oneDrive && name == "config_driveid" && option.contains("Examples") && option["Examples"].is_array()) {
        if (!preferredDriveId.empty() && AuthOptionHasValue(option, preferredDriveId)) {
            answer = preferredDriveId;
            return true;
        }
        for (const auto* kind : {"(personal)", "(business)"}) {
            for (const auto& example : option["Examples"]) {
                if (!example.is_object()) continue;
                const auto value = example.value("Value", std::string());
                const auto help = example.value("Help", std::string());
                if (!value.empty() && help.find(kind) != std::string::npos) {
                    answer = value;
                    return true;
                }
            }
        }
        for (const auto& example : option["Examples"]) {
            if (!example.is_object()) continue;
            answer = example.value("Value", std::string());
            if (!answer.empty()) return true;
        }
    }
    return false;
}

// stderr notices may precede the JSON response. Never log this buffer: it can
// contain OAuth credentials. Reject malformed or ambiguous responses.
inline SyncJson AuthResponse(const std::string& output) {
    SyncJson response = nullptr;
    size_t search = 0;
    while ((search = output.find('{', search)) != std::string::npos) {
        const size_t start = search++;
        size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (size_t end = start; end < output.size(); ++end) {
            const char ch = output[end];
            if (inString) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') inString = false;
                continue;
            }
            if (ch == '"') { inString = true; continue; }
            if (ch == '{') { ++depth; continue; }
            if (ch != '}' || --depth != 0) continue;
            auto value = SyncJson::parse(output.substr(start, end - start + 1), nullptr, false);
            if (value.is_object() && value.contains("State") && value["State"].is_string()) {
                if (!response.is_null()) return nullptr;
                response = std::move(value);
            }
            break;
        }
    }
    return response;
}

// CloudNav resolves provider choices itself. A provider error terminates this
// attempt; reconnect starts fresh discovery instead of replaying stale state.
template<class Run, class Ask>
bool CompleteAuth(Run run, Ask ask, std::wstring& error) {
    std::string state, answer;
    for (unsigned step = 0; step < 32; ++step) {
        std::string output;
        if (!run(step != 0, state, answer, output)) return false;
        const auto response = AuthResponse(output);
        if (response.is_null()) { error = L"Account setup returned an invalid response. Reconnect to try again."; return false; }
        if (response.contains("Error") && (!response["Error"].is_string() || !response["Error"].get<std::string>().empty())) {
            error = L"The provider could not validate this account or drive. Reconnect to discover drives again, or select another drive.";
            return false;
        }
        state = response["State"].get<std::string>();
        if (state.empty()) return true;
        if (!response.contains("Option") || !response["Option"].is_object()) {
            error = L"CloudNav could not finish selecting the account storage. Reconnect to try again."; return false;
        }
        if (!ask(response["Option"], answer)) {
            if (error.empty()) error = L"CloudNav could not finish selecting the account storage. Reconnect to try again.";
            return false;
        }
    }
    error = L"Account setup exceeded its step limit. Reconnect to try again.";
    return false;
}
}
