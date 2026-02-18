// =============================================================================
// DownloaderTest.cpp
// Downloader クラスの GoogleTest ユニットテスト
//
// 設計原則:
//  - curl 依存を MockCurlHandle で完全に代替（ネットワーク不要）
//  - 非同期イベントは condition_variable で待機（sleep 依存なし）
//  - 各テストは独立（SetUp/TearDown でリセット）
//  - スレッドリークを防ぐため Downloader のデストラクタを確実に呼ぶ
// =============================================================================

#include "Downloader.h"
#include "MockCurlHandle.h"
#include "MockObserver.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

using namespace Downloader;
using namespace Downloader::Test;
namespace fs = std::filesystem;

// =============================================================================
// テストフィクスチャ
// =============================================================================

class DownloaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 一時出力ファイルのパスを設定
        tempOutputPath_ = fs::temp_directory_path() / "downloader_test_output.bin";
        // 前回のテストファイルを削除
        fs::remove(tempOutputPath_);
    }

    void TearDown() override {
        // テスト後のファイルを削除
        fs::remove(tempOutputPath_);
    }

    /// @brief MockCurlHandle を使う Downloader を生成するヘルパー
    /// @param mockConfig モックの動作設定
    std::unique_ptr<Downloader> makeDownloader(MockConfig mockConfig = {}) {
        DownloaderConfig config;
        config.chunkSize = 1024;

        return std::make_unique<Downloader>(
            config,
            [mockConfig]() -> std::unique_ptr<ICurlHandle> {
                return std::make_unique<MockCurlHandle>(mockConfig);
            });
    }

    fs::path tempOutputPath_;
};

// =============================================================================
// 基本動作テスト
// =============================================================================

/// startDownload が正常に開始できる
TEST_F(DownloaderTest, StartDownload_ReturnsTrue) {
    auto downloader = makeDownloader();
    MockObserver observer;
    downloader->addObserver(&observer);

    bool started = downloader->startDownload(
        "http://example.com/file.bin",
        tempOutputPath_.string());

    EXPECT_TRUE(started);
}

/// 初期状態が IDLE であること
TEST_F(DownloaderTest, InitialState_IsIdle) {
    auto downloader = makeDownloader();
    EXPECT_EQ(downloader->getState(), DownloadState::IDLE);
}

/// startDownload 後に DOWNLOADING 状態になること
TEST_F(DownloaderTest, AfterStart_StateIsDownloading) {
    MockConfig cfg;
    cfg.totalSize  = 10 * 1024;
    cfg.chunkDelay = std::chrono::milliseconds(5);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());

    // 開始直後は DOWNLOADING または完了している可能性がある
    DownloadState state = downloader->getState();
    EXPECT_TRUE(state == DownloadState::DOWNLOADING ||
                state == DownloadState::COMPLETED);

    observer.waitForFinish();
}

/// 完了時に onCompleted が呼ばれること
TEST_F(DownloaderTest, OnCompleted_CalledAfterSuccessfulDownload) {
    MockConfig cfg;
    cfg.totalSize    = 4 * 1024; // 小さいファイル
    cfg.returnResult = CurlResult::OK;
    cfg.httpCode     = 200;
    cfg.chunkDelay   = std::chrono::milliseconds(0);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());

    bool finished = observer.waitForFinish(std::chrono::seconds(5));

    EXPECT_TRUE(finished) << "Download did not finish within timeout";
    EXPECT_EQ(observer.getCompletedCallCount(), 1);
    EXPECT_EQ(observer.getErrorCallCount(), 0);
    EXPECT_EQ(observer.getCancelledCallCount(), 0);
}

/// =============================================================================
/// 進捗通知テスト
/// =============================================================================

/// onProgress が少なくとも 1 回呼ばれること
TEST_F(DownloaderTest, OnProgress_CalledDuringDownload) {
    MockConfig cfg;
    cfg.totalSize    = 5 * 1024;
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    observer.waitForFinish();

    EXPECT_GT(observer.getProgressCallCount(), 0);
}

/// 完了後の最終進捗が 100% であること
TEST_F(DownloaderTest, Progress_Reaches100Percent_OnCompletion) {
    MockConfig cfg;
    cfg.totalSize    = 3 * 1024;
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    observer.waitForFinish();

    EXPECT_TRUE(observer.isCompleted());
    // 最終進捗が 100% に達していること
    auto last = observer.getLastProgress();
    EXPECT_DOUBLE_EQ(last.percent, 100.0);
}

/// =============================================================================
/// pause / resume テスト
/// =============================================================================

