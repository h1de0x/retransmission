// This file Copyright © Transmission authors and contributors.
// It may be used under GPLv2 (SPDX: GPL-2.0-only) or GPLv3 (SPDX: GPL-3.0-only).
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "libtransmission/types.h"

struct tr_session;
struct tr_torrent;

/** An owning copy of a torrent's name and file subpaths. */
struct tr_torrent_path_snapshot {
    tr_torrent_id_t id = {};

    /** `tr_torrentEditRevision()` at the moment the copy was taken. */
    uint64_t edit_revision = {};

    /** The torrent's name, as `tr_torrentName()` would return it. */
    std::string name;

    /** The torrent's file subpaths in file-index order. */
    std::vector<std::string> file_subpaths;
};

/** The result of one `tr_sessionRequestTorrentPathSnapshots()` call. */
struct tr_torrent_path_snapshot_batch {
    /** How many leading entries of the request's `ids` were examined,
        including ids of torrents that no longer exist. */
    size_t ids_processed = {};

    std::vector<tr_torrent_path_snapshot> snapshots;
};

using tr_torrent_path_snapshot_func = std::function<void(tr_torrent_path_snapshot_batch&& batch)>;

/**
 * Returns a counter that changes whenever rarely-changing torrent metadata is edited.
 *
 * The counter changes on the same events that update `tr_stat::edit_date`,
 * e.g. renaming a file or folder, metainfo arriving for a magnet link,
 * or changes to trackers, labels or the download directory.
 * Unlike `edit_date`, it has no timestamp granularity:
 * two edits in the same second still produce two different values.
 *
 * The value is process-local and is not persisted between sessions.
 * Its absolute value has no meaning; only compare it with an earlier revision from the same torrent.
 *
 * The counter is atomic only so it can be compared from another thread.
 * Observing a new revision does not make other `tr_torrent` reads safe;
 * use `tr_sessionRequestTorrentPathSnapshots()` for a consistent copy.
 * The caller must still ensure that `torrent` is alive.
 */
[[nodiscard]] uint64_t tr_torrentEditRevision(tr_torrent const* torrent);

/**
 * Asynchronously copy the names and file subpaths of some torrents.
 *
 * Queues one task in the session thread.
 * The task walks `ids` in order and copies each torrent's name, file subpaths and `tr_torrentEditRevision()`
 * into a `tr_torrent_path_snapshot`.
 * It stops after examining `max_torrents` ids.
 * After at least one snapshot has been added, it also stops if the batch is already over `max_files`
 * or if copying the next torrent would exceed that limit.
 *
 * `max_files` is a soft limit. The first existing torrent in a batch is always copied in full,
 * regardless of its file count. A `max_torrents` of 0 is treated as 1.
 * Together these rules ensure that every nonempty request examines at least one id,
 * so callers can continue with `ids.subspan(batch.ids_processed)` and always make progress.
 *
 * Ids of torrents that no longer exist are counted in `ids_processed` but produce no snapshot.
 * Each snapshot is taken inside a single session-thread task under the session lock,
 * so it is consistent with edits made in the session thread and with edits made elsewhere under that lock.
 *
 * The ids that can be examined, the first `min(ids.size(), max(max_torrents, 1))` entries,
 * are copied before this function returns, so the caller may free or reuse their storage immediately.
 *
 * `callback` is invoked exactly once, in the session thread, and never synchronously in the calling thread.
 * Because the session thread runs concurrently, the callback may begin before this function returns;
 * any state the callback relies on must be set up before calling this function.
 * When `ids` is empty, the callback receives an empty batch with `ids_processed == 0`.
 * The batch owns all of its data.
 * The callback should return quickly, e.g. by handing the batch off to another thread.
 *
 * Must not be called from the session thread, including from `callback`;
 * request the next batch from another thread.
 *
 * The caller must stop making new requests before calling `tr_sessionClose()`.
 * Only requests made before `tr_sessionClose()` is called are guaranteed to have their callback invoked.
 */
void tr_sessionRequestTorrentPathSnapshots(
    tr_session* session,
    std::span<tr_torrent_id_t const> ids,
    size_t max_torrents,
    size_t max_files,
    tr_torrent_path_snapshot_func callback);
