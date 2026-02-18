#pragma once
// =============================================================================
// MockObserver.h
// テスト用 IDownloaderObserver モック実装
//
// 設計方針:
//  - 各コールバックの呼び出し回数・引数を記録する
//  - condition_variable により、テストが非同期イベントを安全に待機できる
//  - タイムアウト付き waitFor* メソッドで sleep 依存のフレークテストを防ぐ
// =============================================================================

#include "IDownloaderObserver.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Downloader {
namespace Test {

/// 進捗スナップショット
struct ProgressRecord {
    int64_t downloadedBytes;
    int64_t totalBytes;
    double  percent;
};

/// @brief テスト用オブザーバーモック
/// コールバックの呼び出しを記録し、非同期完了を安全に待機する
class MockObserver final : public IDownloaderObserver {
public:
    MockObserver() = default;

    // -------------------------------------------------------------------------
    // IDownloaderObserver 実装
    // -------------------------------------------------------------------------

    void onProgress(int64_t downloadedBytes,
                    int64_t totalBytes,
                    double  percent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        progressRecords_.push_back({downloadedBytes, totalBytes, percent});
        progressCallCount_++;
    }

    void onCompleted() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completedCallCount_++;
        }
        cv_.notify_all();
    }

    void onError(const std::string& errorMessage) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastErrorMessage_ = errorMessage;
            errorCallCount_++;
        }
        cv_.notify_all();
    }

    void onPaused() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pausedCallCount_++;
        }
        pausedCv_.notify_all();
    }

    void onResumed() override {
        std::lock_guard<std::mutex> lock(mutex_);
        resumedCallCount_++;
    }

    void onCancelled() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelledCallCount_++;
        }
        cv_.notify_all();
    }

    // -------------------------------------------------------------------------
    // 待機メソッド（タイムアウト付き）
    // sleep ではなく condition_variable を使うため再現性が高い
    // -------------------------------------------------------------------------

    /// @brief 完了・エラー・キャンセルのいずれかを待機する
    /// @return true: イベント発生 / false: タイムアウト
    bool waitForFinish(std::chrono::milliseconds timeout
                       = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this]() {
            return completedCallCount_ > 0 ||
                   errorCallCount_    > 0 ||
                   cancelledCallCount_ > 0;
        });
    }

    /// @brief Paused イベントを待機する
    bool waitForPaused(std::chrono::milliseconds timeout
                       = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return pausedCv_.wait_for(lock, timeout, [this]() {
            return pausedCallCount_ > 0;
        });
    }

    /// @brief 少なくとも N 回の Progress イベントを待機する
    bool waitForProgress(int count,
                         std::chrono::milliseconds timeout
                         = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lock(mutex_);
        // progress は cv_ では通知していないので、ポーリングで待機
        // （progress は高頻度なので OK）
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (progressCallCount_ >= count) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock.lock();
        }
    }

    // -------------------------------------------------------------------------
    // 検証用アクセサ（スレッドセーフ）
    // -------------------------------------------------------------------------

    int  getProgressCallCount()   const { std::lock_guard<std::mutex> l(mutex_); return progressCallCount_; }
    int  getCompletedCallCount()  const { std::lock_guard<std::mutex> l(mutex_); return completedCallCount_; }
    int  getErrorCallCount()      const { std::lock_guard<std::mutex> l(mutex_); return errorCallCount_; }
    int  getPausedCallCount()     const { std::lock_guard<std::mutex> l(mutex_); return pausedCallCount_; }
    int  getResumedCallCount()    const { std::lock_guard<std::mutex> l(mutex_); return resumedCallCount_; }
    int  getCancelledCallCount()  const { std::lock_guard<std::mutex> l(mutex_); return cancelledCallCount_; }

    std::string getLastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastErrorMessage_;
    }

    std::vector<ProgressRecord> getProgressRecords() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return progressRecords_;
    }

    /// 直近の進捗レコードを取得
    ProgressRecord getLastProgress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (progressRecords_.empty()) {
            return {0, 0, -1.0};
        }
        return progressRecords_.back();
    }

    bool isCompleted()  const { return getCompletedCallCount() > 0; }
    bool isError()      const { return getErrorCallCount() > 0; }
    bool isCancelled()  const { return getCancelledCallCount() > 0; }
    bool isFinished()   const { return isCompleted() || isError() || isCancelled(); }

    // -------------------------------------------------------------------------
    // リセット（複数テストで同じオブザーバーを再利用する場合）
    // -------------------------------------------------------------------------
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        progressRecords_.clear();
        progressCallCount_   = 0;
        completedCallCount_  = 0;
        errorCallCount_      = 0;
        pausedCallCount_     = 0;
        resumedCallCount_    = 0;
        cancelledCallCount_  = 0;
        lastErrorMessage_.clear();
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;       // 完了/エラー/キャンセル待機用
    std::condition_variable pausedCv_; // 一時停止待機用

    std::vector<ProgressRecord> progressRecords_;
    int  progressCallCount_{0};
    int  completedCallCount_{0};
    int  errorCallCount_{0};
    int  pausedCallCount_{0};
    int  resumedCallCount_{0};
    int  cancelledCallCount_{0};
    std::string lastErrorMessage_;
};

} // namespace Test
} // namespace Downloader
