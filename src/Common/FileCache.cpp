#include "FileCache.h"

#include <Common/randomSeed.h>
#include <Common/SipHash.h>
#include <Common/hex.h>
#include <Common/FileCacheSettings.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>
#include <pcg-random/pcg_random.hpp>
#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{
namespace ErrorCodes
{
    extern const int REMOTE_FS_OBJECT_CACHE_ERROR;
    extern const int LOGICAL_ERROR;
}

namespace
{
    String keyToStr(const IFileCache::Key & key)
    {
        return getHexUIntLowercase(key);
    }
}

IFileCache::IFileCache(
    const String & cache_base_path_,
    const FileCacheSettings & cache_settings_)
    : cache_base_path(cache_base_path_)
    , max_size(cache_settings_.max_size)
    , max_element_size(cache_settings_.max_elements)
    , max_file_segment_size(cache_settings_.max_file_segment_size)
{
}

IFileCache::Key IFileCache::hash(const String & path)
{
    return sipHash128(path.data(), path.size());
}

String IFileCache::getPathInLocalCache(const Key & key, size_t offset)
{
    auto key_str = keyToStr(key);
    return fs::path(cache_base_path) / key_str.substr(0, 3) / key_str / std::to_string(offset);
}

String IFileCache::getPathInLocalCache(const Key & key)
{
    auto key_str = keyToStr(key);
    return fs::path(cache_base_path) / key_str.substr(0, 3) / key_str;
}

bool IFileCache::isReadOnly()
{
    return !CurrentThread::isInitialized()
        || !CurrentThread::get().getQueryContext()
        || CurrentThread::getQueryId().size == 0;
}

void IFileCache::assertInitialized() const
{
    if (!is_initialized)
        throw Exception(ErrorCodes::REMOTE_FS_OBJECT_CACHE_ERROR, "Cache not initialized");
}

LRUFileCache::LRUFileCache(const String & cache_base_path_, const FileCacheSettings & cache_settings_)
    : IFileCache(cache_base_path_, cache_settings_)
    , max_stash_element_size(cache_settings_.max_elements)
    , enable_cache_hits_threshold(cache_settings_.enable_cache_hits_threshold)
    , log(&Poco::Logger::get("LRUFileCache"))
{
}

void LRUFileCache::initialize()
{
    std::lock_guard cache_lock(mutex);
    if (!is_initialized)
    {
        if (fs::exists(cache_base_path))
            loadCacheInfoIntoMemory(cache_lock);
        else
            fs::create_directories(cache_base_path);
        is_initialized = true;
    }
}

void LRUFileCache::useCell(
    const FileSegmentCell & cell, FileSegments & result, std::lock_guard<std::mutex> & cache_lock)
{
    auto file_segment = cell.file_segment;

    if (file_segment->isDownloaded()
        && fs::file_size(getPathInLocalCache(file_segment->key(), file_segment->offset())) == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Cannot have zero size downloaded file segments. Current file segment: {}",
                        file_segment->range().toString());

    result.push_back(cell.file_segment);

    /**
     * A cell receives a queue iterator on first successful space reservation attempt
     * (space is reserved incrementally on each read buffer nextImpl() call).
     */
    if (cell.queue_iterator)
    {
        /// Move to the end of the queue. The iterator remains valid.
        queue.moveToEnd(*cell.queue_iterator, cache_lock);
    }
}

LRUFileCache::FileSegmentCell * LRUFileCache::getCell(
    const Key & key, size_t offset, std::lock_guard<std::mutex> & /* cache_lock */)
{
    auto it = files.find(key);
    if (it == files.end())
        return nullptr;

    auto & offsets = it->second;
    auto cell_it = offsets.find(offset);
    if (cell_it == offsets.end())
        return nullptr;

    return &cell_it->second;
}

