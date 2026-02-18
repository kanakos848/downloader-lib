# CppDownloader - Windows ビルド手順

## 動作確認環境

| 項目 | バージョン |
|------|-----------|
| OS | Windows 10/11 (64-bit) |
| コンパイラ | MSVC 2022 (Visual Studio 2022) |
| CMake | 3.20 以上 |
| vcpkg | 最新 |
| libcurl | 8.x (vcpkg でインストール) |

---

## 1. 前提ソフトウェアのインストール

### Visual Studio 2022
1. [Visual Studio 2022](https://visualstudio.microsoft.com/) をインストール
2. **「C++ によるデスクトップ開発」** ワークロードを選択

### CMake
```powershell
# winget を使う場合
winget install Kitware.CMake

# または公式サイトからインストーラをダウンロード
# https://cmake.org/download/
```

### vcpkg（パッケージマネージャー）
```powershell
# C:\vcpkg にインストールする例
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# 環境変数を設定（永続化）
[System.Environment]::SetEnvironmentVariable(
    "VCPKG_ROOT", "C:\vcpkg",
    [System.EnvironmentVariableTarget]::User
)
```

---

## 2. 依存ライブラリのインストール

```powershell
# libcurl をインストール（HTTP/2 (nghttp2) と OpenSSL 付き）
C:\vcpkg\vcpkg.exe install curl[http2,openssl]:x64-windows

# インストール確認
C:\vcpkg\vcpkg.exe list | Select-String "curl"
```

> **注意**: GoogleTest は CMakeLists.txt の `FetchContent` で自動取得されます。
> 手動インストールは不要です。

---

## 3. プロジェクトのビルド

```powershell
# ソースディレクトリに移動
cd path\to\cpp-downloader

# ビルドディレクトリを作成
mkdir build
cd build

# CMake 設定（vcpkg toolchain を指定）
cmake .. `
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_TESTS=ON `
    -A x64

# ビルド
cmake --build . --config Release --parallel

# または Visual Studio で開く場合
# cmake --open .
```

---

## 4. 実行

### ダウンローダー本体

```powershell
# ヘルプ表示（引数なしで起動すると default URL でデモ）
.\Release\downloader.exe

# URL を指定してダウンロード
.\Release\downloader.exe https://example.com/file.zip output.zip

# 動作シーケンス:
#   1. ダウンロード開始
#   2. 2秒後に一時停止
#   3. 2秒後に再開
#   4. 5秒以内に完了しなければキャンセル
```

---

## 5. ユニットテストの実行

```powershell
# CTest でテスト実行
cd build
ctest -C Release --output-on-failure

# GoogleTest 直接実行（詳細ログ付き）
.\Release\DownloaderTests.exe --gtest_color=yes

# 特定のテストだけ実行
.\Release\DownloaderTests.exe --gtest_filter="DownloaderTest.Cancel*"

# テスト一覧表示
.\Release\DownloaderTests.exe --gtest_list_tests
```

---

## 6. デバッグビルド

```powershell
# Debug ビルド（アドレスサニタイザー等を有効にする場合）
cmake .. `
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" `
    -DCMAKE_BUILD_TYPE=Debug `
    -DBUILD_TESTS=ON `
    -A x64

cmake --build . --config Debug --parallel

# デバッグテスト実行
.\Debug\DownloaderTests.exe
```

---

## 7. 必要ライブラリ一覧

| ライブラリ | バージョン | 用途 | 取得方法 |
|-----------|-----------|------|---------|
| libcurl | 8.x | HTTP/HTTPS ダウンロード、Range リクエスト | vcpkg |
| nghttp2 | 1.x | HTTP/2 サポート (curl 依存) | vcpkg (curl[http2]) |
| OpenSSL | 3.x | HTTPS/TLS (curl 依存) | vcpkg (curl[openssl]) |
| GoogleTest | 1.14.0 | ユニットテストフレームワーク | CMake FetchContent |

---

## 8. プロジェクト構造

```
cpp-downloader/
├── CMakeLists.txt          # CMake ビルド設定
├── README.md               # このファイル
├── include/
│   ├── IDownloaderObserver.h  # Observer インターフェース
│   ├── ICurlHandle.h          # curl 抽象化インターフェース
│   ├── CurlHandle.h           # 本番 curl 実装
│   └── Downloader.h           # Downloader メインクラス
├── src/
│   ├── CurlHandle.cpp         # curl RAII ラッパー実装
│   ├── Downloader.cpp         # ダウンローダー実装
│   └── main.cpp               # サンプル・動作確認
└── tests/
    ├── MockCurlHandle.h       # テスト用 curl モック
    ├── MockObserver.h         # テスト用 Observer モック
    └── DownloaderTest.cpp     # GoogleTest ユニットテスト
```

---

## 9. トラブルシューティング

### curl が見つからない
```
Could not find a package configuration file provided by "CURL"
```
→ vcpkg の toolchain パスが正しいか確認してください。  
→ `C:\vcpkg\vcpkg.exe install curl[http2,openssl]:x64-windows` を再実行してください。

### HTTP/2 が動作しない
nghttp2 が含まれているか確認:
```powershell
.\Release\downloader.exe --version  # curl バージョン情報を表示
```

### GoogleTest のダウンロードに失敗する
企業プロキシ環境では FetchContent が失敗することがあります。その場合は:
```powershell
# vcpkg で GTest をインストール
C:\vcpkg\vcpkg.exe install gtest:x64-windows

# CMakeLists.txt の FetchContent 部分をコメントアウトし、以下に置き換え:
# find_package(GTest REQUIRED)
```

### スレッドリークの検出
Visual Studio の診断ツールや、AddressSanitizer を使用してください:
```powershell
cmake .. -DCMAKE_CXX_FLAGS="/fsanitize=address"
```

---

## 10. 使用例コード

```cpp
#include "Downloader.h"
#include "IDownloaderObserver.h"

class MyObserver : public Downloader::IDownloaderObserver {
public:
    void onProgress(int64_t downloaded, int64_t total, double percent) override {
        printf("Progress: %.1f%%\n", percent);
    }
    void onCompleted() override { puts("Done!"); }
    void onError(const std::string& msg) override { printf("Error: %s\n", msg.c_str()); }
    void onPaused() override { puts("Paused"); }
    void onResumed() override { puts("Resumed"); }
    void onCancelled() override { puts("Cancelled"); }
};

int main() {
    Downloader::Downloader downloader;
    MyObserver observer;
    downloader.addObserver(&observer);

    downloader.startDownload("https://example.com/file.zip", "file.zip");

    // pause / resume / cancel
    std::this_thread::sleep_for(std::chrono::seconds(2));
    downloader.pause();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    downloader.resume();
    // downloader.cancel(); // キャンセルしたい場合

    // Downloader のデストラクタが自動的にスレッドを終了する (RAII)
}
```
