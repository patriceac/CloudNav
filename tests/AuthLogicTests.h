#pragma once
#include "../src/auth_logic.h"

inline void TestAuthLogic() {
    using namespace cloudnav;
    const std::string partial = "[cloudnav-onedrive]\ntype = onedrive\ntoken = {\"access_token\":\"synthetic\"}\n";
    assert(!AuthReady(partial, "cloudnav-onedrive", true));
    for (const auto* type : {"personal", "business", "documentLibrary"}) {
        assert(AuthReady(partial + "drive_id = synthetic-drive\ndrive_type = " + type + "\n", "cloudnav-onedrive", true));
    }
    assert(!AuthReady(partial + "drive_id = x\ndrive_type = unknown\n", "cloudnav-onedrive", true));
    assert(!AuthReady("[cloudnav-onedrive]\ntype=onedrive\ntoken={}\ndrive_id=x\ndrive_type=personal", "cloudnav-onedrive", true));
    const std::string google = "[cloudnav-gdrive]\ntype=drive\ntoken={\"access_token\":\"synthetic\"}\n";
    assert(!AuthReady(google, "cloudnav-gdrive", false));
    assert(AuthReady(google + "client_id=own-client\nclient_secret=own-secret\n", "cloudnav-gdrive", false));
    assert(!AuthReady(partial + "[other]\ndrive_id=x\ndrive_type=personal", "cloudnav-onedrive", true));
    assert(AuthResponse("notice before response\n{\"State\":\"\",\"Option\":null}\n")["State"] == "");
    assert(AuthResponse("Failed to read line: EOF").is_null());
    std::wstring error;
    unsigned runs = 0, questions = 0;
    assert(CompleteAuth([&](bool continuation, const std::string& state, const std::string& answer, std::string& output) {
        if (runs++ == 0) {
            assert(!continuation && state.empty());
            output = R"({"State":"choose-drive,opaque","Option":{"Name":"config_driveid","Examples":[{"Value":"synthetic-drive"}]},"Error":""})";
        } else {
            assert(continuation && state == "choose-drive,opaque" && answer == "synthetic-drive");
            output = R"({"State":"","Option":null,"Error":""})";
        }
        return true;
    }, [&](const SyncJson& option, std::string& answer) {
        ++questions; assert(option["Name"] == "config_driveid"); answer = "synthetic-drive"; return true;
    }, error));
    assert(runs == 2 && questions == 1);
    for (const auto* output : {R"({"State":"choose_type","Error":"ObjectHandle is Invalid"})",
        R"({"State":"choose_type","Error":"RootURL not set"})", "Failed to read line: EOF", R"({"State":"next"})"}) {
        runs = 0;
        assert(!CompleteAuth([&](bool, const std::string&, const std::string&, std::string& result) {
            ++runs; result = output; return true;
        }, [](const SyncJson&, std::string&) { assert(false); return false; }, error));
        assert(runs == 1);
    }
    runs = 0;
    assert(!CompleteAuth([&](bool, const std::string&, const std::string&, std::string& output) {
        ++runs; output = R"({"State":"loop","Option":{"Name":"unknown"}})"; return true;
    }, [](const SyncJson&, std::string& answer) { answer = ""; return true; }, error));
    assert(runs == 32);
    assert(!CompleteAuth([](bool, const std::string&, const std::string&, std::string& output) {
        output = R"({"State":"question","Option":{"Name":"drive"}})"; return true;
    }, [](const SyncJson&, std::string&) { return false; }, error));
    assert(!CompleteAuth([](bool, const std::string&, const std::string&, std::string&) { return false; },
        [](const SyncJson&, std::string&) { assert(false); return true; }, error));
}
