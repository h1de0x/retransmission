// This file Copyright © Transmission authors and contributors.
// It may be used under GPLv2 (SPDX: GPL-2.0-only) or GPLv3 (SPDX: GPL-3.0-only).
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <future>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/session.h>
#include <libtransmission/torrent-builder.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/torrent-path-snapshot.h>

#include "test-fixtures.h"

using namespace std::literals;

namespace tr::test
{
namespace
{
class TorrentPathSnapshotTest : public SessionTest
{
protected:
    static auto constexpr MaxWait = 5s;

    struct RequestResult {
        bool called_in_session_thread = false;
        tr_torrent_path_snapshot_batch batch;
    };

    class SessionThreadBlocker
    {
    public:
        explicit SessionThreadBlocker(tr_session* const session)
            : released_{ std::make_shared<std::atomic<bool>>(false) }
        {
            session->queue_session_thread([released = released_]() { released->wait(false, std::memory_order_acquire); });
        }

        SessionThreadBlocker(SessionThreadBlocker const&) = delete;
        SessionThreadBlocker(SessionThreadBlocker&&) = delete;
        SessionThreadBlocker& operator=(SessionThreadBlocker const&) = delete;
        SessionThreadBlocker& operator=(SessionThreadBlocker&&) = delete;

        ~SessionThreadBlocker() noexcept
        {
            release();
        }

        void release() noexcept
        {
            if (released_) {
                released_->store(true, std::memory_order_release);
                released_->notify_all();
                released_.reset();
            }
        }

    private:
        std::shared_ptr<std::atomic<bool>> released_;
    };

    [[nodiscard]] RequestResult requestAndWait(
        std::vector<tr_torrent_id_t> const& ids,
        size_t const max_torrents,
        size_t const max_files,
        size_t const offset = 0U)
    {
        auto promise = std::make_shared<std::promise<RequestResult>>();
        auto future = promise->get_future();
        auto const tail = std::span{ ids }.subspan(offset);

        tr_sessionRequestTorrentPathSnapshots(
            session_,
            tail,
            max_torrents,
            max_files,
            [promise, session = session_](tr_torrent_path_snapshot_batch&& batch) {
                promise->set_value(
                    RequestResult{
                        .called_in_session_thread = session->am_in_session_thread(),
                        .batch = std::move(batch),
                    });
            });

        if (future.wait_for(MaxWait) != std::future_status::ready) {
            ADD_FAILURE() << "callback was not invoked";
            return {};
        }

        return future.get();
    }

    [[nodiscard]] static int torrentRenameAndWait(
        tr_torrent* const tor,
        std::string_view const oldpath,
        std::string_view const newname)
    {
        auto promise = std::make_shared<std::promise<int>>();
        auto future = promise->get_future();

        tr_torrentRenamePath(
            tor,
            oldpath,
            newname,
            [promise](
                tr_torrent_id_t const /*tor_id*/,
                std::string_view const /*oldpath*/,
                std::string_view const /*newname*/,
                tr_error const& error) { promise->set_value(error ? error.code() : 0); });

        if (future.wait_for(MaxWait) != std::future_status::ready) {
            ADD_FAILURE() << "rename callback was not invoked";
            return -1;
        }

        return future.get();
    }

    [[nodiscard]] static std::vector<std::string> expectedSubpaths(tr_torrent const* const tor)
    {
        auto ret = std::vector<std::string>{};
        for (tr_file_index_t i = 0, n = tr_torrentFileCount(tor); i < n; ++i) {
            ret.emplace_back(tr_torrentFile(tor, i).name);
        }
        return ret;
    }

    [[nodiscard]] std::vector<tr_torrent*> createThreeTorrents()
    {
        auto ret = std::vector<tr_torrent*>{};
        ret.emplace_back(zeroTorrentInit(ZeroTorrentState::NoFiles));
        ret.emplace_back(torrentInitFromFile("ubuntu-20.04.4-desktop-amd64.iso.torrent"sv));
        ret.emplace_back(torrentInitFromFile("debian-11.2.0-amd64-DVD-1.iso.torrent"sv));
        for (auto const* const tor : ret) {
            EXPECT_NE(nullptr, tor);
        }
        return ret;
    }

