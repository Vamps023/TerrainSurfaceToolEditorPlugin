#include "LandscapeSaveManager.h"

#include <cstdarg>

using namespace Unigine;

LandscapeSaveManager::LandscapeSaveManager(bool debug_logging)
    : debug_logging_(debug_logging)
{
    save_file_connection_id_ = Landscape::getEventSaveFile().connect(
        [this](const UGUID& guid, int operation_id, const char* path_new_diff, const char* path_old_diff)
        {
            onSaveFile(guid, operation_id, path_new_diff, path_old_diff);
        });

    pre_world_save_connection_id_ = World::getEventPreWorldSave().connect(
        [this](const char*)
        {
            forceFlush();
        });
}

LandscapeSaveManager::~LandscapeSaveManager()
{
    if (save_file_connection_id_)
    {
        Landscape::getEventSaveFile().disconnect(save_file_connection_id_);
        save_file_connection_id_ = nullptr;
    }

    if (pre_world_save_connection_id_)
    {
        World::getEventPreWorldSave().disconnect(pre_world_save_connection_id_);
        pre_world_save_connection_id_ = nullptr;
    }

    pending_save_requests_.clear();
    inflight_saves_.clear();
    dirty_versions_.clear();
    dirty_guids_.clear();
    transaction_depth_ = 0;
}

void LandscapeSaveManager::beginTransaction()
{
    ++transaction_depth_;
    logDebug("[LandscapeSaveManager] Begin transaction depth=%d\n", transaction_depth_);
}

void LandscapeSaveManager::endTransaction()
{
    if (transaction_depth_ <= 0)
    {
        transaction_depth_ = 0;
        return;
    }

    --transaction_depth_;
    logDebug("[LandscapeSaveManager] End transaction depth=%d\n", transaction_depth_);
    if (transaction_depth_ == 0)
        flushPending();
}

void LandscapeSaveManager::markDirty(const UGUID& guid)
{
    if (!guid.isValid())
        return;

    const std::string key = guidKey(guid);
    if (key.empty())
        return;

    dirty_guids_[key] = guid;
    dirty_versions_[key] = ++version_counter_;
    logDebug("[LandscapeSaveManager] Mark dirty %s version=%llu\n",
             key.c_str(),
             static_cast<unsigned long long>(dirty_versions_[key]));
}

void LandscapeSaveManager::flushPending()
{
    if (transaction_depth_ > 0)
        return;

    for (const auto& [key, guid] : dirty_guids_)
    {
        if (!guid.isValid())
            continue;
        if (inflight_saves_.count(key) != 0)
            continue;

        const auto version_it = dirty_versions_.find(key);
        const std::uint64_t version = version_it != dirty_versions_.end() ? version_it->second : 0;
        queueSave(key, guid, version);
    }
}

void LandscapeSaveManager::forceFlush()
{
    transaction_depth_ = 0;
    flushPending();
}

std::string LandscapeSaveManager::guidKey(const UGUID& guid)
{
    return guid.isValid() ? std::string(guid.getString()) : std::string();
}

void LandscapeSaveManager::queueSave(const std::string& key, const UGUID& guid, std::uint64_t version)
{
    if (!guid.isValid() || key.empty() || inflight_saves_.count(key) != 0)
        return;

    const int operation_id = Landscape::generateOperationID();
    pending_save_requests_[operation_id] = PendingSaveRequest{key, version};
    inflight_saves_.insert(key);

    Landscape::asyncSaveFile(operation_id, guid);
    logDebug("[LandscapeSaveManager] Queued save %s version=%llu op=%d\n",
             key.c_str(),
             static_cast<unsigned long long>(version),
             operation_id);
}

void LandscapeSaveManager::onSaveFile(const UGUID& guid, int operation_id,
                                      const char* path_new_diff, const char* path_old_diff)
{
    UNIGINE_UNUSED(guid);
    UNIGINE_UNUSED(path_new_diff);
    UNIGINE_UNUSED(path_old_diff);

    const auto request_it = pending_save_requests_.find(operation_id);
    if (request_it == pending_save_requests_.end())
        return;

    const PendingSaveRequest request = request_it->second;
    pending_save_requests_.erase(request_it);
    inflight_saves_.erase(request.key);

    const auto version_it = dirty_versions_.find(request.key);
    if (version_it == dirty_versions_.end() || version_it->second <= request.version)
    {
        dirty_versions_.erase(request.key);
        dirty_guids_.erase(request.key);
        logDebug("[LandscapeSaveManager] Save completed %s version=%llu\n",
                 request.key.c_str(),
                 static_cast<unsigned long long>(request.version));
        return;
    }

    logDebug("[LandscapeSaveManager] Save completed for stale version %s saved=%llu current=%llu\n",
             request.key.c_str(),
             static_cast<unsigned long long>(request.version),
             static_cast<unsigned long long>(version_it->second));

    if (transaction_depth_ == 0)
    {
        const auto guid_it = dirty_guids_.find(request.key);
        if (guid_it != dirty_guids_.end())
            queueSave(request.key, guid_it->second, version_it->second);
    }
}

void LandscapeSaveManager::logDebug(const char* format, ...) const
{
    if (!debug_logging_)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log::message("%s", buffer);
}
