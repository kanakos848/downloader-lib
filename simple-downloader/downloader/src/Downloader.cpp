// =============================================================================
// Downloader.cpp
// HTTP ファイルダウンローダーのメイン実装
//
// 設計方針:
//  - ワーカースレッドが curl を介してデータを受信し、ファイルに書き込む
//  - 一時停止は condition_variable で実装（CPU を消費しない待機）
//  - キャンセルは atomic フラグで curl コールバックから中断する
//  - レジュームは CURLOPT_RESUME_FROM_LARGE で実現（HTTP Range ヘッダー）
//  - 1024 バイトのチャンクバッファで低メモリを維持
// =============================================================================

#include "Downloader.h"
#include "CurlHandle.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h> // CURL グローバル初期化のために必要な場合がある
#endif

namespace Downloader {

// =============================================================================
// グローバル curl 初期化 (プロセス単位で一度だけ実施)
// =============================================================================

/// RAII でプロセス全体の curl グローバル状態を管理するクラス
/// 複数の Downloader インスタンスが共存してもセーフ
class CurlGlobalInit {
public:
    CurlGlobalInit() {
        // curl_global_init はスレッドセーフではないため、
        // 静的初期化（シングルトン）でプロセス開始時に一度だけ呼ぶ
        curl_global_init(CURL_GLOBAL_ALL);
    }
    ~CurlGlobalInit() {
        curl_global_cleanup();
    }
};

// プログラム起動時に一度だけ初期化される
static CurlGlobalInit g_curlGlobalInit;

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================

Downloader::Downloader(DownloaderConfig config)
    : config_(std::move(config))
    , curlFactory_([]() -> std::unique_ptr<ICurlHandle> {
          return std::make_unique<CurlHandle>();
      }) {
    // write バッファを設定されたチャンクサイズで事前確保（低メモリ設計）
    writeBuffer_.resize(config_.chunkSize);
}

Downloader::Downloader(DownloaderConfig config, CurlFactory curlFactory)
    : config_(std::move(config))
    , curlFactory_(std::move(curlFactory)) {
    writeBuffer_.resize(config_.chunkSize);
}

Downloader::~Downloader() {
    // RAII: デストラクタでワーカースレッドを確実に終了させる
    cancel();

    // ワーカースレッドが起動していれば join して完全終了を待つ
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

// =============================================================================
// Observer 管理
// =============================================================================

void Downloader::addObserver(IDownloaderObserver* observer) {
    if (!observer) return;
    std::lock_guard<std::mutex> lock(observerMutex_);
    // 重複登録を防ぐ
    auto it = std::find(observers_.begin(), observers_.end(), observer);
    if (it == observers_.end()) {
        observers_.push_back(observer);
    }
}

void Downloader::removeObserver(IDownloaderObserver* observer) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    observers_.erase(
        std::remove(observers_.begin(), observers_.end(), observer),
        observers_.end());
}

// =============================================================================
// ダウンロード制御
// =============================================================================

bool Downloader::startDownload(const std::string& url,
                               const std::string& outputPath) {
    // 既に実行中の場合は拒否する
    DownloadState current = state_.load(std::memory_order_acquire);
    if (current == DownloadState::DOWNLOADING ||
        current == DownloadState::PAUSED) {
        return false;
    }

    // 前回のスレッドが残っていれば join して完全終了を待つ
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    // 状態をリセットする
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        url_        = url;
        outputPath_ = outputPath;
    }
    downloadedBytes_.store(0, std::memory_order_relaxed);
    totalBytes_.store(0, std::memory_order_relaxed);
    pauseRequested_.store(false, std::memory_order_release);
    cancelRequested_.store(false, std::memory_order_release);

    state_.store(DownloadState::DOWNLOADING, std::memory_order_release);

    // ワーカースレッドを起動する
    workerThread_ = std::thread(&Downloader::workerThread, this);

    return true;
}

void Downloader::pause() {
    DownloadState expected = DownloadState::DOWNLOADING;
    if (state_.compare_exchange_strong(expected, DownloadState::PAUSED,
                                       std::memory_order_acq_rel)) {
        // pause フラグを立てる（curl コールバック内で検出する）
        pauseRequested_.store(true, std::memory_order_release);
    }
}

void Downloader::resume() {
    DownloadState expected = DownloadState::PAUSED;
    if (state_.compare_exchange_strong(expected, DownloadState::DOWNLOADING,
                                       std::memory_order_acq_rel)) {
        pauseRequested_.store(false, std::memory_order_release);
        // 一時停止中のワーカースレッドを起こす
        pauseCv_.notify_all();
    }
}

