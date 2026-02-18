// =============================================================================
// main.cpp
// Downloader の動作確認サンプル
//
// 実行例:
//   downloader.exe https://example.com/file.zip output.zip
//
// 動作シーケンス:
//   1. ダウンロード開始
//   2. 2 秒後に一時停止
//   3. 2 秒後に再開
//   4. 大きなファイルの場合は 5 秒後にキャンセル（小さいファイルは完了まで待機）
// =============================================================================

#include "Downloader.h"
#include "IDownloaderObserver.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace Downloader;

// =============================================================================
// ログユーティリティ
// =============================================================================

/// 現在時刻文字列を返す
std::string currentTime() {
    auto now    = std::chrono::system_clock::now();
    auto timeT  = std::chrono::system_clock::to_time_t(now);
    auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

/// ログ出力マクロ（スレッドセーフな cout に相当）
#define LOG(tag, msg) \
    std::cout << "[" << currentTime() << "][" << (tag) << "] " << (msg) << "\n" << std::flush

// =============================================================================
// ConsoleObserver: コンソールに全イベントを出力するオブザーバー実装
// =============================================================================
class ConsoleObserver final : public IDownloaderObserver {
public:
    explicit ConsoleObserver(const std::string& name = "Observer")
        : name_(name) {}

    // -------------------------------------------------------------------------
    // 進捗通知: プログレスバーをコンソールに描画する
    // -------------------------------------------------------------------------
    void onProgress(int64_t downloadedBytes,
                    int64_t totalBytes,
                    double  percent) override {
        // 進捗バー描画（50文字幅）
        constexpr int BAR_WIDTH = 40;
        std::ostringstream oss;

        if (percent >= 0.0) {
            int filled = static_cast<int>(percent / 100.0 * BAR_WIDTH);
            oss << "[";
            for (int i = 0; i < BAR_WIDTH; ++i) {
                oss << (i < filled ? '#' : '-');
            }
            oss << "] " << std::fixed << std::setprecision(1) << percent << "% ";
        } else {
            oss << "[" << std::string(BAR_WIDTH, '?') << "] --.-% ";
        }

        // バイト数を人間が読みやすい形式に変換
        auto humanize = [](int64_t bytes) -> std::string {
            if (bytes < 0)         return "?";
            if (bytes < 1024)      return std::to_string(bytes) + " B";
            if (bytes < 1024*1024) return std::to_string(bytes / 1024) + " KB";
            return std::to_string(bytes / (1024*1024)) + " MB";
        };

        oss << humanize(downloadedBytes) << " / " << humanize(totalBytes);

        // 同一行を上書きして表示（\r で行頭に戻る）
        std::cout << "\r" << oss.str() << "    " << std::flush;
    }

    // -------------------------------------------------------------------------
    // 完了通知
    // -------------------------------------------------------------------------
    void onCompleted() override {
        std::cout << "\n"; // 進捗バーの行を改行
        LOG(name_, ">>> COMPLETED <<<");
        completed_.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // エラー通知
    // -------------------------------------------------------------------------
    void onError(const std::string& errorMessage) override {
        std::cout << "\n";
        LOG(name_, ">>> ERROR: " + errorMessage + " <<<");
        error_.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // 一時停止通知
    // -------------------------------------------------------------------------
    void onPaused() override {
        std::cout << "\n";
        LOG(name_, ">>> PAUSED <<<");
    }

    // -------------------------------------------------------------------------
    // 再開通知
    // -------------------------------------------------------------------------
    void onResumed() override {
        LOG(name_, ">>> RESUMED <<<");
    }

    // -------------------------------------------------------------------------
    // キャンセル通知
    // -------------------------------------------------------------------------
    void onCancelled() override {
        std::cout << "\n";
        LOG(name_, ">>> CANCELLED <<<");
        cancelled_.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // 終了検出用ヘルパー
    // -------------------------------------------------------------------------
    bool isFinished() const {
        return completed_.load(std::memory_order_acquire) ||
               error_.load(std::memory_order_acquire)     ||
               cancelled_.load(std::memory_order_acquire);
    }

    bool isCompleted()  const { return completed_.load(std::memory_order_acquire); }
    bool isError()      const { return error_.load(std::memory_order_acquire); }
    bool isCancelled()  const { return cancelled_.load(std::memory_order_acquire); }

private:
    std::string         name_;
    std::atomic<bool>   completed_{false};
    std::atomic<bool>   error_{false};
    std::atomic<bool>   cancelled_{false};
};

// =============================================================================
// ヘルパー: 最大タイムアウト付きで終了を待機する
// =============================================================================
bool waitForFinish(const ConsoleObserver& observer,
                   std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!observer.isFinished()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false; // タイムアウト
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

// =============================================================================
// デモシーケンス: pause → resume → cancel を実演する
// =============================================================================
void demoDownloadWithControls(const std::string& url,
                              const std::string& outputPath) {
    LOG("Demo", "=== Downloader Demo Start ===");
    LOG("Demo", "URL: " + url);
    LOG("Demo", "Output: " + outputPath);

    // Downloader 生成（デフォルト設定 + チャンクサイズ 1024 バイト明示）
    DownloaderConfig config;
    config.chunkSize = 1024; // 1 KB チャンク

    // 名前空間と同名のため完全修飾名を使用
    Downloader::Downloader downloader(config);

    // オブザーバーを登録
    ConsoleObserver observer("Main");
    downloader.addObserver(&observer);

    // --------------------------------------------------------
    // (1) ダウンロード開始
    // --------------------------------------------------------
    LOG("Demo", "Starting download...");
    if (!downloader.startDownload(url, outputPath)) {
        LOG("Demo", "Failed to start download (already running?)");
        return;
    }

    // 2 秒後に一時停止
    std::this_thread::sleep_for(std::chrono::seconds(2));

    if (!observer.isFinished()) {
        // --------------------------------------------------------
        // (2) 一時停止
        // --------------------------------------------------------
        LOG("Demo", "Pausing download...");
        downloader.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ダウンロードが一時停止されたことを確認
        auto stats = downloader.getStats();
        LOG("Demo", "State after pause: " +
            std::to_string(static_cast<int>(stats.state)));

        // 2 秒停止
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (!observer.isFinished()) {
            // --------------------------------------------------------
            // (3) 再開
            // --------------------------------------------------------
            LOG("Demo", "Resuming download...");
            downloader.resume();

            // 5 秒待ってまだ終わっていなければキャンセル
            bool finished = waitForFinish(observer, std::chrono::seconds(5));

            if (!finished && !observer.isFinished()) {
                // --------------------------------------------------------
                // (4) キャンセル
                // --------------------------------------------------------
                LOG("Demo", "Cancelling download (demo timeout)...");
                downloader.cancel();
            }
        }
    }

    // 完了・キャンセル・エラーのいずれかを待機（最大 10 秒）
    waitForFinish(observer, std::chrono::seconds(10));

    // 最終統計を表示
    auto stats = downloader.getStats();
    LOG("Demo", "Final stats:");
    LOG("Demo", "  Downloaded: " + std::to_string(stats.downloadedBytes) + " bytes");
    LOG("Demo", "  Total:      " + std::to_string(stats.totalBytes) + " bytes");
    LOG("Demo", "  Completed:  " + std::string(observer.isCompleted() ? "YES" : "NO"));
    LOG("Demo", "  Cancelled:  " + std::string(observer.isCancelled() ? "YES" : "NO"));
    LOG("Demo", "  Error:      " + std::string(observer.isError() ? "YES" : "NO"));
    LOG("Demo", "=== Downloader Demo End ===");
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char* argv[]) {
    // コマンドライン引数
    std::string url;
    std::string outputPath;

    if (argc >= 3) {
        url        = argv[1];
        outputPath = argv[2];
    } else if (argc >= 2) {
        url        = argv[1];
        outputPath = "downloaded_file";
    } else {
        // デフォルト: httpbin.org の 10MB ダウンロードエンドポイントを使用
        // ※インターネット接続が必要
        url        = "https://httpbin.org/bytes/10485760"; // 10 MB
        outputPath = "test_download.bin";

        LOG("Main", "Usage: downloader <url> <output_path>");
        LOG("Main", "Using default demo URL: " + url);
    }

    // デモ実行
    demoDownloadWithControls(url, outputPath);

    return 0;
}