FileSegments LRUFileCache::getImpl(
    const Key & key, const FileSegment::Range & range, std::lock_guard<std::mutex> & cache_lock)
{
    /// Given range = [left, right] and non-overlapping ordered set of file segments,
    /// find list [segment1, ..., segmentN] of segments which intersect with given range.

    auto it = files.find(key);
    if (it == files.end())
        return {};

    const auto & file_segments = it->second;
    if (file_segments.empty())
    {
        auto key_path = getPathInLocalCache(key);

        files.erase(key);

        if (fs::exists(key_path))
            fs::remove(key_path);

        return {};
    }

    FileSegments result;
    auto segment_it = file_segments.lower_bound(range.left);
    if (segment_it == file_segments.end())
    {
        /// N - last cached segment for given file key, segment{N}.offset < range.left:
        ///   segment{N}                       segment{N}
        /// [________                         [_______]
        ///     [__________]         OR                  [________]
        ///     ^                                        ^
        ///     range.left                               range.left

        const auto & cell = file_segments.rbegin()->second;
        if (cell.file_segment->range().right < range.left)
            return {};

        useCell(cell, result, cache_lock);
    }
    else /// segment_it <-- segmment{k}
    {
        if (segment_it != file_segments.begin())
        {
            const auto & prev_cell = std::prev(segment_it)->second;
            const auto & prev_cell_range = prev_cell.file_segment->range();

            if (range.left <= prev_cell_range.right)
            {
                ///   segment{k-1}  segment{k}
                ///   [________]   [_____
                ///       [___________
                ///       ^
                ///       range.left

                useCell(prev_cell, result, cache_lock);
            }
        }

        ///  segment{k} ...       segment{k-1}  segment{k}                      segment{k}
        ///  [______              [______]     [____                        [________
        ///  [_________     OR              [________      OR    [______]   ^
        ///  ^                              ^                           ^   segment{k}.offset
        ///  range.left                     range.left                  range.right

        while (segment_it != file_segments.end())
        {
            const auto & cell = segment_it->second;
            if (range.right < cell.file_segment->range().left)
                break;

            useCell(cell, result, cache_lock);
            ++segment_it;
        }
    }

    return result;
}

FileSegments LRUFileCache::splitRangeIntoCells(
    const Key & key, size_t offset, size_t size, FileSegment::State state, std::lock_guard<std::mutex> & cache_lock)
{
    assert(size > 0);

    auto current_pos = offset;
    auto end_pos_non_included = offset + size;

    size_t current_cell_size;
    size_t remaining_size = size;

    FileSegments file_segments;
    while (current_pos < end_pos_non_included)
    {
        current_cell_size = std::min(remaining_size, max_file_segment_size);
        remaining_size -= current_cell_size;

        auto * cell = addCell(key, current_pos, current_cell_size, state, cache_lock);
        if (cell)
            file_segments.push_back(cell->file_segment);
        assert(cell);

        current_pos += current_cell_size;
    }

    assert(file_segments.empty() || offset + size - 1 == file_segments.back()->range().right);
    return file_segments;
}

void LRUFileCache::fillHolesWithEmptyFileSegments(
    FileSegments & file_segments,
    const Key & key,
    const FileSegment::Range & range,
    bool fill_with_detached_file_segments,
    std::lock_guard<std::mutex> & cache_lock)
{
    /// There are segments [segment1, ..., segmentN]
    /// (non-overlapping, non-empty, ascending-ordered) which (maybe partially)
    /// intersect with given range.

    /// It can have holes:
    /// [____________________]         -- requested range
    ///     [____]  [_]   [_________]  -- intersecting cache [segment1, ..., segmentN]
    ///
    /// For each such hole create a cell with file segment state EMPTY.

    auto it = file_segments.begin();
    auto segment_range = (*it)->range();

    size_t current_pos;
    if (segment_range.left < range.left)
    {
        ///    [_______     -- requested range
        /// [_______
        /// ^
        /// segment1

        current_pos = segment_range.right + 1;
        ++it;
    }
    else
        current_pos = range.left;

    while (current_pos <= range.right && it != file_segments.end())
    {
        segment_range = (*it)->range();

        if (current_pos == segment_range.left)
        {
            current_pos = segment_range.right + 1;
            ++it;
            continue;
        }

        assert(current_pos < segment_range.left);

        auto hole_size = segment_range.left - current_pos;

        if (fill_with_detached_file_segments)
        {
            auto file_segment = std::make_shared<FileSegment>(current_pos, hole_size, key, this, FileSegment::State::EMPTY);
            {
                std::lock_guard segment_lock(file_segment->mutex);
                file_segment->markAsDetached(segment_lock);
            }
            file_segments.insert(it, file_segment);
        }
        else
        {
            file_segments.splice(it, splitRangeIntoCells(key, current_pos, hole_size, FileSegment::State::EMPTY, cache_lock));
        }

        current_pos = segment_range.right + 1;
        ++it;
    }

    if (current_pos <= range.right)
    {
        ///   ________]     -- requested range
        ///   _____]
        ///        ^
        /// segmentN

        auto hole_size = range.right - current_pos + 1;

        if (fill_with_detached_file_segments)
        {
            auto file_segment = std::make_shared<FileSegment>(current_pos, hole_size, key, this, FileSegment::State::EMPTY);
            {
                std::lock_guard segment_lock(file_segment->mutex);
                file_segment->markAsDetached(segment_lock);
            }
            file_segments.insert(file_segments.end(), file_segment);
        }
        else
        {
            file_segments.splice(
                file_segments.end(), splitRangeIntoCells(key, current_pos, hole_size, FileSegment::State::EMPTY, cache_lock));
        }
    }
}

