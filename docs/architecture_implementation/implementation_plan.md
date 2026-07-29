# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（✅ 完了）**
- **[Step 3] SHT45 (温湿度センサ) の統合（← 現在のフェーズ）**
  - `SHT45` ドライバの実装
  - 共通データモデル (`EnvironmentData`) への実値反映
  - OLEDへの温湿度表示
- **[Step 4] BMP585 (気圧センサ) の統合**
- **[Step 5] SCD41 (CO2センサ) の統合**
- **[Step 6] MAX30102 (脈波・血中酸素センサ) 基礎**
- **[Step 7] MAX30102 信号処理**
- **[Step 8] システム全体の完成**

---

## User Review Required
> [!IMPORTANT]
> Step 2の動作確認ありがとうございました。続いて **Step 3** に進みます。
> SHT45センサのドライバとして、センシリオン公式の `Sensirion I2C SHT4x` ライブラリの追加を提案します。
> これを用いてSHT45ドライバクラスを作成し、`SensorManager` 内のモックデータを実際の計測値に置き換えます。
> 以下のStep 3の変更内容について問題がないか確認をお願いします。

## Proposed Changes (Step 3用)

### Core (共通データ型)
#### [NEW] [include/drivers/sensors/sensor_interface.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/sensor_interface.h)
- 全てのセンサドライバが実装すべき共通の基底クラス（インターフェース）`ISensor` および `IEnvironmentSensor` を定義します。

### Drivers (ドライバ層)
#### [NEW] [include/drivers/sensors/sht45_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/sht45_sensor.h)
- `IEnvironmentSensor` を継承する `Sht45Sensor` クラスを定義します。

#### [NEW] [src/drivers/sensors/sht45_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/sht45_sensor.cpp)
- センシリオン公式の `Sensirion I2C SHT4x` ライブラリを使用してSHT45の初期化、温湿度の読み出し、状態の管理を実装します。

### Services (サービス層)
#### [MODIFY] [include/services/sensor_manager.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/services/sensor_manager.h)
- `Sht45Sensor` のインスタンスを保持するようにクラスを修正します。

#### [MODIFY] [src/services/sensor_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sensor_manager.cpp)
- `begin()` 内で SHT45 の初期化を呼び出すように変更します。
- `update()` 内で SHT45 から実際の温湿度を読み出し、`EnvironmentData` の該当フィールドに反映するように変更します。他の値（CO2、気圧など）は後続ステップで実装するまで初期値（または無効な値）とします。

### Application (アプリケーション層)
#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- `lib_deps` に `sensirion/Sensirion I2C SHT4x` （および必要に応じて依存するCoreライブラリ）を追加します。

### Documentation
#### [MODIFY] [README.md](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/README.md)
- Step 3の進捗と追加ライブラリについて記載します。

## Verification Plan (Step 3)

### Automated Tests
- `pio run` コマンドによりコンパイルが通り、警告が出ないことを確認します。

### Manual Verification
- 実機への書き込み後、OLEDディスプレイの `T:` と `RH:` の部分に SHT45 から取得した実際の温湿度が表示され、リアルタイムに変化することを確認します。
- CO2と気圧(`P:`)は実機データの反映前のため、無効状態（例えば値が0、あるいは表示されない）となることを確認します。
