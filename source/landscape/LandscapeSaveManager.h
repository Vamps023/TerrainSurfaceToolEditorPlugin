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
    explicit LandscapeSaveManager(bool debugLogging = false);
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
    void onSaveFile(const Unigine::UGUID& guid, int operationId,
                    const char* pathNewDiff, const char* pathOldDiff);
    void logDebug(const char* format, ...) const;

    std::unordered_map<std::string, Unigine::UGUID> dirtyGuids;
    std::unordered_map<std::string, std::uint64_t> dirtyVersions;
    std::unordered_map<int, PendingSaveRequest> pendingSaveRequests;
    std::unordered_set<std::string> inflightSaves;

    std::uint64_t versionCounter = 0;
    int transactionDepth = 0;
    bool debugLogging = false;

    Unigine::EventConnection saveFileConnection;
    Unigine::EventConnection preWorldSaveConnection;
};