FileSegmentsHolder LRUFileCache::getOrSet(const Key & key, size_t offset, size_t size)
{
    assertInitialized();

    FileSegment::Range range(offset, offset + size - 1);

    std::lock_guard cache_lock(mutex);

#ifndef NDEBUG
    assertCacheCorrectness(key, cache_lock);
#endif

    /// Get all segments which intersect with the given range.
    auto file_segments = getImpl(key, range, cache_lock);

    if (file_segments.empty())
    {
        file_segments = splitRangeIntoCells(key, offset, size, FileSegment::State::EMPTY, cache_lock);
    }
    else
    {
        fillHolesWithEmptyFileSegments(file_segments, key, range, false, cache_lock);
    }

    assert(!file_segments.empty());
    return FileSegmentsHolder(std::move(file_segments));
}

FileSegmentsHolder LRUFileCache::get(const Key & key, size_t offset, size_t size)
{
    assertInitialized();

    FileSegment::Range range(offset, offset + size - 1);

    std::lock_guard cache_lock(mutex);

#ifndef NDEBUG
    assertCacheCorrectness(key, cache_lock);
#endif

    /// Get all segments which intersect with the given range.
    auto file_segments = getImpl(key, range, cache_lock);

    if (file_segments.empty())
    {
        auto file_segment = std::make_shared<FileSegment>(offset, size, key, this, FileSegment::State::EMPTY);
        {
            std::lock_guard segment_lock(file_segment->mutex);
            file_segment->markAsDetached(segment_lock);
        }
        file_segments = { file_segment };
    }
    else
    {
        fillHolesWithEmptyFileSegments(file_segments, key, range, true, cache_lock);
    }

    return FileSegmentsHolder(std::move(file_segments));
}

LRUFileCache::FileSegmentCell * LRUFileCache::addCell(
    const Key & key, size_t offset, size_t size, FileSegment::State state,
    std::lock_guard<std::mutex> & cache_lock)
{
    /// Create a file segment cell and put it in `files` map by [key][offset].

    if (!size)
        return nullptr; /// Empty files are not cached.

    if (files[key].contains(offset))
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Cache already exists for key: `{}`, offset: {}, size: {}.\nCurrent cache structure: {}",
            keyToStr(key), offset, size, dumpStructureUnlocked(key, cache_lock));

    auto skip_or_download = [&]() -> FileSegmentPtr
    {
        if (state == FileSegment::State::EMPTY && enable_cache_hits_threshold)
        {
            auto record = records.find({key, offset});

            if (record == records.end())
            {
                auto queue_iter = stash_queue.add(key, offset, 0, cache_lock);
                records.insert({{key, offset}, queue_iter});

                if (stash_queue.getElementsNum(cache_lock) > max_stash_element_size)
                {
                    auto remove_queue_iter = stash_queue.begin();
                    records.erase({remove_queue_iter->key, remove_queue_iter->offset});
                    stash_queue.remove(remove_queue_iter, cache_lock);
                }

                /// For segments that do not reach the download threshold, we do not download them, but directly read them
                return std::make_shared<FileSegment>(offset, size, key, this, FileSegment::State::SKIP_CACHE);
            }
            else
            {
                auto queue_iter = record->second;
                queue_iter->hits++;
                stash_queue.moveToEnd(queue_iter, cache_lock);

                state = queue_iter->hits >= enable_cache_hits_threshold ? FileSegment::State::EMPTY : FileSegment::State::SKIP_CACHE;
                return std::make_shared<FileSegment>(offset, size, key, this, state);
            }
        }
        else
            return std::make_shared<FileSegment>(offset, size, key, this, state);
    };

    FileSegmentCell cell(skip_or_download(), this, cache_lock);
    auto & offsets = files[key];

    if (offsets.empty())
    {
        auto key_path = getPathInLocalCache(key);
        if (!fs::exists(key_path))
            fs::create_directories(key_path);
    }

    auto [it, inserted] = offsets.insert({offset, std::move(cell)});
    if (!inserted)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Failed to insert into cache key: `{}`, offset: {}, size: {}",
            keyToStr(key), offset, size);

    return &(it->second);
}