    [[nodiscard]] static std::vector<tr_torrent_id_t> idsOf(std::vector<tr_torrent*> const& torrents)
    {
        auto ret = std::vector<tr_torrent_id_t>{};
        std::ranges::transform(torrents, std::back_inserter(ret), [](tr_torrent const* const tor) {
            return tr_torrentId(tor);
        });
        return ret;
    }
};
} // namespace

// --- edit revision

TEST_F(TorrentPathSnapshotTest, editRevisionChangesWhenMagnetGetsMetainfo)
{
    static auto constexpr MagnetLink =
        "magnet:?xt=urn:btih:2e34989b1c60df821b2d046c884d8f4d1858b97a&dn=archlinux-2025.05.01-x86_64.iso"sv;
    static auto constexpr TorrentFile = LIBTRANSMISSION_TEST_ASSETS_DIR "/archlinux-2025.05.01-x86_64.iso.torrent";

    auto builder = tr_torrent_builder{ session_ };
    ASSERT_TRUE(builder.set_metainfo_from_magnet_link(MagnetLink));
    builder.set_paused(true);
    auto* const tor = tr_torrentNew(&builder, nullptr);
    ASSERT_NE(nullptr, tor);
    ASSERT_FALSE(tr_torrentHasMetadata(tor));

    auto result = requestAndWait({ tr_torrentId(tor) }, 1U, 1U);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    EXPECT_EQ(tr_torrentName(tor), result.batch.snapshots.front().name);
    EXPECT_TRUE(std::empty(result.batch.snapshots.front().file_subpaths));

    auto metainfo = tr_torrent_metainfo{};
    ASSERT_TRUE(metainfo.parse_torrent_file(TorrentFile));

    auto revision_before = uint64_t{};
    auto revision_after = uint64_t{};
    auto const set_ok = blockingRunInSessionThread([&]() {
        revision_before = tr_torrentEditRevision(tor);
        auto const ok = tr_torrentSetMetainfoFromFile(tor, &metainfo, TorrentFile);
        revision_after = tr_torrentEditRevision(tor);
        return ok;
    });

    ASSERT_TRUE(set_ok);
    EXPECT_TRUE(tr_torrentHasMetadata(tor));
    EXPECT_NE(revision_before, revision_after);

    result = requestAndWait({ tr_torrentId(tor) }, 1U, 1000U);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    EXPECT_EQ(revision_after, result.batch.snapshots.front().edit_revision);
    EXPECT_EQ(expectedSubpaths(tor), result.batch.snapshots.front().file_subpaths);
}

// --- path snapshots

TEST_F(TorrentPathSnapshotTest, snapshotCopiesNameRevisionAndPaths)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);

    auto const result = requestAndWait({ tr_torrentId(tor) }, 16U, 4096U);

    EXPECT_TRUE(result.called_in_session_thread);
    EXPECT_EQ(1U, result.batch.ids_processed);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    auto const& snapshot = result.batch.snapshots.front();
    EXPECT_EQ(tr_torrentId(tor), snapshot.id);
    EXPECT_EQ(tr_torrentEditRevision(tor), snapshot.edit_revision);
    EXPECT_EQ(tr_torrentName(tor), snapshot.name);
    EXPECT_EQ(expectedSubpaths(tor), snapshot.file_subpaths);
    EXPECT_EQ(3U, std::size(snapshot.file_subpaths));
}

TEST_F(TorrentPathSnapshotTest, snapshotReflectsNestedRenameAndRevision)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    auto const before = requestAndWait({ tr_torrentId(tor) }, 1U, 4096U);
    ASSERT_EQ(1U, std::size(before.batch.snapshots));

    EXPECT_EQ(0, torrentRenameAndWait(tor, "files-filled-with-zeroes/512", "renamed-512"));

    auto const after = requestAndWait({ tr_torrentId(tor) }, 1U, 4096U);
    ASSERT_EQ(1U, std::size(after.batch.snapshots));
    EXPECT_EQ(before.batch.snapshots.front().name, after.batch.snapshots.front().name);
    EXPECT_NE(before.batch.snapshots.front().edit_revision, after.batch.snapshots.front().edit_revision);
    auto const& paths = after.batch.snapshots.front().file_subpaths;
    EXPECT_NE(std::ranges::end(paths), std::ranges::find(paths, "files-filled-with-zeroes/renamed-512"sv));
    EXPECT_EQ(std::ranges::end(paths), std::ranges::find(paths, "files-filled-with-zeroes/512"sv));
}

TEST_F(TorrentPathSnapshotTest, emptyRequestGetsEmptyBatch)
{
    struct CallbackState {
        std::atomic<int> count = 0;
        RequestResult result;
    };

    auto blocker = SessionThreadBlocker{ session_ };
    auto const state = std::make_shared<CallbackState>();

    tr_sessionRequestTorrentPathSnapshots(
        session_,
        {},
        16U,
        4096U,
        [state, session = session_](tr_torrent_path_snapshot_batch&& batch) {
            state->result = RequestResult{
                .called_in_session_thread = session->am_in_session_thread(),
                .batch = std::move(batch),
            };
            state->count.fetch_add(1, std::memory_order_relaxed);
        });

    EXPECT_EQ(0, state->count.load(std::memory_order_relaxed));

    blocker.release();
    auto marker = std::make_shared<std::promise<void>>();
    auto marker_future = marker->get_future();
    session_->queue_session_thread([marker]() { marker->set_value(); });
    ASSERT_EQ(std::future_status::ready, marker_future.wait_for(MaxWait));

    EXPECT_EQ(1, state->count.load(std::memory_order_relaxed));
    EXPECT_TRUE(state->result.called_in_session_thread);
    EXPECT_EQ(0U, state->result.batch.ids_processed);
    EXPECT_TRUE(std::empty(state->result.batch.snapshots));
}

