# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（← 現在のフェーズ）**
  - 共通状態型（`DeviceState`, `ErrorCode`, `SensorId` 等）の定義
  - `SensorManager` と `DisplayManager` の骨格作成
  - SSD1306 (OLED) の統合
- **[Step 3] SHT45 (温湿度センサ) の統合**
- **[Step 4] BMP585 (気圧センサ) の統合**
- **[Step 5] SCD41 (CO2センサ) の統合**
- **[Step 6] MAX30102 (脈波・血中酸素センサ) 基礎**
- **[Step 7] MAX30102 信号処理**
- **[Step 8] システム全体の完成**

---

## User Review Required
> [!IMPORTANT]
> Step 1の動作確認ありがとうございました。続いて **Step 2** に進みます。
> 今回の実装では、SSD1306 OLEDディスプレイを描画するためのライブラリとして `Adafruit SSD1306` および `Adafruit GFX Library` の追加を提案します。（これらは `platformio.ini` に追加されます）
> 以下のStep 2の変更内容と採用ライブラリについて問題がないか確認をお願いします。

## Proposed Changes (Step 2用)

### Core (共通データ型)
#### [NEW] [include/core/sensor_types.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/core/sensor_types.h)
- `SensorId`, `DeviceState`, `ErrorCode` の enum class 定義を追加します。
- `EnvironmentData` などの測定値構造体を追加します。

#### [NEW] [include/core/sensor_snapshot.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/core/sensor_snapshot.h)
- センサ群の最新データを保持する構造体 `SensorSnapshot` と、システム全体の状態を保持する `SystemStatus` を追加します。

### Services (サービス層)
#### [NEW] [include/services/sensor_manager.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/sensor_manager.h)
- 全センサを統括し、初期化や更新処理、状態の集約を行う `SensorManager` クラスの骨格を追加します。（今回は骨格のみで実際のセンサは未接続）

#### [NEW] [src/services/sensor_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sensor_manager.cpp)
- `SensorManager` の仮実装。

#### [NEW] [include/services/display_manager.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/display_manager.h)
- 描画ライブラリの詳細を隠蔽し、アプリケーション層にAPIを提供する `DisplayManager` の定義。
- `ScreenId` の定義。

#### [NEW] [src/services/display_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/display_manager.cpp)
- `Adafruit_SSD1306` を内部で使用してディスプレイの初期化と初期画面の描画を行う実装。

### Drivers (ドライバ層)
#### [NEW] [include/drivers/display/display_interface.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/display/display_interface.h)
- 今回は `DisplayManager` 内部に閉じる形でも良いですが、仕様書に従いインターフェース化・SSD1306クラス化するか、直接Manager内に書くかを調整します。（今回は仕様書通り `DisplayManager` が直接描画をコントロールするシンプルな形とします）

### Application (アプリケーション層)
#### [MODIFY] [src/main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- 新設した `SensorManager` と `DisplayManager` を初期化・定期呼び出しするようループを更新します。

#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- `lib_deps` に `adafruit/Adafruit SSD1306` と `adafruit/Adafruit GFX Library` を追加します。

### Documentation
#### [MODIFY] [README.md](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/README.md)
- Step 2の進捗と追加ライブラリについて記載します。

## Verification Plan (Step 2)

### Automated Tests
- `pio run` コマンドによりコンパイルが通り、警告が出ないことを確認します。

### Manual Verification
- 実機への書き込み後、OLEDディスプレイに初期画面（値は初期値・空のテキストなど）が表示されることを確認します。
- シリアルコンソール上で `DisplayManager` の初期化成功ログが出力されることを確認します。