FileSegmentsHolder LRUFileCache::setDownloading(const Key & key, size_t offset, size_t size)
{
    std::lock_guard cache_lock(mutex);

#ifndef NDEBUG
    assertCacheCorrectness(key, cache_lock);
#endif

    auto * cell = getCell(key, offset, cache_lock);
    if (cell)
        throw Exception(
            ErrorCodes::REMOTE_FS_OBJECT_CACHE_ERROR,
            "Cache cell already exists for key `{}` and offset {}",
            keyToStr(key), offset);

    auto file_segments = splitRangeIntoCells(key, offset, size, FileSegment::State::DOWNLOADING, cache_lock);
    return FileSegmentsHolder(std::move(file_segments));
}

bool LRUFileCache::tryReserve(
    const Key & key, size_t offset, size_t size, std::lock_guard<std::mutex> & cache_lock)
{
    auto removed_size = 0;
    size_t queue_size = queue.getElementsNum(cache_lock);
    assert(queue_size <= max_element_size);

    /// Since space reservation is incremental, cache cell already exists if it's state is EMPTY.
    /// And it cache cell does not exist on startup -- as we first check for space and then add a cell.
    auto * cell_for_reserve = getCell(key, offset, cache_lock);

    /// A cell acquires a LRUQueue iterator on first successful space reservation attempt.
    /// cell_for_reserve can be nullptr here when we call tryReserve() from loadCacheInfoIntoMemory().
    if (!cell_for_reserve || !cell_for_reserve->queue_iterator)
        queue_size += 1;

    auto is_overflow = [&]
    {
        /// max_size == 0 means unlimited cache size, max_element_size means unlimited number of cache elements.
        return (max_size != 0 && queue.getTotalWeight(cache_lock) + size - removed_size > max_size)
            || (max_element_size != 0 && queue_size > max_element_size);
    };

    std::vector<FileSegmentCell *> to_evict;
    std::vector<FileSegmentCell *> trash;

    for (const auto & [entry_key, entry_offset, entry_size, _] : queue)
    {
        if (!is_overflow())
            break;

        auto * cell = getCell(entry_key, entry_offset, cache_lock);
        if (!cell)
            throw Exception(
                ErrorCodes::REMOTE_FS_OBJECT_CACHE_ERROR,
                "Cache became inconsistent. Key: {}, offset: {}",
                keyToStr(key), offset);

        size_t cell_size = cell->size();
        assert(entry_size == cell_size);

        /// It is guaranteed that cell is not removed from cache as long as
        /// pointer to corresponding file segment is hold by any other thread.

        if (cell->releasable())
        {
            auto & file_segment = cell->file_segment;

            std::lock_guard segment_lock(file_segment->mutex);

            switch (file_segment->download_state)
            {
                case FileSegment::State::DOWNLOADED:
                {
                    /// Cell will actually be removed only if
                    /// we managed to reserve enough space.

                    to_evict.push_back(cell);
                    break;
                }
                default:
                {
                    trash.push_back(cell);
                    break;
                }
            }

            removed_size += cell_size;
            --queue_size;
        }
    }


    /// This case is very unlikely, can happen in case of exception from
    /// file_segment->complete(), which would be a logical error.
    assert(trash.empty());
    for (auto & cell : trash)
    {
        auto file_segment = cell->file_segment;
        if (file_segment)
        {
            std::lock_guard segment_lock(file_segment->mutex);
            remove(file_segment->key(), file_segment->offset(), cache_lock, segment_lock);
        }
    }

    if (is_overflow())
        return false;

    /// cache cell is nullptr on server startup because we first check for space and then add a cell.
    if (cell_for_reserve)
    {
        /// queue_iteratir is std::nullopt here if no space has been reserved yet, a cache cell
        /// acquires queue iterator on first successful space reservation attempt.
        /// If queue iterator already exists, we need to update the size after each space reservation.
        auto queue_iterator = cell_for_reserve->queue_iterator;
        if (queue_iterator)
            queue.incrementSize(*queue_iterator, size, cache_lock);
        else
            cell_for_reserve->queue_iterator = queue.add(key, offset, size, cache_lock);
    }

    for (auto & cell : to_evict)
    {
        auto file_segment = cell->file_segment;
        if (file_segment)
        {
            std::lock_guard<std::mutex> segment_lock(file_segment->mutex);
            remove(file_segment->key(), file_segment->offset(), cache_lock, segment_lock);
        }
    }

    if (queue.getTotalWeight(cache_lock) > (1ull << 63))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cache became inconsistent. There must be a bug");

    return true;
}

