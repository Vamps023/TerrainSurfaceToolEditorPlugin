#include "LandscapeSaveManager.h"

#include <cstdarg>

using namespace Unigine;

LandscapeSaveManager::LandscapeSaveManager(bool debugLogging)
    : debugLogging(debugLogging)
{
    Landscape::getEventSaveFile().connect(saveFileConnection,
        [this](const UGUID& guid, int operationId, const char* pathNewDiff, const char* pathOldDiff)
        {
            onSaveFile(guid, operationId, pathNewDiff, pathOldDiff);
        });

    World::getEventPreWorldSave().connect(preWorldSaveConnection,
        [this](const char*)
        {
            forceFlush();
        });
}

LandscapeSaveManager::~LandscapeSaveManager()
{
    saveFileConnection.disconnect();
    preWorldSaveConnection.disconnect();

    pendingSaveRequests.clear();
    inflightSaves.clear();
    dirtyVersions.clear();
    dirtyGuids.clear();
    transactionDepth = 0;
}

void LandscapeSaveManager::beginTransaction()
{
    ++transactionDepth;
    logDebug("[LandscapeSaveManager] Begin transaction depth=%d\n", transactionDepth);
}

void LandscapeSaveManager::endTransaction()
{
    if (transactionDepth <= 0)
    {
        transactionDepth = 0;
        return;
    }

    --transactionDepth;
    logDebug("[LandscapeSaveManager] End transaction depth=%d\n", transactionDepth);
    if (transactionDepth == 0)
        flushPending();
}

void LandscapeSaveManager::markDirty(const UGUID& guid)
{
    if (!guid.isValid())
        return;

    const std::string key = guidKey(guid);
    if (key.empty())
        return;

    dirtyGuids[key] = guid;
    dirtyVersions[key] = ++versionCounter;
    logDebug("[LandscapeSaveManager] Mark dirty %s version=%llu\n",
             key.c_str(),
             static_cast<unsigned long long>(dirtyVersions[key]));
}

void LandscapeSaveManager::flushPending()
{
    if (transactionDepth > 0)
        return;

    for (const auto& [key, guid] : dirtyGuids)
    {
        if (!guid.isValid())
            continue;
        if (inflightSaves.count(key) != 0)
            continue;

        const auto versionIt = dirtyVersions.find(key);
        const std::uint64_t version = versionIt != dirtyVersions.end() ? versionIt->second : 0;
        queueSave(key, guid, version);
    }
}

void LandscapeSaveManager::forceFlush()
{
    transactionDepth = 0;
    flushPending();
}

std::string LandscapeSaveManager::guidKey(const UGUID& guid)
{
    return guid.isValid() ? std::string(guid.getString()) : std::string();
}

void LandscapeSaveManager::queueSave(const std::string& key, const UGUID& guid, std::uint64_t version)
{
    if (!guid.isValid() || key.empty() || inflightSaves.count(key) != 0)
        return;

    const int operationId = Landscape::generateOperationID();
    pendingSaveRequests[operationId] = PendingSaveRequest{key, version};
    inflightSaves.insert(key);

    Landscape::asyncSaveFile(operationId, guid);
    logDebug("[LandscapeSaveManager] Queued save %s version=%llu op=%d\n",
             key.c_str(),
             static_cast<unsigned long long>(version),
             operationId);
}

void LandscapeSaveManager::onSaveFile(const UGUID& guid, int operationId,
                                      const char* pathNewDiff, const char* pathOldDiff)
{
    UNIGINE_UNUSED(guid);
    UNIGINE_UNUSED(pathNewDiff);
    UNIGINE_UNUSED(pathOldDiff);

    const auto requestIt = pendingSaveRequests.find(operationId);
    if (requestIt == pendingSaveRequests.end())
        return;

    const PendingSaveRequest request = requestIt->second;
    pendingSaveRequests.erase(requestIt);
    inflightSaves.erase(request.key);

    const auto versionIt = dirtyVersions.find(request.key);
    if (versionIt == dirtyVersions.end() || versionIt->second <= request.version)
    {
        dirtyVersions.erase(request.key);
        dirtyGuids.erase(request.key);
        logDebug("[LandscapeSaveManager] Save completed %s version=%llu\n",
                 request.key.c_str(),
                 static_cast<unsigned long long>(request.version));
        return;
    }

    logDebug("[LandscapeSaveManager] Save completed for stale version %s saved=%llu current=%llu\n",
             request.key.c_str(),
             static_cast<unsigned long long>(request.version),
             static_cast<unsigned long long>(versionIt->second));

    if (transactionDepth == 0)
    {
        const auto guidIt = dirtyGuids.find(request.key);
        if (guidIt != dirtyGuids.end())
            queueSave(request.key, guidIt->second, versionIt->second);
    }
}

void LandscapeSaveManager::logDebug(const char* format, ...) const
{
    if (!debugLogging)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log::message("%s", buffer);
}
