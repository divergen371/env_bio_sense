# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

全体の移行は以下の段階（Step 1 〜 Step 8）に分けて安全に実施します。

- **[Step 1] HAL・共通基盤の整備（← 現在のフェーズ）**
  - `pins.h`, `i2c_bus`, `logger` の作成
  - 既存のI2Cスキャナの移行
  - `README.md` の更新
- **[Step 2] 状態管理とディスプレイ基盤**
  - 共通状態型（`DeviceState` 等）の定義
  - `SensorManager` と `DisplayManager` の骨格作成
  - SSD1306 (OLED) の統合
- **[Step 3] SHT45 (温湿度センサ) の統合**
  - `SHT45` ドライバの実装と `EnvironmentData` 型の作成
  - OLEDへの温湿度表示
- **[Step 4] BMP585 (気圧センサ) の統合**
  - `BMP585` ドライバの実装とOLEDへの気圧表示
  - 割り込み (INT) フラグ処理の骨格作成
- **[Step 5] SCD41 (CO2センサ) の統合**
  - `SCD41` ドライバの実装とOLEDへのCO2表示
  - 各センサの測定周期管理の導入
- **[Step 6] MAX30102 (脈波・血中酸素センサ) 基礎**
  - `MAX30102` ドライバの実装 (RAW取得)
  - FIFO処理とINTフラグの回収
- **[Step 7] MAX30102 信号処理**
  - `PpgProcessor` の実装（心拍、SpO2の計算）
  - 信号品質フラグの実装
- **[Step 8] システム全体の完成**
  - `EventBus` によるモジュール間通信
  - システム診断情報 (`Diagnostics`) の実装
  - エラーからの再初期化戦略の実装

---

## User Review Required
> [!IMPORTANT]
> 上記のロードマップ全体を一度に実装すると、障害の切り分けが困難になるため、まずは**Step 1**の変更から実施します。
> この方針とロードマップ、および下記のStep 1の具体的な変更内容に問題がないか確認をお願いします。

## Proposed Changes (Step 1用)

### HAL (ハードウェア抽象化)
#### [NEW] [include/hal/pins.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/hal/pins.h)
- センサおよびI2Cなどのピン定義（`I2C_SDA`, `I2C_SCL`, `MAX30102_INT`, `BMP585_INT`）を保持します。

#### [NEW] [include/hal/i2c_bus.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/hal/i2c_bus.h)
- I2Cバス管理クラス。`begin()` と `scan()` のインターフェースを定義します。

#### [NEW] [src/hal/i2c_bus.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/hal/i2c_bus.cpp)
- `Wire.begin` とI2Cスキャナの実装を配置します。

### Services (サービス層)
#### [NEW] [include/services/logger.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/logger.h)
- ログレベルの定義 (`LogLevel: Debug, Info, Warning, Error`) と Logger クラス。

#### [NEW] [src/services/logger.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/logger.cpp)
- `Serial` 出力を用いたロガーの実装。

### Application (アプリケーション層)
#### [MODIFY] [src/main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- 既存のピン定義およびI2Cスキャンロジックを削除。
- 移行した `I2cBus` と `Logger` を使用して起動時のログとピン状態の定期出力を再現します。

### Documentation
#### [MODIFY] [README.md](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/README.md)
- アーキテクチャ移行のロードマップとStep 1の進捗を反映します。

## Verification Plan (Step 1)

### Automated Tests
- `pio run` コマンドによりコンパイルが通り、警告が出ないことを確認します。

### Manual Verification
- 実機への書き込み後、シリアルモニタ上でI2C診断コードが機能し、接続済みのデバイスが正しく検出されることを確認します。
- `loop()` での割り込みピン状態の定期出力が引き続き機能していることを確認します。