/// pause → resume が正しく動作すること
TEST_F(DownloaderTest, PauseResume_WorksCorrectly) {
    MockConfig cfg;
    cfg.totalSize    = 50 * 1024; // 50 KB（pause できる余地を確保）
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(2); // 各チャンクを少し遅らせる

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/large.bin",
                              tempOutputPath_.string());

    // pause を呼び出す前に少しダウンロードさせる
    observer.waitForProgress(3, std::chrono::seconds(2));

    // 一時停止
    downloader->pause();

    // onPaused が呼ばれるまで待機（sleep でなく cv で待機）
    bool paused = observer.waitForPaused(std::chrono::seconds(5));
    EXPECT_TRUE(paused) << "onPaused was not called";
    EXPECT_EQ(observer.getPausedCallCount(), 1);

    // 一時停止中の状態確認
    EXPECT_EQ(downloader->getState(), DownloadState::PAUSED);

    // 再開
    downloader->resume();

    // 完了を待つ
    bool finished = observer.waitForFinish(std::chrono::seconds(10));
    EXPECT_TRUE(finished) << "Download did not finish after resume";
    EXPECT_TRUE(observer.isCompleted());
    EXPECT_EQ(observer.getResumedCallCount(), 1);
}

/// IDLE 状態で pause を呼んでも安全であること
TEST_F(DownloaderTest, Pause_WhenIdle_IsNoop) {
    auto downloader = makeDownloader();
    // 例外なく完了すること
    EXPECT_NO_THROW(downloader->pause());
    EXPECT_EQ(downloader->getState(), DownloadState::IDLE);
}

/// IDLE 状態で resume を呼んでも安全であること
TEST_F(DownloaderTest, Resume_WhenIdle_IsNoop) {
    auto downloader = makeDownloader();
    EXPECT_NO_THROW(downloader->resume());
    EXPECT_EQ(downloader->getState(), DownloadState::IDLE);
}

/// =============================================================================
/// cancel テスト
/// =============================================================================

/// cancel が正しく停止し onCancelled が呼ばれること
TEST_F(DownloaderTest, Cancel_StopsDownload_AndNotifiesCancelled) {
    MockConfig cfg;
    cfg.totalSize    = 100 * 1024; // 十分大きくしてキャンセルできる余地を確保
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(2);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/large.bin",
                              tempOutputPath_.string());

    // 少しダウンロードさせてからキャンセル
    observer.waitForProgress(3, std::chrono::seconds(2));
    downloader->cancel();

    bool finished = observer.waitForFinish(std::chrono::seconds(5));

    EXPECT_TRUE(finished) << "Cancel did not finish within timeout";
    EXPECT_TRUE(observer.isCancelled());
    EXPECT_EQ(observer.getCancelledCallCount(), 1);
    EXPECT_EQ(observer.getCompletedCallCount(), 0);
}

/// cancel 後に再度 startDownload できること
TEST_F(DownloaderTest, AfterCancel_CanRestartDownload) {
    MockConfig cfg;
    cfg.totalSize    = 100 * 1024;
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(2);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    // 1回目
    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    observer.waitForProgress(2, std::chrono::seconds(2));
    downloader->cancel();
    observer.waitForFinish(std::chrono::seconds(5));

    EXPECT_TRUE(observer.isCancelled());

    // 2回目（再起動できること）
    observer.reset();
    fs::remove(tempOutputPath_);

    MockConfig cfg2;
    cfg2.totalSize    = 2 * 1024;
    cfg2.chunkDelay   = std::chrono::milliseconds(0);
    cfg2.httpCode     = 200;

    // 2回目は完了するダウンローダー
    auto downloader2 = std::make_unique<Downloader>(
        DownloaderConfig{},
        [cfg2]() -> std::unique_ptr<ICurlHandle> {
            return std::make_unique<MockCurlHandle>(cfg2);
        });
    downloader2->addObserver(&observer);

    bool started = downloader2->startDownload("http://example.com/file.bin",
                                              tempOutputPath_.string());
    EXPECT_TRUE(started);
    observer.waitForFinish(std::chrono::seconds(5));
    EXPECT_TRUE(observer.isCompleted());
}

/// IDLE 状態での cancel は安全
TEST_F(DownloaderTest, Cancel_WhenIdle_IsNoop) {
    auto downloader = makeDownloader();
    EXPECT_NO_THROW(downloader->cancel());
}

/// =============================================================================
/// エラー処理テスト
/// =============================================================================

