```mermaid
classDiagram

    %% ================================================================
    %% インターフェース
    %% ================================================================

    class IDownloaderObserver {
        <<interface>>
        +onProgress(downloadedBytes, totalBytes, percent)
        +onCompleted()
        +onError(errorMessage)
        +onPaused()
        +onResumed()
        +onCancelled()
    }

    class ICurlHandle {
        <<interface>>
        +setUrl(url)
        +setResumeFrom(startByte)
        +enableHttp2()
        +setWriteCallback(cb)
        +setProgressCallback(cb)
        +setConnectTimeout(seconds)
        +setUserAgent(ua)
        +setFollowLocation(follow)
        +setSslVerify(verify)
        +perform() CurlResult
        +getHttpResponseCode() long
        +getLastError() string
    }

    %% ================================================================
    %% 本番実装クラス
    %% ================================================================

    class CurlHandle {
        -CURL* handle_
        -WriteCallback writeCallback_
        -ProgressCallback progressCallback_
        -char errorBuffer_[256]
        +CurlHandle()
        +~CurlHandle()
        -curlWriteCallback()$
        -curlProgressCallback()$
        -toCurlResult(CURLcode) CurlResult
    }

    class Downloader {
        -DownloaderConfig config_
        -CurlFactory curlFactory_
        -mutex observerMutex_
        -vector~IDownloaderObserver*~ observers_
        -mutex statsMutex_
        -string url_
        -string outputPath_
        -atomic~int64_t~ downloadedBytes_
        -atomic~int64_t~ totalBytes_
        -atomic~DownloadState~ state_
        -mutex pauseMutex_
        -condition_variable pauseCv_
        -atomic~bool~ pauseRequested_
        -atomic~bool~ cancelRequested_
        -thread workerThread_
        +Downloader(config)
        +Downloader(config, curlFactory)
        +~Downloader()
        +addObserver(observer)
        +removeObserver(observer)
        +startDownload(url, outputPath) bool
        +pause()
        +resume()
        +cancel()
        +getStats() DownloadStats
        +getState() DownloadState
        -workerThread()
        -doDownload()
        -waitIfPaused() bool
        -notifyProgress()
        -notifyCompleted()
        -notifyError()
        -notifyPaused()
        -notifyResumed()
        -notifyCancelled()
    }

    %% ================================================================
    %% テスト用クラス
    %% ================================================================

    class MockCurlHandle {
        -MockConfig mockConfig_
        -string url_
        -int64_t resumeFrom_
        -WriteCallback writeCallback_
        -ProgressCallback progressCallback_
        -int performCallCount_
        -int resumeFromCallCount_
        +MockCurlHandle(config)
        +getUrl() string
        +getResumeFrom() int64_t
        +getPerformCallCount() int
    }

    class MockObserver {
        -mutex mutex_
        -condition_variable cv_
        -condition_variable pausedCv_
        -vector~ProgressRecord~ progressRecords_
        -int progressCallCount_
        -int completedCallCount_
        -int errorCallCount_
        -int pausedCallCount_
        -int resumedCallCount_
        -int cancelledCallCount_
        -string lastErrorMessage_
        +waitForFinish(timeout) bool
        +waitForPaused(timeout) bool
        +waitForProgress(count, timeout) bool
        +isCompleted() bool
        +isError() bool
        +isCancelled() bool
        +reset()
    }

    class ConsoleObserver {
        -string name_
        -atomic~bool~ completed_
        -atomic~bool~ error_
        -atomic~bool~ cancelled_
        +ConsoleObserver(name)
        +isFinished() bool
    }

    %% ================================================================
    %% 値型（struct）
    %% ================================================================

    class DownloaderConfig {
        <<struct>>
        +size_t chunkSize = 1024
        +long connectTimeoutSec = 30
        +bool useHttp2 = true
        +bool sslVerify = true
        +bool followRedirects = true
        +string userAgent
    }

    class DownloadStats {
        <<struct>>
        +int64_t downloadedBytes
        +int64_t totalBytes
        +double percent
        +DownloadState state
        +string url
        +string outputPath
    }

    class MockConfig {
        <<struct>>
        +size_t totalSize
        +size_t chunkSize
        +CurlResult returnResult
        +long httpCode
        +string errorMessage
        +bool supportsRange
        +milliseconds chunkDelay
    }

    class DownloadState {
        <<enumeration>>
        IDLE
        DOWNLOADING
        PAUSED
        COMPLETED
        CANCELLED
        ERROR
    }

    class CurlResult {
        <<enumeration>>
        OK
        ABORTED_BY_CALLBACK
        HTTP_ERROR
        NETWORK_ERROR
        RANGE_NOT_SATISFIED
        OTHER_ERROR
    }

    %% ================================================================
    %% 関係
    %% ================================================================

    %% 継承（実装）
    CurlHandle      ..|> ICurlHandle         : implements
    MockCurlHandle  ..|> ICurlHandle         : implements
    MockObserver    ..|> IDownloaderObserver  : implements
    ConsoleObserver ..|> IDownloaderObserver  : implements

    %% Downloader の依存
    Downloader o-- IDownloaderObserver : observes (0..*)
    Downloader ..> ICurlHandle         : creates via factory
    Downloader ..> DownloaderConfig    : uses
    Downloader ..> DownloadStats       : returns
    Downloader ..> DownloadState       : uses

    %% CurlHandle の依存
    CurlHandle ..> CurlResult : returns

    %% MockCurlHandle の依存
    MockCurlHandle ..> MockConfig  : uses
    MockCurlHandle ..> CurlResult  : returns

    %% テスト専用の依存
    MockObserver ..> IDownloaderObserver : implements for test