void Downloader::cancel() {
    // IDLE 以外の全状態からキャンセル可能
    cancelRequested_.store(true, std::memory_order_release);
    pauseRequested_.store(false, std::memory_order_release);

    // 一時停止中のワーカーが condition_variable で待機している場合は起こす
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        // ロックを取得することで、ワーカーが cv.wait() に入る前/後のどちらの
        // タイミングでキャンセルされても確実に起こせる
    }
    pauseCv_.notify_all();
}

// =============================================================================
// 状態取得
// =============================================================================

DownloadStats Downloader::getStats() const {
    DownloadStats stats;
    stats.state         = state_.load(std::memory_order_acquire);
    stats.downloadedBytes = downloadedBytes_.load(std::memory_order_relaxed);
    stats.totalBytes    = totalBytes_.load(std::memory_order_relaxed);

    if (stats.totalBytes > 0) {
        stats.percent = static_cast<double>(stats.downloadedBytes) /
                        static_cast<double>(stats.totalBytes) * 100.0;
    } else {
        stats.percent = -1.0;
    }

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats.url        = url_;
    stats.outputPath = outputPath_;

    return stats;
}

DownloadState Downloader::getState() const {
    return state_.load(std::memory_order_acquire);
}

// =============================================================================
// ワーカースレッド
// =============================================================================

void Downloader::workerThread() {
    // 例外はすべてここでキャッチして onError に変換する
    try {
        doDownload();
    } catch (const std::exception& e) {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError(std::string("Unexpected exception: ") + e.what());
    } catch (...) {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Unknown exception in worker thread");
    }
}

