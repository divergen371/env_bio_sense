# XIAO ESP32S3 アーキテクチャ実装計画およびロードマップ

## 目的
XIAO ESP32S3 センサプラットフォームのアーキテクチャ仕様書に基づくリファクタリング計画です。
システムの将来的な拡張（通信、記録、画面拡張など）に備え、ベタ書きされている現在の実装を段階的にレイヤアーキテクチャへ移行します。

## 開発ロードマップ (アーキテクチャ仕様書に基づく)

- **[Step 1] HAL・共通基盤の整備（✅ 完了）**
- **[Step 2] 状態管理とディスプレイ基盤（✅ 完了）**
- **[Step 3] SHT45 (温湿度センサ) の統合（✅ 完了）**
- **[Step 4] BMP585 (気圧センサ) の統合（← 現在のフェーズ: 公式APIへの移行）**
  - `BMP585` ドライバの実装（Bosch公式C APIを使用）
  - I2C通信ラッパーの実装
  - 気圧表示のOLEDへの統合
- **[Step 5] SCD41 (CO2センサ) の統合**
- **[Step 6] MAX30102 (脈波・血中酸素センサ) 基礎**
- **[Step 7] MAX30102 信号処理**
- **[Step 8] システム全体の完成**

---

## User Review Required
> [!IMPORTANT]
> ご要望に基づき、BMP585のドライバを **Bosch公式のC言語API (`BMP5_SensorAPI`)** に切り替える計画に変更しました。
> 公式APIは純粋なC言語ライブラリであるため、I2C通信をArduinoの `Wire` に仲介するためのラッパー関数（HAL）を自前で実装する必要があります。本格的な組み込み開発の構成になります。
> 以下の変更内容で実装を進めてよろしいかご確認ください。

## Proposed Changes (Step 4: Bosch公式APIへの移行)

### Application (アプリケーション層)
#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- `adafruit/Adafruit BMP5xx Library` を削除し、代わりにBosch公式のリポジトリ `https://github.com/boschsensortec/BMP5_SensorAPI.git` を追加します。

### Drivers (ドライバ層)
#### [MODIFY] [include/drivers/sensors/bmp585_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/bmp585_sensor.h)
- `Adafruit_BMP5xx` のインクルードとメンバ変数を削除し、代わりに公式APIの `bmp5.h` をインクルードして `struct bmp5_dev` を保持するように変更します。

#### [MODIFY] [src/drivers/sensors/bmp585_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/bmp585_sensor.cpp)
- Bosch公式APIが要求する以下のインターフェース関数（C言語コールバック）を実装します：
  - I2C読み取り関数
  - I2C書き込み関数
  - マイクロ秒遅延関数
- これらの関数を `bmp5_dev` 構造体にバインドし、公式APIの `bmp5_init()` や `bmp5_get_sensor_data()` を呼び出して気圧を取得するようにロジックを書き換えます。

### Services (サービス層)
- `SensorManager` 側のロジックはすでにインターフェース（`Bmp585Sensor`）経由で抽象化されているため、変更の必要はありません。

## Verification Plan (Step 4)

### Automated Tests
- `pio run` コマンドによりコンパイルが通り、公式C APIとC++ラッパーの結合部分でエラーが発生しないことを確認します。

### Manual Verification
- 実機への書き込み後、OLEDディスプレイの `P:` に気圧値が正常に表示されるか（前回のAdafruit版と同様に動作するか）を確認します。
