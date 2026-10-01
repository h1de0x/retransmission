// This file Copyright © Transmission authors and contributors.
// It may be used under GPLv2 (SPDX: GPL-2.0-only) or GPLv3 (SPDX: GPL-3.0-only).
// License text can be found in the licenses/ folder.

#include "TorrentSearchIndex.h"

#include <libtransmission/torrent-path-snapshot.h>

#include <glibmm/ustring.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef> // std::ptrdiff_t
#include <functional> // std::ref()
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace
{

// Soft bounds on session-thread work. A single oversized torrent is not split.
auto constexpr MaxTorrentsPerBatch = size_t{ 64U };
auto constexpr MaxFilesPerBatch = size_t{ 4096U };

} // namespace

struct TorrentSearchIndex::Shared {
    // Set by shutdown() in the main thread and read from every thread.
    std::atomic<bool> cancelled = false;

    std::mutex mutex;
    std::condition_variable cv;

    // Requests are serialized.
    // At most one job waits for the worker and one result waits for the main thread.
    std::optional<tr_torrent_path_snapshot_batch> job;
    std::optional<Done> done;
    bool stop = false;
};

TorrentSearchIndex::TorrentSearchIndex(tr_session* session, FindTorrentFunc find_torrent, PublishedFunc on_published)
    : session_{ session }
    , find_torrent_{ std::move(find_torrent) }
    , on_published_{ std::move(on_published) }
    , shared_{ std::make_shared<Shared>() }
{
    dispatcher_.connect(sigc::mem_fun(*this, &TorrentSearchIndex::on_worker_done));
}

TorrentSearchIndex::~TorrentSearchIndex()
{
    shutdown();
}

void TorrentSearchIndex::enable(std::vector<Glib::RefPtr<Torrent>> const& torrents)
{
    if (enabled_ || shut_down_ || session_ == nullptr) {
        return;
    }

    enabled_ = true;
    worker_ = std::thread{ &TorrentSearchIndex::worker_main, shared_, std::ref(dispatcher_) };

    for (auto const& torrent : torrents) {
        if (torrent) {
            check(*torrent.get());
        }
    }

    initial_remaining_ = std::size(queue_);
    kick();
}

void TorrentSearchIndex::check(Torrent const& torrent)
{
    if (!enabled_) {
        return;
    }

    if (torrent.has_search_text() && torrent.get_search_text_revision() == tr_torrentEditRevision(&torrent.get_underlying())) {
        return;
    }

    if (auto const id = torrent.get_id(); queued_.insert(id).second) {
        queue_.push_back(id);
    }
}

void TorrentSearchIndex::kick()
{
    if (!enabled_ || in_flight_ || std::empty(queue_)) {
        return;
    }

    auto const n_ids = std::min(std::size(queue_), MaxTorrentsPerBatch);
    auto const ids = std::vector<tr_torrent_id_t>{
        std::begin(queue_),
        std::next(std::begin(queue_), static_cast<std::ptrdiff_t>(n_ids)),
    };

    // The callback can begin before this function returns. Set our state first;
    // queue_ stays unchanged until ids_processed is handled in on_batch_done().
    in_flight_ = true;

    tr_sessionRequestTorrentPathSnapshots(
        session_,
        std::span{ ids },
        std::size(ids),
        MaxFilesPerBatch,
        [shared = shared_](tr_torrent_path_snapshot_batch&& batch) {
            // The callback runs in the session thread.
            // Hand the owning copy off quickly.
            if (shared->cancelled.load(std::memory_order_acquire)) {
                return;
            }

            {
                auto const lock = std::scoped_lock{ shared->mutex };
                shared->job = std::move(batch);
            }

            shared->cv.notify_one();
        });
}

void TorrentSearchIndex::shutdown()
{
    if (shut_down_) {
        return;
    }

    shut_down_ = true;
    enabled_ = false;
    shared_->cancelled.store(true, std::memory_order_release);

    {
        auto const lock = std::scoped_lock{ shared_->mutex };
        shared_->stop = true;
    }

    shared_->cv.notify_all();

    // join() prevents any later dispatcher_ emission.
    // on_worker_done() ignores a notification emitted before shutdown.
    if (worker_.joinable()) {
        worker_.join();
    }

    session_ = nullptr;
    queue_.clear();
    queued_.clear();
    published_.clear();
    in_flight_ = false;
}

