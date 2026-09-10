#pragma once
#include "../src/auth_logic.h"

inline void TestAuthLogic() {
    using namespace cloudnav;
    assert(AuthFailureDetails("panic: runtime error\nSENSITIVE-TOKEN\ngithub.com/rclone/rclone/lib/oauthutil.ConfigOAuth(0x123)") ==
        L"The bundled rclone engine crashed during account setup. Component: lib/oauthutil.ConfigOAuth.");
    assert(AuthFailureDetails("secret=private").empty());
    assert(AuthFailureDetails("NOTICE: shared Google Drive client_id is being retired").empty());
    const std::string partial = "[cloudnav-onedrive]\ntype = onedrive\ntoken = {\"access_token\":\"synthetic\"}\n";
    assert(!AuthReady(partial, "cloudnav-onedrive", true));
    for (const auto* type : {"personal", "business", "documentLibrary"}) {
        assert(AuthReady(partial + "drive_id = synthetic-drive\ndrive_type = " + type + "\n", "cloudnav-onedrive", true));
    }
    assert(!AuthReady(partial + "drive_id = x\ndrive_type = unknown\n", "cloudnav-onedrive", true));
    assert(!AuthReady("[cloudnav-onedrive]\ntype=onedrive\ntoken={}\ndrive_id=x\ndrive_type=personal", "cloudnav-onedrive", true));
    const std::string google = "[cloudnav-gdrive]\ntype=drive\ntoken={\"access_token\":\"synthetic\"}\n";
    assert(AuthReady(google, "cloudnav-gdrive", false));
    assert(AuthReady(google + "client_id=\nclient_secret=\n", "cloudnav-gdrive", false));
    assert(UsesSharedGoogleClient(google, "cloudnav-gdrive"));
    assert(!AuthReady("[cloudnav-gdrive]\ntype=drive\n", "cloudnav-gdrive", false));
    assert(AuthReady(google + "client_id=own-client\nclient_secret=own-secret\n", "cloudnav-gdrive", false));
    assert(!UsesSharedGoogleClient(google + "client_id=own-client\nclient_secret=own-secret\n", "cloudnav-gdrive"));
    assert(!AuthReady(google + "client_id=own-client\nclient_secret=\n", "cloudnav-gdrive", false));
    assert(!AuthReady(google + "client_id=\nclient_secret=own-secret\n", "cloudnav-gdrive", false));
    assert(!AuthReady(partial + "[other]\ndrive_id=x\ndrive_type=personal", "cloudnav-onedrive", true));
    assert(AuthResponse("notice before response\n{\n\t\"State\": \"\",\n\t\"Option\": null\n}\n")["State"] == "");
    assert(AuthResponse("{\"level\":\"notice\",\"msg\":\"shared {client}\"}\n"
        "{\n\t\"State\": \"done\",\n\t\"Option\": {\n\t\t\"Name\": \"choice\"\n\t}\n}\n"
        "{\"level\":\"notice\",\"msg\":\"after response\"}\n")["State"] == "done");
    assert(AuthResponse("{\"State\":\"first\"}\n{\"State\":\"second\"}\n").is_null());
    assert(AuthResponse("{\"State\":\"truncated\"").is_null());
    assert(AuthResponse("Failed to read line: EOF").is_null());
    std::string automatic;
    for (const auto* name : {"config_is_local", "config_refresh_token", "config_shared_client_id", "config_drive_ok"}) {
        automatic.clear();
        assert(AutomaticAuthAnswer(SyncJson{{"Name", name}}, true, "", automatic) && automatic == "true");
    }
    assert(AutomaticAuthAnswer(SyncJson{{"Name", "config_change_team_drive"}}, false, "", automatic) && automatic == "false");
    const SyncJson types = {{"Name", "config_type"}, {"Examples", SyncJson::array({
        {{"Value", "onedrive"}}, {{"Value", "sharepoint"}}})}};
    assert(AutomaticAuthAnswer(types, true, "", automatic) && automatic == "onedrive");
    const SyncJson drives = {{"Name", "config_driveid"}, {"Examples", SyncJson::array({
        {{"Value", "library"}, {"Help", "Documents (documentLibrary)"}},
        {{"Value", "personal"}, {"Help", "OneDrive (personal)"}},
        {{"Value", "business"}, {"Help", "Work (business)"}}})}};
    assert(AutomaticAuthAnswer(drives, true, "business", automatic) && automatic == "business");
    assert(AutomaticAuthAnswer(drives, true, "missing", automatic) && automatic == "personal");
    const SyncJson fallbackDrive = {{"Name", "config_driveid"}, {"Examples", SyncJson::array({
        {{"Value", "first"}, {"Help", "Unknown drive"}}, {{"Value", "second"}}})}};
    assert(AutomaticAuthAnswer(fallbackDrive, true, "", automatic) && automatic == "first");
    assert(!AutomaticAuthAnswer(SyncJson{{"Name", "unexpected"}}, true, "", automatic));
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
    // The shared-client retirement notice is an internal, nonfatal choice. It
    // must continue without creating another user prompt.
    runs = 0;
    questions = 0;
    assert(CompleteAuth([&](bool continuation, const std::string& state, const std::string& answer, std::string& output) {
        if (runs++ == 0) output = R"({"State":"client_id_warning","Option":{"Name":"config_shared_client_id","Default":false,"Type":"bool"},"Error":""})";
        else {
            assert(continuation && state == "client_id_warning" && answer == "true");
            output = R"({"State":"","Error":""})";
        }
        return true;
    }, [&](const SyncJson& option, std::string& answer) {
        if (!AutomaticAuthAnswer(option, false, "", answer)) return false;
        ++questions;
        return true;
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
