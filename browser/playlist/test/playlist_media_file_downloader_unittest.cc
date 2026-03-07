/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/playlist/content/browser/playlist_media_file_downloader.h"

#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "brave/browser/playlist/playlist_service_factory.h"
#include "brave/components/playlist/core/common/features.h"
#include "brave/components/playlist/core/common/mojom/playlist.mojom.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/testing_profile.h"
#include "components/download/public/common/download_task_runner.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_host_resolver.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace playlist {

namespace {

std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  if (request.relative_url == "/media_file") {
    response->set_code(net::HTTP_OK);
    response->set_content_type("video/mp4");
    response->set_content("fake media content");
  } else {
    response->set_code(net::HTTP_NOT_FOUND);
  }
  return response;
}

}  // namespace

class MockMediaFileDownloaderDelegate
    : public PlaylistMediaFileDownloader::Delegate {
 public:
  MOCK_METHOD(void,
              OnMediaFileDownloadProgressed,
              (const std::string& id,
               int64_t total_bytes,
               int64_t received_bytes,
               int percent_complete,
               base::TimeDelta time_remaining),
              (override));
  MOCK_METHOD(void,
              OnMediaFileReady,
              (const std::string& id,
               const std::string& media_file_path,
               int64_t received_bytes),
              (override));
  MOCK_METHOD(void,
              OnMediaFileGenerationFailed,
              (const std::string& id),
              (override));

  base::SequencedTaskRunner* GetTaskRunner() override {
    return base::SingleThreadTaskRunner::GetCurrentDefault().get();
  }
};

class PlaylistMediaFileDownloaderTest : public testing::Test {
 public:
  PlaylistMediaFileDownloaderTest() {
    scoped_feature_list_.InitAndEnableFeature(features::kPlaylist);
  }

  void SetUp() override {
    testing::Test::SetUp();

    host_resolver_ = std::make_unique<content::TestHostResolver>();
    host_resolver_->host_resolver()->AddRule("*", "127.0.0.1");

    // Ensure PlaylistServiceFactory is instantiated so its prefs are
    // registered via DependencyManager.
    PlaylistServiceFactory::GetInstance();

    auto prefs =
        std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
    RegisterUserProfilePrefs(prefs->registry());
    profile_ =
        TestingProfile::Builder().SetPrefService(std::move(prefs)).Build();

    DCHECK(!download::GetIOTaskRunner());
    download::SetIOTaskRunner(
        base::SingleThreadTaskRunner::GetCurrentDefault());

    server_.RegisterRequestHandler(base::BindRepeating(&HandleRequest));
    ASSERT_TRUE(server_.Start());

    downloader_ = std::make_unique<PlaylistMediaFileDownloader>(
        &delegate_, profile_.get());
  }

  void TearDown() override {
    downloader_.reset();
    profile_.reset();
    download::ClearIOTaskRunnerForTesting();
    testing::Test::TearDown();
  }

 protected:
  // IO_MAINLOOP merges the IO thread with the main thread so that download
  // callbacks (which post to the IO task runner) are processed in the same
  // loop as the test.
  content::BrowserTaskEnvironment task_environment_{
      content::BrowserTaskEnvironment::IO_MAINLOOP};

  base::test::ScopedFeatureList scoped_feature_list_;
  std::unique_ptr<content::TestHostResolver> host_resolver_;
  std::unique_ptr<TestingProfile> profile_;
  testing::NiceMock<MockMediaFileDownloaderDelegate> delegate_;
  std::unique_ptr<PlaylistMediaFileDownloader> downloader_;
  net::EmbeddedTestServer server_{net::EmbeddedTestServer::TYPE_HTTP};
};

// Regression test for https://github.com/brave/brave-browser/issues/53444
//
// Crash scenario:
//   1. DownloadMediaFileForPlaylistItem() sets guid-A and posts BeginDownload
//      to the IO task runner. The download item doesn't exist yet.
//   2. RequestCancelCurrentPlaylistGeneration() clears current_download_item_
//      guid_ (to "") and posts an async cancel. current_item_ is reset to null.
//   3. The event loop processes BeginDownload, the server responds, and
//      InProgressDownloadManager creates a DownloadItemImpl with guid-A, then
//      fires OnDownloadCreated(item_A).
//      - Before fix: guid-A != "" → item_A is NOT added to
//        download_item_observation_, only a cancel is scheduled. The item
//        remains in InProgressDownloadManager unobserved.
//      - After fix:  item_A IS added to download_item_observation_ first,
//        then the cancel is scheduled.
//   4. ~PlaylistMediaFileDownloader() calls TakeInProgressDownloads() which
//      returns item_A, then calls DetachCachedFile(item_A) which calls
//      RemoveObservation(item_A):
//      - Before fix: CHECK failure — item_A is not observed → crash.
//      - After fix:  item_A is observed → cleanup succeeds.
TEST_F(PlaylistMediaFileDownloaderTest,
       NoCrashOnDestroyWithMismatchedGuidDownload) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  auto item = mojom::PlaylistItem::New();
  item->id = "test-item-id";
  item->media_source = server_.GetURL("/media_file");
  item->media_path = server_.GetURL("/media_file");
  item->cached = false;

  // Step 1: Start download. Posts BeginDownload to the IO task runner.
  // The download item (DownloadItemImpl) does not exist yet.
  downloader_->DownloadMediaFileForPlaylistItem(
      item, temp_dir.GetPath().AppendASCII("media.file"));

  // Step 2: Cancel before the download item is created.
  // current_download_item_guid_ becomes "" and current_item_ is reset.
  downloader_->RequestCancelCurrentPlaylistGeneration();

  // Step 3: Pump the event loop. This processes BeginDownload, the network
  // round-trip to the local test server, and DownloadItemImpl creation.
  // OnDownloadCreated() fires with the original guid-A while
  // current_download_item_guid_ is "".
  //   Before fix: item not observed → remains in InProgressDownloadManager.
  //   After fix:  item observed → will be cleaned up properly.
  base::RunLoop().RunUntilIdle();

  // Step 4: Destroy the downloader. The destructor calls
  // TakeInProgressDownloads() and then DetachCachedFile() → RemoveObservation.
  //   Before fix: RemoveObservation on unobserved item → CHECK failure → crash.
  //   After fix:  observation was added → RemoveObservation succeeds.
  downloader_.reset();
}

}  // namespace playlist
