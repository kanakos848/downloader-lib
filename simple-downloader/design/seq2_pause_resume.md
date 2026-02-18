```mermaid
sequenceDiagram
    actor       Client  as Client<br/>(main.cpp)
    participant DL      as Downloader
    participant Worker  as WorkerThread
    participant Curl    as ICurlHandle
    participant Obs     as IDownloaderObserver

    Note over Client,Obs: ⑤ pause → resume シナリオ

    %% ダウンロード中
    rect rgb(230, 255, 230)
        Client ->> DL: startDownload(url, path)
        DL ->> Worker: thread 起動
        activate Worker
        DL -->> Client: return true

        loop ダウンロード中
            Curl ->> Worker: writeCallback(chunk)
            Worker ->> Obs: onProgress(...)
        end
    end

    %% pause
    rect rgb(255, 235, 200)
        Note over Client,Obs: pause() 呼び出し
        Client ->> DL: pause()
        activate DL
        Note right of DL: compare_exchange:<br/>DOWNLOADING → PAUSED<br/>pauseRequested_ = true
        DL -->> Client: (return)
        deactivate DL

        Note right of Worker: 次の writeCallback で<br/>pauseRequested_ を検出
        Curl ->> Worker: writeCallback(next chunk)
        Worker ->> Worker: waitIfPaused()
        activate Worker
        Note right of Worker: condition_variable.wait()<br/>でブロック（CPU 消費ゼロ）
        Worker ->> Obs: onPaused()
        activate Obs
        Obs -->> Worker: (return)
        deactivate Obs
    end

    %% resume
    rect rgb(200, 240, 255)
        Note over Client,Obs: resume() 呼び出し
        Client ->> DL: resume()
        activate DL
        Note right of DL: compare_exchange:<br/>PAUSED → DOWNLOADING<br/>pauseRequested_ = false
        DL ->> Worker: pauseCv_.notify_all()
        DL -->> Client: (return)
        deactivate DL

        Note right of Worker: condition_variable から復帰
        Worker ->> Obs: onResumed()
        deactivate Worker

        loop ダウンロード再開
            Curl ->> Worker: writeCallback(chunk)
            Worker ->> Obs: onProgress(...)
        end

        Worker ->> Obs: onCompleted()
        deactivate Worker
    end
