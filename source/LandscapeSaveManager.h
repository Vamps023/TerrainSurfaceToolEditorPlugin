#pragma once

#include <UnigineEvent.h>
#include <UnigineLog.h>
#include <UnigineObjects.h>
#include <UnigineWorld.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

class LandscapeSaveManager
{
public:
    explicit LandscapeSaveManager(bool debug_logging = false);
    ~LandscapeSaveManager();

    void beginTransaction();
    void endTransaction();
    void markDirty(const Unigine::UGUID& guid);
    void flushPending();
    void forceFlush();

private:
    struct PendingSaveRequest
    {
        std::string key;
        std::uint64_t version = 0;
    };

    static std::string guidKey(const Unigine::UGUID& guid);
    void queueSave(const std::string& key, const Unigine::UGUID& guid, std::uint64_t version);
    void onSaveFile(const Unigine::UGUID& guid, int operation_id,
                    const char* path_new_diff, const char* path_old_diff);
    void logDebug(const char* format, ...) const;

    std::unordered_map<std::string, Unigine::UGUID> dirty_guids_;
    std::unordered_map<std::string, std::uint64_t> dirty_versions_;
    std::unordered_map<int, PendingSaveRequest> pending_save_requests_;
    std::unordered_set<std::string> inflight_saves_;

    std::uint64_t version_counter_ = 0;
    int transaction_depth_ = 0;
    bool debug_logging_ = false;

    Unigine::EventConnectionId save_file_connection_id_ = nullptr;
    Unigine::EventConnectionId pre_world_save_connection_id_ = nullptr;
};