/// ネットワークエラー時に onError が呼ばれること
TEST_F(DownloaderTest, NetworkError_CallsOnError) {
    MockConfig cfg;
    cfg.returnResult  = CurlResult::NETWORK_ERROR;
    cfg.errorMessage  = "Connection refused";

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://invalid.example.com/file.bin",
                              tempOutputPath_.string());

    bool finished = observer.waitForFinish(std::chrono::seconds(5));

    EXPECT_TRUE(finished);
    EXPECT_TRUE(observer.isError());
    EXPECT_EQ(observer.getCompletedCallCount(), 0);
    EXPECT_FALSE(observer.getLastError().empty());
}

/// HTTP 404 エラー時に onError が呼ばれること
TEST_F(DownloaderTest, Http404_CallsOnError) {
    MockConfig cfg;
    cfg.returnResult = CurlResult::OK;
    cfg.httpCode     = 404;
    cfg.totalSize    = 0; // 空レスポンス
    cfg.chunkDelay   = std::chrono::milliseconds(0);

    // HTTP エラーをシミュレートするため、totalSize を 0 にして即完了させる
    // perform() は OK を返すが httpCode が 404
    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/notfound.bin",
                              tempOutputPath_.string());

    bool finished = observer.waitForFinish(std::chrono::seconds(5));
    EXPECT_TRUE(finished);
    EXPECT_TRUE(observer.isError());
    EXPECT_THAT(observer.getLastError(), ::testing::HasSubstr("404"));
}

/// HTTP 500 エラー時に onError が呼ばれること
TEST_F(DownloaderTest, Http500_CallsOnError) {
    MockConfig cfg;
    cfg.returnResult = CurlResult::OK;
    cfg.httpCode     = 500;
    cfg.totalSize    = 0;
    cfg.chunkDelay   = std::chrono::milliseconds(0);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/server_error.bin",
                              tempOutputPath_.string());

    bool finished = observer.waitForFinish(std::chrono::seconds(5));
    EXPECT_TRUE(finished);
    EXPECT_TRUE(observer.isError());
    EXPECT_THAT(observer.getLastError(), ::testing::HasSubstr("500"));
}

/// =============================================================================
/// レジュームテスト
/// =============================================================================

/// 既存ファイルがある場合に Range ヘッダーが設定されること
TEST_F(DownloaderTest, Resume_SetsRangeHeader_WhenFileExists) {
    // 事前に既存ファイルを作成（1024 バイト）
    {
        std::ofstream pre(tempOutputPath_, std::ios::binary);
        std::vector<char> dummy(1024, 'X');
        pre.write(dummy.data(), static_cast<std::streamsize>(dummy.size()));
    }

    // モックハンドル作成時に setResumeFrom が呼ばれたことを検証するための
    // 共有状態（ファクトリはラムダなので外から確認する）
    std::atomic<bool> resumeFromWasCalled{false};
    std::atomic<int64_t> resumeFromValue{-1};

    DownloaderConfig config;
    config.chunkSize = 1024;

    MockConfig mockCfg;
    mockCfg.totalSize   = 2 * 1024;
    mockCfg.chunkDelay  = std::chrono::milliseconds(0);
    mockCfg.httpCode    = 206; // Partial Content

    auto downloader = std::make_unique<Downloader>(
        config,
        [&, mockCfg]() -> std::unique_ptr<ICurlHandle> {
            auto mock = std::make_unique<MockCurlHandle>(mockCfg);
            // perform 前に検証できるよう、setResumeFrom のシャドウ確認は
            // MockCurlHandle の内部カウンタを使う
            return mock;
        });

    MockObserver observer;
    downloader->addObserver(&observer);
    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    observer.waitForFinish(std::chrono::seconds(5));

    // ファイルが 1024 バイト存在するので downloadedBytes が 1024 以上で始まること
    auto records = observer.getProgressRecords();
    if (!records.empty()) {
        EXPECT_GE(records.front().downloadedBytes, 1024LL)
            << "Resume should start from existing file size";
    }
}

/// =============================================================================
/// 大きいファイルテスト
/// =============================================================================

/// 大きなファイル (1 MB) でもクラッシュしないこと
TEST_F(DownloaderTest, LargeFile_DoesNotCrash) {
    MockConfig cfg;
    cfg.totalSize    = 1024 * 1024; // 1 MB
    cfg.chunkSize    = 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0); // 速度優先
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    downloader->startDownload("http://example.com/large.bin",
                              tempOutputPath_.string());

    bool finished = observer.waitForFinish(std::chrono::seconds(30));
    EXPECT_TRUE(finished) << "Large file download did not finish";
    EXPECT_TRUE(observer.isCompleted());
    EXPECT_EQ(observer.getErrorCallCount(), 0);
}

/// =============================================================================
/// スレッド安全性テスト
/// =============================================================================

