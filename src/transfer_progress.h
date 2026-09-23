#pragma once

#include "migration_logic.h"
#include "sync_logic.h"
#include "../third_party/nlohmann/json.hpp"
#include <deque>
#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace cloudnav {

// Work is identified by batch and source path, not rclone's traffic counters.
// Those counters include retries and may even account for a file more than once.
class TransferProgress {
public:
    using Json = nlohmann::json;
    struct File {
        std::string path;
        std::uint64_t bytes = 0;
        bool complete = false;
    };
    struct Display {
        int percent = 0;
        std::wstring text, details;
        double eta = -1;
    };

    size_t AddBatch(const std::vector<File>& files, bool move = false) {
        Batch batch;
        batch.move = move;
        for (const auto& file : files) {
            if (!batch.files.emplace(file.path, file).second) throw std::runtime_error("Duplicate progress path");
            if (file.bytes > (std::numeric_limits<std::uint64_t>::max)() - totalBytes_)
                throw std::runtime_error("Transfer plan is too large");
            totalBytes_ += file.bytes;
            ++totalFiles_;
        }
        batches_.push_back(std::move(batch));
        return batches_.size() - 1;
    }

    void BeginBatch(size_t batch, double now) {
        current_ = batch;
        active_.clear();
        failed_.clear();
        engineErrors_ = false;
        googleQuota_ = false;
        trafficBase_ = traffic_;
        previousTraffic_ = 0;
        lastAdvance_ = now;
        lastLogical_ = static_cast<double>(completedBytes_);
        // Keep whole-job history across subprocesses. A pause still expires it.
        if (samples_.empty()) samples_.push_back({now, lastLogical_});
    }

    void Log(const std::string& line, double now) {
        const auto json = Json::parse(line, nullptr, false);
        if (!json.is_object() || current_ >= batches_.size()) return;
        auto& batch = batches_[current_];
        const auto string = [&](const char* key) {
            const auto it = json.find(key);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        const auto message = string("msg"), path = string("object"), level = string("level");
        if (level == "error" || level == "critical") {
            engineErrors_ = true;
            if (!path.empty() && batch.files.count(path)) {
                failed_.insert(path);
                active_.erase(path);
            }
            googleQuota_ |= message.find("RATE_LIMIT_EXCEEDED") != std::string::npos ||
                (message.find("googleapi") != std::string::npos && message.find("429") != std::string::npos);
            samples_.clear();
        }
        // INFO is emitted by the pinned engine only after copy verification.
        // A backup-dir move must never complete the copy that caused it.
        const bool copied = message.rfind("Copied (", 0) == 0 || message.rfind("Multi-thread Copied (", 0) == 0;
        const bool moved = message.rfind("Moved (", 0) == 0;
        if (level == "info" && (batch.move ? moved : copied)) Complete(path, now, false);

        const auto stats = json.find("stats");
        if (stats == json.end() || !stats->is_object()) return;
        const auto counter = [](const Json& object, const char* key) -> std::uint64_t {
            const auto value = object.find(key);
            return value != object.end() && value->is_number_unsigned() ? value->get<std::uint64_t>() : 0;
        };
        const auto traffic = counter(*stats, "bytes");
        if (traffic < previousTraffic_) trafficBase_ = traffic_;
        previousTraffic_ = traffic;
        traffic_ = traffic > (std::numeric_limits<std::uint64_t>::max)() - trafficBase_
            ? (std::numeric_limits<std::uint64_t>::max)() : trafficBase_ + traffic;
        engineErrors_ = counter(*stats, "errors") > 0;
        active_.clear();
        const auto transferring = stats->find("transferring");
        if (transferring != stats->end() && transferring->is_array()) for (const auto& item : *transferring) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            const auto name = item["name"].get<std::string>();
            const auto file = batch.files.find(name);
            if (file != batch.files.end() && !file->second.complete)
                active_[name] = (std::min)(counter(item, "bytes"), file->second.bytes);
        }
        Sample(now);
    }

    void EndBatch(bool success, double now) {
        // --update can legitimately skip a planned file that changed meanwhile.
        // Resolve such work only on exit code 0; do not call it a copied file.
        if (success) {
            for (const auto& file : batches_.at(current_).files) Complete(file.first, now, !batches_.at(current_).move);
            failed_.clear();
            engineErrors_ = false;
        }
        active_.clear();
        Sample(now);
    }

