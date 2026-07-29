# XIAO ESP32S3 アーキテクチャ実装 (Step 1)

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリングの第一段階（Step 1）を実施します。
現状 `main.cpp` に直接記述されているピン定義、I2C初期化、I2Cスキャナなどの処理を、指定されたレイヤアーキテクチャに沿ってモジュール化します。

## User Review Required
> [!IMPORTANT]
> 仕様書の通り、今回は一度に全アーキテクチャを実装せず、Step 1として `pins.h`, `i2c_bus`, `logger`, `I2Cスキャナ` の移行と `README.md` の更新に留めます。
> また、ユーザー指定のルールに基づき、ドキュメントの保存場所を `docs/architecture_implementation/` 配下としています。

## Proposed Changes

### HAL (ハードウェア抽象化)
#### [NEW] [include/hal/pins.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/hal/pins.h)
- センサおよびI2Cなどのピン定義（`I2C_SDA`, `I2C_SCL`, `MAX30102_INT`, `BMP585_INT`）を `main.cpp` から移行します。

#### [NEW] [include/hal/i2c_bus.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/hal/i2c_bus.h)
- I2Cバス管理クラスの定義。`begin()` による初期化や、`scan()` によるデバイス走査のインターフェースを定義します。

#### [NEW] [src/hal/i2c_bus.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/hal/i2c_bus.cpp)
- `Wire.begin` を呼び出す処理と、既存のI2C診断ロジック（スキャナ）の実装を配置します。

### Services (サービス層)
#### [NEW] [include/services/logger.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/logger.h)
- ログレベルの定義 (`LogLevel: Debug, Info, Warning, Error`) と Logger クラスのインターフェース。

#### [NEW] [src/services/logger.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/logger.cpp)
- ログを標準出力 (`Serial`) にフォーマットして出力する処理の実装。

### Application (アプリケーション層)
#### [MODIFY] [src/main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- 既存のピン定義およびI2Cスキャンロジックを削除します。
- 移行した `I2cBus` と `Logger` モジュールを使用して、同等以上の起動ログと定期的なピン状態の出力を行うようリファクタリングします。

### Documentation
#### [MODIFY] [README.md](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/README.md)
- 現在のアーキテクチャ移行状態（Step 1）を記載します。
- プロジェクトの目標と現在の進捗を更新します。

## Verification Plan

### Automated Tests
- `pio run` コマンドによりコンパイル・ビルドが成功し、新規の警告(Warning)が出ないことを確認します。

### Manual Verification
- 実機への書き込み後、シリアルモニタ上でI2C診断コードが機能し、接続済みのデバイスが正しく検出されることを確認していただきます。
- 現在実装されている `loop()` での割り込みピン状態の定期出力が引き続き機能していることを確認していただきます。