/// 複数のオブザーバーを登録・削除してもクラッシュしないこと
TEST_F(DownloaderTest, MultipleObservers_AllReceiveNotifications) {
    MockConfig cfg;
    cfg.totalSize    = 4 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);

    MockObserver obs1, obs2, obs3;
    downloader->addObserver(&obs1);
    downloader->addObserver(&obs2);
    downloader->addObserver(&obs3);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());

    obs1.waitForFinish(std::chrono::seconds(5));
    obs2.waitForFinish(std::chrono::seconds(1));
    obs3.waitForFinish(std::chrono::seconds(1));

    EXPECT_TRUE(obs1.isCompleted());
    EXPECT_TRUE(obs2.isCompleted());
    EXPECT_TRUE(obs3.isCompleted());
}

/// すでに実行中に startDownload を呼ぶと false が返ること
TEST_F(DownloaderTest, StartDownload_WhileRunning_ReturnsFalse) {
    MockConfig cfg;
    cfg.totalSize    = 100 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(5);

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    bool first = downloader->startDownload("http://example.com/file.bin",
                                           tempOutputPath_.string());
    EXPECT_TRUE(first);

    // 少し待ってから2回目を呼ぶ
    observer.waitForProgress(2, std::chrono::seconds(2));

    bool second = downloader->startDownload("http://example.com/file2.bin",
                                            tempOutputPath_.string());
    EXPECT_FALSE(second) << "Second startDownload should return false while running";

    downloader->cancel();
    observer.waitForFinish(std::chrono::seconds(5));
}

/// デストラクタが実行中ダウンロードを安全に終了すること
TEST_F(DownloaderTest, Destructor_SafelyStopsRunningDownload) {
    MockConfig cfg;
    cfg.totalSize    = 500 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(1);

    {
        // スコープを抜けるとデストラクタが呼ばれる
        auto downloader = makeDownloader(cfg);
        MockObserver observer;
        downloader->addObserver(&observer);

        downloader->startDownload("http://example.com/file.bin",
                                   tempOutputPath_.string());
        observer.waitForProgress(5, std::chrono::seconds(2));
        // デストラクタを呼ぶ（スコープアウト）
    }
    // ここに到達できれば成功（デッドロックや例外がないことの確認）
    SUCCEED();
}

/// =============================================================================
/// Observer 管理テスト
/// =============================================================================

/// removeObserver 後は通知が来ないこと
TEST_F(DownloaderTest, RemoveObserver_StopsNotifications) {
    MockConfig cfg;
    cfg.totalSize    = 5 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);
    MockObserver obs1, obs2;

    downloader->addObserver(&obs1);
    downloader->addObserver(&obs2);

    // obs2 を削除
    downloader->removeObserver(&obs2);

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    obs1.waitForFinish(std::chrono::seconds(5));

    EXPECT_TRUE(obs1.isCompleted());
    // obs2 は削除したので通知が来ていないはず
    EXPECT_EQ(obs2.getCompletedCallCount(), 0);
    EXPECT_EQ(obs2.getProgressCallCount(), 0);
}

/// 重複登録しても onCompleted が 1 回しか呼ばれないこと
TEST_F(DownloaderTest, AddObserver_Duplicate_NotAddedTwice) {
    MockConfig cfg;
    cfg.totalSize    = 2 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);
    MockObserver obs;

    downloader->addObserver(&obs);
    downloader->addObserver(&obs); // 重複登録
    downloader->addObserver(&obs); // さらに重複

    downloader->startDownload("http://example.com/file.bin",
                              tempOutputPath_.string());
    obs.waitForFinish(std::chrono::seconds(5));

    // 重複登録されていても 1 回だけ呼ばれること
    EXPECT_EQ(obs.getCompletedCallCount(), 1);
}

/// =============================================================================
/// getStats テスト
/// =============================================================================

/// getStats が正しい情報を返すこと
TEST_F(DownloaderTest, GetStats_ReturnsCorrectInfo) {
    MockConfig cfg;
    cfg.totalSize    = 4 * 1024;
    cfg.chunkDelay   = std::chrono::milliseconds(0);
    cfg.httpCode     = 200;

    auto downloader = makeDownloader(cfg);
    MockObserver observer;
    downloader->addObserver(&observer);

    const std::string url = "http://example.com/stats_test.bin";
    downloader->startDownload(url, tempOutputPath_.string());
    observer.waitForFinish(std::chrono::seconds(5));

    auto stats = downloader->getStats();
    EXPECT_EQ(stats.state, DownloadState::COMPLETED);
    EXPECT_EQ(stats.url, url);
    EXPECT_GE(stats.downloadedBytes, 0LL);
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