    Display Format(double now) {
        Sample(now);
        Display display;
        auto currentBytes = completedBytes_;
        for (const auto& active : active_) currentBytes += active.second;
        display.percent = (std::min)(99, MigrationPercent(currentBytes, totalBytes_, completedFiles_, totalFiles_));
        display.text = std::to_wstring(display.percent) + L" % — " + std::to_wstring(completedFiles_) + L" / " +
            std::to_wstring(totalFiles_) + L" files processed — " + FormatBytes(currentBytes) + L" / " + FormatBytes(totalBytes_);
        const bool retrying = engineErrors_ || !failed_.empty();
        const bool stalled = now - lastAdvance_ >= 15;
        if (retrying) display.details = googleQuota_ ? L"Retrying after a Google Drive quota limit — ETA unavailable." :
            L"Retrying failed transfers — ETA unavailable.";
        else if (completedFiles_ == totalFiles_) display.details = L"Finishing — waiting for confirmation.";
        else if (stalled) display.details = L"Waiting for transfer progress — ETA unavailable.";
        else {
            // Require sustained, recent logical progress. Raw speed/ETA and
            // totalBytes are intentionally never used to predict completion.
            if (samples_.size() >= 2 && samples_.back().time - samples_.front().time >= 30 &&
                now - lastAdvance_ < 5) {
                const auto& first = samples_.front();
                const auto& last = samples_.back();
                const double rate = (last.bytes - first.bytes) / (last.time - first.time);
                const double remaining = static_cast<double>(totalBytes_) - last.bytes;
                if (rate > 0 && remaining > 0) {
                    const double eta = remaining / rate;
                    if (std::isfinite(eta) && eta <= 7 * 24 * 3600) display.eta = eta;
                }
            }
            if (display.eta < 0) display.details = L"Copying — estimating time remaining…";
            else {
                // Avoid a seconds-precise promise for a variable cloud transfer.
                const double rounded = (std::max)(10.0, std::ceil(display.eta / 10.0) * 10.0);
                display.details = L"Copying — about " + FormatEta(rounded).substr(4) + L" remaining for this plan.";
            }
        }
        display.details += L"\r\nTraffic (includes retries): " + FormatBytes(traffic_);
        if (skippedFiles_) display.details += L". Skipped by transfer rules: " + std::to_wstring(skippedFiles_) + L".";
        return display;
    }

    std::uint64_t TotalBytes() const { return totalBytes_; }
    std::uint64_t CompletedBytes() const { return completedBytes_; }
    size_t TotalFiles() const { return totalFiles_; }
    size_t CompletedFiles() const { return completedFiles_; }
    size_t CopiedFiles() const { return completedFiles_ - skippedFiles_; }
    size_t SkippedFiles() const { return skippedFiles_; }

private:
    struct Batch { std::map<std::string, File> files; bool move = false; };
    struct SamplePoint { double time, bytes; };
    std::vector<Batch> batches_;
    size_t current_ = (std::numeric_limits<size_t>::max)();
    size_t totalFiles_ = 0, completedFiles_ = 0, skippedFiles_ = 0;
    std::uint64_t totalBytes_ = 0, completedBytes_ = 0, traffic_ = 0, trafficBase_ = 0, previousTraffic_ = 0;
    std::map<std::string, std::uint64_t> active_;
    std::set<std::string> failed_;
    std::deque<SamplePoint> samples_;
    double lastAdvance_ = 0, lastLogical_ = 0;
    bool engineErrors_ = false, googleQuota_ = false;

    void Complete(const std::string& path, double now, bool skipped) {
        auto& files = batches_.at(current_).files;
        const auto found = files.find(path);
        if (found == files.end() || found->second.complete) return;
        found->second.complete = true;
        completedBytes_ += found->second.bytes;
        ++completedFiles_;
        skippedFiles_ += skipped;
        failed_.erase(path);
        active_.erase(path);
        lastAdvance_ = now;
        if (skipped) samples_.clear();
    }

    void Sample(double now) {
        double logical = static_cast<double>(completedBytes_);
        for (const auto& active : active_) logical += static_cast<double>(active.second);
        if (logical < lastLogical_ || now - lastAdvance_ >= 15) samples_.clear();
        if (logical > lastLogical_) lastAdvance_ = now;
        lastLogical_ = logical;
        if (samples_.empty() || now - samples_.back().time >= 1) samples_.push_back({now, logical});
        while (samples_.size() > 1 && now - samples_.front().time > 90) samples_.pop_front();
    }
};

struct TransferBatches {
    std::map<SyncAction, size_t> actions;
    std::array<size_t, 3> conflicts{};
};

inline TransferBatches PrepareTransferProgress(const SyncAnalysis& analysis, const std::vector<SyncRow>& rows, TransferProgress& progress) {
    progress = {};
    TransferBatches batches;
    std::vector<TransferProgress::File> googleConflicts, oneDriveConflicts;
    for (const auto& row : rows) if (row.action == SyncAction::KeepBoth) {
        googleConflicts.push_back({row.path, analysis.google.at(row.path).size});
        oneDriveConflicts.push_back({row.path, analysis.oneDrive.at(row.path).size});
    }
    if (!googleConflicts.empty()) {
        batches.conflicts[0] = progress.AddBatch(googleConflicts);
        batches.conflicts[1] = progress.AddBatch(googleConflicts, true);
        batches.conflicts[2] = progress.AddBatch(oneDriveConflicts);
    }
    for (const auto action : {SyncAction::ToGoogle, SyncAction::ToOneDrive, SyncAction::DeleteGoogle, SyncAction::DeleteOneDrive}) {
        std::vector<TransferProgress::File> files;
        const bool move = action == SyncAction::DeleteGoogle || action == SyncAction::DeleteOneDrive;
        for (const auto& row : rows) if (row.action == action) files.push_back({row.path, row.bytes});
        if (!files.empty()) batches.actions[action] = progress.AddBatch(files, move);
    }
    return batches;
}

inline TransferBatches PrepareTransferProgress(const SyncAnalysis& analysis, SyncMode mode, TransferProgress& progress) {
    return PrepareTransferProgress(analysis, analysis.Plan(mode), progress);
}

} // namespace cloudnav
