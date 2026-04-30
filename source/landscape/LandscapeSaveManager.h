#pragma once

#include <UnigineEvent.h>
#include <UnigineLog.h>
#include <UnigineObjects.h>
#include <UnigineWorld.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Manages deferred saving of landscape tile data.
// Uses a transaction model: saves are batched between beginTransaction / endTransaction
// and dispatched via Landscape::asyncSaveFile once the transaction depth reaches zero.
class LandscapeSaveManager
{
public:
    explicit LandscapeSaveManager(bool debugLogging = false);
    ~LandscapeSaveManager();

    // Increments transaction depth. Saves are held until the matching endTransaction.
    void beginTransaction();
    // Decrements transaction depth. Flushes pending saves when depth reaches zero.
    void endTransaction();
    // Marks the landscape tile identified by guid as needing a save.
    void markDirty(const Unigine::UGUID& guid);
    // Queues async saves for all dirty tiles (no-op while inside a transaction).
    void flushPending();
    // Forces transaction depth to zero, then flushes all pending saves immediately.
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