void LRUFileCache::remove(const Key & key)
{
    assertInitialized();

    std::lock_guard cache_lock(mutex);

    auto it = files.find(key);
    if (it == files.end())
        return;

    auto & offsets = it->second;

    std::vector<FileSegmentCell *> to_remove;
    to_remove.reserve(offsets.size());

    for (auto & [offset, cell] : offsets)
        to_remove.push_back(&cell);

    for (auto & cell : to_remove)
    {
        if (!cell->releasable())
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Cannot remove file from cache because someone reads from it. File segment info: {}",
                cell->file_segment->getInfoForLog());

        auto file_segment = cell->file_segment;
        if (file_segment)
        {
            std::lock_guard<std::mutex> segment_lock(file_segment->mutex);
            remove(file_segment->key(), file_segment->offset(), cache_lock, segment_lock);
        }
    }

    auto key_path = getPathInLocalCache(key);

    files.erase(key);

    if (fs::exists(key_path))
        fs::remove(key_path);
}

void LRUFileCache::remove()
{
    /// Try remove all cached files by cache_base_path.
    /// Only releasable file segments are evicted.

    std::lock_guard cache_lock(mutex);

    std::vector<FileSegment *> to_remove;
    for (auto it = queue.begin(); it != queue.end();)
    {
        const auto & [key, offset, size, _] = *it++;
        auto * cell = getCell(key, offset, cache_lock);
        if (!cell)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Cache is in inconsistent state: LRU queue contains entries with no cache cell");

        if (cell->releasable())
        {
            auto file_segment = cell->file_segment;
            if (file_segment)
            {
                std::lock_guard segment_lock(file_segment->mutex);
                file_segment->detach(cache_lock, segment_lock);
                remove(file_segment->key(), file_segment->offset(), cache_lock, segment_lock);
            }
        }
    }

    /// Remove all access information.
    records.clear();
    stash_queue.removeAll(cache_lock);
}

void LRUFileCache::remove(
    Key key, size_t offset,
    std::lock_guard<std::mutex> & cache_lock, std::lock_guard<std::mutex> & /* segment_lock */)
{
    LOG_TEST(log, "Remove. Key: {}, offset: {}", keyToStr(key), offset);

    auto * cell = getCell(key, offset, cache_lock);
    if (!cell)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "No cache cell for key: {}, offset: {}", keyToStr(key), offset);

    if (cell->queue_iterator)
    {
        queue.remove(*cell->queue_iterator, cache_lock);
    }

    auto & offsets = files[key];
    offsets.erase(offset);

    auto cache_file_path = getPathInLocalCache(key, offset);
    if (fs::exists(cache_file_path))
    {
        try
        {
            fs::remove(cache_file_path);

            if (is_initialized && offsets.empty())
            {
                auto key_path = getPathInLocalCache(key);

                files.erase(key);

                if (fs::exists(key_path))
                    fs::remove(key_path);
            }
        }
        catch (...)
        {
            throw Exception(ErrorCodes::REMOTE_FS_OBJECT_CACHE_ERROR,
                            "Removal of cached file failed. Key: {}, offset: {}, path: {}, error: {}",
                            keyToStr(key), offset, cache_file_path, getCurrentExceptionMessage(false));
        }
    }
}

