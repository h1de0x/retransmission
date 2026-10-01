// This file Copyright © Transmission authors and contributors.
// It may be used under GPLv2 (SPDX: GPL-2.0-only) or GPLv3 (SPDX: GPL-3.0-only).
// License text can be found in the licenses/ folder.

#pragma once

#include "Torrent.h"

#include <glibmm/dispatcher.h>
#include <glibmm/refptr.h>

#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct tr_session;
struct tr_torrent_path_snapshot;

// Builds the text searched by TorrentFilter.
// Each torrent contributes its case-folded name and file subpaths, separated by NULs.
// Work moves through three threads:
//
// main thread     queues ids and requests one bounded batch at a time
// session thread  copies names and file subpaths
// worker thread   case-folds the copies and wakes the main thread
// main thread     publishes results that still match their edit revision
//
// Every public method must be called from the GTK main thread.
class TorrentSearchIndex
{
public:
    using FindTorrentFunc = std::function<Glib::RefPtr<Torrent>(tr_torrent_id_t)>;
    using PublishedFunc = std::function<void(std::unordered_set<tr_torrent_id_t> const&)>;

    // Initial publication is deferred.
    // If any results are published, `on_published` runs once after the ids present at enable() time are processed.
    TorrentSearchIndex(tr_session* session, FindTorrentFunc find_torrent, PublishedFunc on_published);
    TorrentSearchIndex(TorrentSearchIndex&&) = delete;
    TorrentSearchIndex(TorrentSearchIndex const&) = delete;
    TorrentSearchIndex& operator=(TorrentSearchIndex&&) = delete;
    TorrentSearchIndex& operator=(TorrentSearchIndex const&) = delete;
    ~TorrentSearchIndex();

    [[nodiscard]] bool is_enabled() const noexcept
    {
        return enabled_;
    }

    void enable(std::vector<Glib::RefPtr<Torrent>> const& torrents);

    // Queue a missing or stale torrent.
    // This is cheap when its indexed text is current.
    void check(Torrent const& torrent);

    // Request the next batch if work is queued and no request is in flight.
    void kick();

    // Stop the worker and prevent new requests. Call this before tr_sessionClose().
    void shutdown();

private:
    struct Result {
        tr_torrent_id_t id = {};
        uint64_t edit_revision = {};
        std::string text;
    };

    struct Done {
        size_t ids_processed = {};
        std::vector<Result> results;
    };

    struct Shared;

    // The worker touches only `shared` and `dispatcher`, never `this`.
    static void worker_main(std::shared_ptr<Shared> shared, Glib::Dispatcher& dispatcher);
    static std::string build_search_text(tr_torrent_path_snapshot const& snapshot);

    void on_worker_done();
    void on_batch_done(size_t ids_processed, std::vector<Result>& results);

    tr_session* session_ = nullptr;
    FindTorrentFunc const find_torrent_;
    PublishedFunc const on_published_;
    std::shared_ptr<Shared> const shared_;

    // shutdown() joins worker_ before dispatcher_ can be destroyed.
    Glib::Dispatcher dispatcher_;
    std::thread worker_;

    std::deque<tr_torrent_id_t> queue_;
    std::unordered_set<tr_torrent_id_t> queued_;
    std::unordered_set<tr_torrent_id_t> published_;

    // Number of ids at the front of queue_ that belong to the initial build.
    size_t initial_remaining_ = 0U;

    bool enabled_ = false;
    bool in_flight_ = false;
    bool shut_down_ = false;
};