void TorrentSearchIndex::worker_main(std::shared_ptr<Shared> shared, Glib::Dispatcher& dispatcher)
{
    for (;;) {
        auto batch = tr_torrent_path_snapshot_batch{};

        {
            auto lock = std::unique_lock{ shared->mutex };
            shared->cv.wait(lock, [&shared]() { return shared->stop || shared->job.has_value(); });

            if (shared->stop) {
                return;
            }

            if (!shared->job.has_value()) {
                continue;
            }

            batch = std::move(*shared->job);
            shared->job.reset();
        }

        auto done = Done{ .ids_processed = batch.ids_processed, .results = {} };
        done.results.reserve(std::size(batch.snapshots));

        for (auto const& snapshot : batch.snapshots) {
            if (shared->cancelled.load(std::memory_order_acquire)) {
                return;
            }

            done.results.push_back(
                Result{
                    .id = snapshot.id,
                    .edit_revision = snapshot.edit_revision,
                    .text = build_search_text(snapshot),
                });
        }

        {
            auto const lock = std::scoped_lock{ shared->mutex };
            shared->done = std::move(done);
        }

        // Even an empty result advances ids_processed.
        // This matters when torrents disappear before their snapshot is taken.
        dispatcher.emit();
    }
}

std::string TorrentSearchIndex::build_search_text(tr_torrent_path_snapshot const& snapshot)
{
    auto text = std::string{};

    // Case folding rarely changes length, so this avoids most reallocations.
    auto size_hint = std::size(snapshot.name) + 1U;
    for (auto const& path : snapshot.file_subpaths) {
        size_hint += std::size(path) + 1U;
    }
    text.reserve(size_hint);

    auto const append = [&text](std::string const& str) {
        // Metainfo names are converted to UTF-8, but a rename can store arbitrary bytes.
        auto utf8 = Glib::ustring{ str };
        if (!utf8.validate()) {
            utf8 = utf8.make_valid();
        }
        text += utf8.casefold().raw();

        // A query cannot contain NUL, so a match cannot span two entries.
        text.push_back('\0');
    };

    append(snapshot.name);
    for (auto const& path : snapshot.file_subpaths) {
        append(path);
    }

    return text;
}

void TorrentSearchIndex::on_worker_done()
{
    if (shut_down_) {
        return;
    }

    auto done = std::optional<Done>{};
    {
        auto const lock = std::scoped_lock{ shared_->mutex };
        done = std::exchange(shared_->done, std::nullopt);
    }

    if (done) {
        on_batch_done(done->ids_processed, done->results);
    }
}

void TorrentSearchIndex::on_batch_done(size_t const ids_processed, std::vector<Result>& results)
{
    in_flight_ = false;

    auto const n_done = std::min(ids_processed, std::size(queue_));
    for (size_t i = 0U; i < n_done; ++i) {
        queued_.erase(queue_.front());
        queue_.pop_front();
    }
    initial_remaining_ -= std::min(initial_remaining_, n_done);

    for (auto& result : results) {
        auto const torrent = find_torrent_(result.id);
        if (!torrent) {
            continue;
        }

        // Keep the old search text if the torrent changed after the snapshot.
        // check() will queue another snapshot on the next periodic update.
        if (tr_torrentEditRevision(&torrent->get_underlying()) != result.edit_revision) {
            continue;
        }

        torrent->set_search_text(std::move(result.text), result.edit_revision);
        published_.insert(result.id);
    }

    // Defer refiltering during the initial build, then publish later batches as they finish.
    if (initial_remaining_ == 0U && !std::empty(published_)) {
        auto published = std::exchange(published_, {});
        on_published_(published);
    }

    // The snapshot API cannot be called from its session-thread callback.
    // This runs in the GTK main thread, so requesting the next batch is safe.
    kick();
}