void LRUFileCache::loadCacheInfoIntoMemory(std::lock_guard<std::mutex> & cache_lock)
{
    Key key;
    UInt64 offset = 0;
    size_t size = 0;
    std::vector<std::pair<LRUQueue::Iterator, std::weak_ptr<FileSegment>>> queue_entries;

    /// cache_base_path / key_prefix / key / offset

    fs::directory_iterator key_prefix_it{cache_base_path};
    for (; key_prefix_it != fs::directory_iterator(); ++key_prefix_it)
    {
        fs::directory_iterator key_it{key_prefix_it->path()};
        for (; key_it != fs::directory_iterator(); ++key_it)
        {
            key = unhexUInt<UInt128>(key_it->path().filename().string().data());

            fs::directory_iterator offset_it{key_it->path()};
            for (; offset_it != fs::directory_iterator(); ++offset_it)
            {
                bool parsed = tryParse<UInt64>(offset, offset_it->path().filename());
                if (!parsed)
                {
                    LOG_WARNING(log, "Unexpected file: ", offset_it->path().string());
                    continue; /// Or just remove? Some unexpected file.
                }

                size = offset_it->file_size();
                if (!size)
                {
                    fs::remove(offset_it->path());
                    continue;
                }

                if (tryReserve(key, offset, size, cache_lock))
                {
                    auto * cell = addCell(key, offset, size, FileSegment::State::DOWNLOADED, cache_lock);
                    if (cell)
                        queue_entries.emplace_back(*cell->queue_iterator, cell->file_segment);
                }
                else
                {
                    LOG_WARNING(log,
                                "Cache capacity changed (max size: {}, available: {}), cached file `{}` does not fit in cache anymore (size: {})",
                                max_size, getAvailableCacheSizeUnlocked(cache_lock), key_it->path().string(), size);
                    fs::remove(offset_it->path());
                }
            }
        }
    }

    /// Shuffle cells to have random order in LRUQueue as at startup all cells have the same priority.
    pcg64 generator(randomSeed());
    std::shuffle(queue_entries.begin(), queue_entries.end(), generator);
    for (const auto & [it, file_segment] : queue_entries)
    {
        /// Cell cache size changed and, for example, 1st file segment fits into cache
        /// and 2nd file segment will fit only if first was evicted, then first will be removed and
        /// cell is nullptr here.
        if (file_segment.expired())
            continue;

        queue.moveToEnd(it, cache_lock);
    }
#ifndef NDEBUG
    assertCacheCorrectness(cache_lock);
#endif
}

void LRUFileCache::reduceSizeToDownloaded(
    const Key & key, size_t offset,
    std::lock_guard<std::mutex> & cache_lock, std::lock_guard<std::mutex> & /* segment_lock */)
{
    /**
     * In case file was partially downloaded and it's download cannot be continued
     * because of no space left in cache, we need to be able to cut cell's size to downloaded_size.
     */

    auto * cell = getCell(key, offset, cache_lock);

    if (!cell)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "No cell found for key: {}, offset: {}",
            keyToStr(key), offset);
    }

    const auto & file_segment = cell->file_segment;

    size_t downloaded_size = file_segment->downloaded_size;
    if (downloaded_size == file_segment->range().size())
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Nothing to reduce, file segment fully downloaded, key: {}, offset: {}",
            keyToStr(key), offset);
    }

    cell->file_segment = std::make_shared<FileSegment>(offset, downloaded_size, key, this, FileSegment::State::DOWNLOADED);
}