TEST_F(TorrentPathSnapshotTest, maxTorrentsLimitsBatch)
{
    auto const ids = idsOf(createThreeTorrents());

    auto result = requestAndWait(ids, 2U, 4096U);
    EXPECT_EQ(2U, result.batch.ids_processed);
    ASSERT_EQ(2U, std::size(result.batch.snapshots));
    EXPECT_EQ(ids[0], result.batch.snapshots[0].id);
    EXPECT_EQ(ids[1], result.batch.snapshots[1].id);

    result = requestAndWait(ids, 0U, 4096U);
    EXPECT_EQ(1U, result.batch.ids_processed);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    EXPECT_EQ(ids[0], result.batch.snapshots[0].id);
}

TEST_F(TorrentPathSnapshotTest, maxFilesIsSoftLimit)
{
    static auto constexpr MagnetLink =
        "magnet:?xt=urn:btih:2e34989b1c60df821b2d046c884d8f4d1858b97a&dn=archlinux-2025.05.01-x86_64.iso"sv;

    auto* const multi = zeroTorrentInit(ZeroTorrentState::NoFiles);
    auto* const single = torrentInitFromFile("ubuntu-20.04.4-desktop-amd64.iso.torrent"sv);
    auto builder = tr_torrent_builder{ session_ };
    ASSERT_TRUE(builder.set_metainfo_from_magnet_link(MagnetLink));
    builder.set_paused(true);
    auto* const magnet = tr_torrentNew(&builder, nullptr);
    ASSERT_NE(nullptr, multi);
    ASSERT_NE(nullptr, single);
    ASSERT_NE(nullptr, magnet);
    ASSERT_EQ(3U, tr_torrentFileCount(multi));
    ASSERT_EQ(1U, tr_torrentFileCount(single));
    ASSERT_EQ(0U, tr_torrentFileCount(magnet));

    // The first torrent is copied whole even when it exceeds max_files.
    auto result = requestAndWait({ tr_torrentId(multi), tr_torrentId(single) }, 16U, 1U);
    EXPECT_EQ(1U, result.batch.ids_processed);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    EXPECT_EQ(3U, std::size(result.batch.snapshots[0].file_subpaths));

    // A later torrent is deferred when it would exceed max_files.
    result = requestAndWait({ tr_torrentId(single), tr_torrentId(multi) }, 16U, 2U);
    EXPECT_EQ(1U, result.batch.ids_processed);
    ASSERT_EQ(1U, std::size(result.batch.snapshots));
    EXPECT_EQ(tr_torrentId(single), result.batch.snapshots[0].id);

    // A zero-file torrent still fits when the batch has exactly reached max_files.
    result = requestAndWait({ tr_torrentId(single), tr_torrentId(magnet) }, 16U, 1U);
    EXPECT_EQ(2U, result.batch.ids_processed);
    ASSERT_EQ(2U, std::size(result.batch.snapshots));
    EXPECT_EQ(tr_torrentId(single), result.batch.snapshots[0].id);
    EXPECT_EQ(tr_torrentId(magnet), result.batch.snapshots[1].id);
}

TEST_F(TorrentPathSnapshotTest, continuingFromIdsProcessedCoversEveryId)
{
    auto const torrents = createThreeTorrents();
    auto ids = idsOf(torrents);
    ids.insert(std::begin(ids) + 1, tr_torrent_id_t{ 100000 });

    auto seen = std::vector<tr_torrent_id_t>{};
    auto offset = size_t{};
    auto n_requests = size_t{};
    while (offset < std::size(ids) && n_requests < std::size(ids)) {
        auto const result = requestAndWait(ids, 1U, 1U, offset);
        ASSERT_GE(result.batch.ids_processed, 1U);
        for (auto const& snapshot : result.batch.snapshots) {
            seen.emplace_back(snapshot.id);
        }
        offset += result.batch.ids_processed;
        ++n_requests;
    }

    EXPECT_EQ(std::size(ids), offset);
    EXPECT_EQ(idsOf(torrents), seen);
}

} // namespace tr::test
