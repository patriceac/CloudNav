#pragma once
#include "sync_logic.h"

namespace cloudnav {
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
    if (!oneDrive) return !fields["client_id"].empty() && !fields["client_secret"].empty();
    return !fields["drive_id"].empty() && (fields["drive_type"] == "personal" ||
        fields["drive_type"] == "business" || fields["drive_type"] == "documentLibrary");
}

// stderr notices may precede the JSON response. Never log this buffer: it can
// contain OAuth credentials. Reject malformed or ambiguous responses.
inline SyncJson AuthResponse(const std::string& output) {
    for (size_t pos = output.find('{'); pos != std::string::npos; pos = output.find('{', pos + 1)) {
        auto value = SyncJson::parse(output.substr(pos), nullptr, false);
        if (value.is_object() && value.contains("State") && value["State"].is_string()) return value;
    }
    return nullptr;
}

// All provider questions go through the GUI. A provider error terminates this
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
            error = L"Account setup returned an unsupported question. Reconnect to try again."; return false;
        }
        if (!ask(response["Option"], answer)) return false;
    }
    error = L"Account setup exceeded its step limit. Reconnect to try again.";
    return false;
}
}