bool LRUFileCache::isLastFileSegmentHolder(
    const Key & key, size_t offset,
    std::lock_guard<std::mutex> & cache_lock, std::lock_guard<std::mutex> & /* segment_lock */)
{
    auto * cell = getCell(key, offset, cache_lock);

    if (!cell)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "No cell found for key: {}, offset: {}", keyToStr(key), offset);

    /// The caller of this method is last file segment holder if use count is 2 (the second pointer is cache itself)
    return cell->file_segment.use_count() == 2;
}

FileSegments LRUFileCache::getSnapshot() const
{
    std::lock_guard cache_lock(mutex);

    FileSegments file_segments;

    for (const auto & [key, cells_by_offset] : files)
    {
        for (const auto & [offset, cell] : cells_by_offset)
            file_segments.push_back(FileSegment::getSnapshot(cell.file_segment, cache_lock));
    }

    return file_segments;
}

std::vector<String> LRUFileCache::tryGetCachePaths(const Key & key)
{
    std::lock_guard cache_lock(mutex);

    std::vector<String> cache_paths;

    const auto & cells_by_offset = files[key];

    for (const auto & [offset, cell] : cells_by_offset)
    {
        if (cell.file_segment->state() == FileSegment::State::DOWNLOADED)
            cache_paths.push_back(getPathInLocalCache(key, offset));
    }

    return cache_paths;
}

size_t LRUFileCache::getUsedCacheSize() const
{
    std::lock_guard cache_lock(mutex);
    return getUsedCacheSizeUnlocked(cache_lock);
}

size_t LRUFileCache::getUsedCacheSizeUnlocked(std::lock_guard<std::mutex> & cache_lock) const
{
    return queue.getTotalWeight(cache_lock);
}

size_t LRUFileCache::getAvailableCacheSize() const
{
    std::lock_guard cache_lock(mutex);
    return getAvailableCacheSizeUnlocked(cache_lock);
}

size_t LRUFileCache::getAvailableCacheSizeUnlocked(std::lock_guard<std::mutex> & cache_lock) const
{
    return max_size - getUsedCacheSizeUnlocked(cache_lock);
}

size_t LRUFileCache::getFileSegmentsNum() const
{
    std::lock_guard cache_lock(mutex);
    return getFileSegmentsNumUnlocked(cache_lock);
}

size_t LRUFileCache::getFileSegmentsNumUnlocked(std::lock_guard<std::mutex> & cache_lock) const
{
    return queue.getElementsNum(cache_lock);
}

LRUFileCache::FileSegmentCell::FileSegmentCell(
    FileSegmentPtr file_segment_,
    LRUFileCache * cache,
    std::lock_guard<std::mutex> & cache_lock)
    : file_segment(file_segment_)
{
    /**
     * Cell can be created with either DOWNLOADED or EMPTY file segment's state.
     * File segment acquires DOWNLOADING state and creates LRUQueue iterator on first
     * successful getOrSetDownaloder call.
     */

    switch (file_segment->download_state)
    {
        case FileSegment::State::DOWNLOADED:
        {
            queue_iterator = cache->queue.add(file_segment->key(), file_segment->offset(), file_segment->range().size(), cache_lock);
            break;
        }
        case FileSegment::State::SKIP_CACHE:
        case FileSegment::State::EMPTY:
        case FileSegment::State::DOWNLOADING:
        {
            break;
        }
        default:
            throw Exception(ErrorCodes::REMOTE_FS_OBJECT_CACHE_ERROR,
                            "Can create cell with either EMPTY, DOWNLOADED, DOWNLOADING state, got: {}",
                            FileSegment::stateToString(file_segment->download_state));
    }
}

LRUFileCache::LRUQueue::Iterator LRUFileCache::LRUQueue::add(
    const IFileCache::Key & key, size_t offset, size_t size, std::lock_guard<std::mutex> & /* cache_lock */)
{
#ifndef NDEBUG
    for (const auto & [entry_key, entry_offset, entry_size, entry_hits] : queue)
    {
        if (entry_key == key && entry_offset == offset)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Attempt to add duplicate queue entry to queue. (Key: {}, offset: {}, size: {})",
                keyToStr(key), offset, size);
    }