void Downloader::doDownload() {
    // --------------------------------------------------------
    // (1) 出力ファイルを開く
    //     レジューム対応のため、既存ファイルがあれば追記モードで開く
    // --------------------------------------------------------
    std::string outputPath;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        outputPath = outputPath_;
        url        = url_;
    }

    // 既存ファイルのサイズを確認してレジューム位置を決定する
    int64_t resumeFrom = 0;
    {
        std::ifstream existing(outputPath, std::ios::binary | std::ios::ate);
        if (existing.is_open()) {
            resumeFrom = static_cast<int64_t>(existing.tellg());
        }
    }
    downloadedBytes_.store(resumeFrom, std::memory_order_relaxed);

    // ファイルを追記モードで開く
    std::ofstream outFile(outputPath,
                          std::ios::binary |
                          (resumeFrom > 0 ? std::ios::app : std::ios::trunc));
    if (!outFile.is_open()) {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Failed to open output file: " + outputPath);
        return;
    }

    // --------------------------------------------------------
    // (2) curl ハンドルを初期化する（ファクトリで生成）
    // --------------------------------------------------------
    auto curl = curlFactory_();
    if (!curl) {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Failed to create curl handle");
        return;
    }

    curl->setUrl(url);
    curl->setConnectTimeout(config_.connectTimeoutSec);
    curl->setUserAgent(config_.userAgent);
    curl->setFollowLocation(config_.followRedirects);
    curl->setSslVerify(config_.sslVerify);

    if (config_.useHttp2) {
        curl->enableHttp2();
    }

    // レジューム位置を設定する（0 の場合は通常のダウンロード）
    if (resumeFrom > 0) {
        curl->setResumeFrom(resumeFrom);
    }

    // --------------------------------------------------------
    // (3) 書き込みコールバック設定
    //     1024 バイトのチャンクでストリーミング書き込みする
    // --------------------------------------------------------
    curl->setWriteCallback([this, &outFile](const char* data, size_t size) -> size_t {
        // キャンセル検出: 書き込みコールバック内でフラグを確認
        if (cancelRequested_.load(std::memory_order_acquire)) {
            // 0 を返すと curl が CURLE_WRITE_ERROR を発生させてダウンロードを中断する
            return 0;
        }

        // 一時停止検出
        if (pauseRequested_.load(std::memory_order_acquire)) {
            // 一時停止状態に遷移（すでに PAUSED になっているはずだが念のため）
            // 書き込み前に一時停止を待機する
            bool shouldContinue = waitIfPaused();
            if (!shouldContinue) {
                return 0; // キャンセルされた場合
            }
        }

        // ファイルに書き込む
        outFile.write(data, static_cast<std::streamsize>(size));
        if (outFile.fail()) {
            return 0; // 書き込みエラー
        }

        // ダウンロード済みバイト数を更新する
        downloadedBytes_.fetch_add(static_cast<int64_t>(size),
                                   std::memory_order_relaxed);

        return size; // 書き込んだバイト数を返す
    });

    // --------------------------------------------------------
    // (4) 進捗コールバック設定
    // --------------------------------------------------------
    curl->setProgressCallback([this](int64_t dltotal, int64_t dlnow) -> int {
        // キャンセル検出
        if (cancelRequested_.load(std::memory_order_acquire)) {
            return 1; // 非0を返すと curl が中断する
        }

        // 総バイト数を更新する（レジューム時は既存ファイルサイズを加算）
        const int64_t baseOffset = downloadedBytes_.load(std::memory_order_relaxed) -
                                   dlnow; // 現在セッションの dlnow を除いた base
        // 注: dltotal は「今回のセッションでの」期待サイズ
        //     レジューム時は resumeFrom が加算されるが、
        //     サーバによってはレジューム後の残りサイズだけを返すことがある
        if (dltotal > 0) {
            totalBytes_.store(dltotal + baseOffset, std::memory_order_relaxed);
        }

        // 進捗通知
        const int64_t downloaded = downloadedBytes_.load(std::memory_order_relaxed);
        const int64_t total      = totalBytes_.load(std::memory_order_relaxed);
        double percent = -1.0;
        if (total > 0) {
            percent = static_cast<double>(downloaded) /
                      static_cast<double>(total) * 100.0;
        }
        notifyProgress(downloaded, total, percent);

        return 0; // 継続
    });

    // --------------------------------------------------------
    // (5) ダウンロード実行
    // --------------------------------------------------------
    CurlResult result = curl->perform();

    // ファイルを閉じる（スコープアウトで自動的に閉じられるが明示的にフラッシュ）
    outFile.flush();

    // --------------------------------------------------------
    // (6) 結果処理
    // --------------------------------------------------------

    // キャンセルチェック（コールバックからの中断はキャンセル扱い）
    if (cancelRequested_.load(std::memory_order_acquire)) {
        state_.store(DownloadState::CANCELLED, std::memory_order_release);
        notifyCancelled();
        return;
    }

    // 一時停止中にここに到達した場合（レジューム待ち中にキャンセルされた etc.）
    if (state_.load(std::memory_order_acquire) == DownloadState::PAUSED) {
        // pause() → waitIfPaused() でキャンセルが返った場合はここに来ない
        // この分岐は安全のためのガード
        state_.store(DownloadState::CANCELLED, std::memory_order_release);
        notifyCancelled();
        return;
    }

    if (result == CurlResult::OK) {
        // HTTP レスポンスコードを確認する（4xx/5xx はエラー）
        const long httpCode = curl->getHttpResponseCode();
        if (httpCode >= 400) {
            state_.store(DownloadState::ERROR, std::memory_order_release);
            notifyError("HTTP error: " + std::to_string(httpCode));
        } else {
            // 完了: 100% の進捗通知を出してから完了通知
            const int64_t total = totalBytes_.load(std::memory_order_relaxed);
            const int64_t downloaded = downloadedBytes_.load(std::memory_order_relaxed);
            notifyProgress(downloaded, total > 0 ? total : downloaded, 100.0);

            state_.store(DownloadState::COMPLETED, std::memory_order_release);
            notifyCompleted();
        }
    } else if (result == CurlResult::ABORTED_BY_CALLBACK) {
        // コールバックからの中断は cancel とは別扱い（書き込みエラーなど）
        // cancelRequested_ チェックは上で済んでいるのでここはエラー
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Download aborted: " + curl->getLastError());
    } else if (result == CurlResult::NETWORK_ERROR) {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Network error: " + curl->getLastError());
    } else if (result == CurlResult::RANGE_NOT_SATISFIED) {
        // サーバが Range をサポートしていない場合はエラー
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Server does not support resume (Range not satisfied)");
    } else {
        state_.store(DownloadState::ERROR, std::memory_order_release);
        notifyError("Download failed: " + curl->getLastError());
    }
}

// =============================================================================
// 一時停止待機
// =============================================================================

bool Downloader::waitIfPaused() {
    std::unique_lock<std::mutex> lock(pauseMutex_);

    // onPaused 通知は一度だけ出す（pauseRequested_ が初めて true になった時）
    notifyPaused();

    // 一時停止が解除されるか、キャンセルされるまで待機する
    pauseCv_.wait(lock, [this]() {
        return !pauseRequested_.load(std::memory_order_acquire) ||
               cancelRequested_.load(std::memory_order_acquire);
    });

    if (cancelRequested_.load(std::memory_order_acquire)) {
        return false; // キャンセル
    }

    // 再開通知
    notifyResumed();
    return true; // 再開
}

// =============================================================================
// Observer 通知ヘルパー
// =============================================================================

void Downloader::notifyProgress(int64_t downloaded, int64_t total, double percent) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onProgress(downloaded, total, percent);
    }
}

void Downloader::notifyCompleted() {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onCompleted();
    }
}

void Downloader::notifyError(const std::string& message) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onError(message);
    }
}

void Downloader::notifyPaused() {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onPaused();
    }
}

void Downloader::notifyResumed() {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onResumed();
    }
}

void Downloader::notifyCancelled() {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (auto* obs : observers_) {
        obs->onCancelled();
    }
}

} // namespace Downloader