#endif

    cache_size += size;
    return queue.insert(queue.end(), FileKeyAndOffset(key, offset, size));
}

void LRUFileCache::LRUQueue::remove(Iterator queue_it, std::lock_guard<std::mutex> & /* cache_lock */)
{
    cache_size -= queue_it->size;
    queue.erase(queue_it);
}

void LRUFileCache::LRUQueue::removeAll(std::lock_guard<std::mutex> & /* cache_lock */)
{
    queue.clear();
    cache_size = 0;
}

void LRUFileCache::LRUQueue::moveToEnd(Iterator queue_it, std::lock_guard<std::mutex> & /* cache_lock */)
{
    queue.splice(queue.end(), queue, queue_it);
}

void LRUFileCache::LRUQueue::incrementSize(Iterator queue_it, size_t size_increment, std::lock_guard<std::mutex> & /* cache_lock */)
{
    cache_size += size_increment;
    queue_it->size += size_increment;
}

bool LRUFileCache::LRUQueue::contains(
    const IFileCache::Key & key, size_t offset, std::lock_guard<std::mutex> & /* cache_lock */) const
{
    /// This method is used for assertions in debug mode.
    /// So we do not care about complexity here.
    for (const auto & [entry_key, entry_offset, size, _] : queue)
    {
        if (key == entry_key && offset == entry_offset)
            return true;
    }
    return false;
}

void LRUFileCache::LRUQueue::assertCorrectness(LRUFileCache * cache, std::lock_guard<std::mutex> & cache_lock)
{
    [[maybe_unused]] size_t total_size = 0;
    for (auto it = queue.begin(); it != queue.end();)
    {
        auto & [key, offset, size, _] = *it++;

        auto * cell = cache->getCell(key, offset, cache_lock);
        if (!cell)
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Cache is in inconsistent state: LRU queue contains entries with no cache cell (assertCorrectness())");
        }

        assert(cell->size() == size);
        total_size += size;
    }

    assert(total_size == cache_size);
    assert(cache_size <= cache->max_size);
    assert(queue.size() <= cache->max_element_size);
}

String LRUFileCache::LRUQueue::toString(std::lock_guard<std::mutex> & /* cache_lock */) const
{
    String result;
    for (const auto & [key, offset, size, _] : queue)
    {
        if (!result.empty())
            result += ", ";
        result += fmt::format("{}: [{}, {}]", keyToStr(key), offset, offset + size - 1);
    }
    return result;
}

String LRUFileCache::dumpStructure(const Key & key)
{
    std::lock_guard cache_lock(mutex);
    return dumpStructureUnlocked(key, cache_lock);
}

String LRUFileCache::dumpStructureUnlocked(const Key & key, std::lock_guard<std::mutex> & cache_lock)
{
    WriteBufferFromOwnString result;
    const auto & cells_by_offset = files[key];

    for (const auto & [offset, cell] : cells_by_offset)
        result << cell.file_segment->getInfoForLog() << "\n";

    result << "\n\nQueue: " << queue.toString(cache_lock);
    return result.str();
}

void LRUFileCache::assertCacheCellsCorrectness(
    const FileSegmentsByOffset & cells_by_offset, [[maybe_unused]] std::lock_guard<std::mutex> & cache_lock)
{
    for (const auto & [_, cell] : cells_by_offset)
    {
        const auto & file_segment = cell.file_segment;
        file_segment->assertCorrectness();

        if (file_segment->reserved_size != 0)
        {
            assert(cell.queue_iterator);
            assert(queue.contains(file_segment->key(), file_segment->offset(), cache_lock));
        }
    }
}

void LRUFileCache::assertCacheCorrectness(const Key & key, std::lock_guard<std::mutex> & cache_lock)
{
    assertCacheCellsCorrectness(files[key], cache_lock);
    queue.assertCorrectness(this, cache_lock);
}

void LRUFileCache::assertCacheCorrectness(std::lock_guard<std::mutex> & cache_lock)
{
    for (const auto & [key, cells_by_offset] : files)
        assertCacheCellsCorrectness(files[key], cache_lock);
    queue.assertCorrectness(this, cache_lock);
}

}
