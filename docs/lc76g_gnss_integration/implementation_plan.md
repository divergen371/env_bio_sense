# LC76G GNSSモジュールの統合

LC76G GNSSモジュールのUART接続およびPPS・SD_CSのピン変更に伴い、GNSSデータの取得・記録・表示を行うためのドライバ実装およびシステム統合の計画です。

## Open Questions

> [!NOTE]
> 1. **GNSSデータのログ**: 既存の環境・生体データと一緒にSDカードへ記録する想定でよろしいでしょうか？
> 2. **PPSの利用**: PPS（D0）の1秒パルスは、内部時刻（RTC/NTP）の補正に使用しますか？それとも単にPPSが有効になったこと（フィックス状態）の判定として使用しますか？初回実装ではフィックス判定および高精度なタイムスタンプ付与に使用する方針です。

## Proposed Changes

### Configuration
#### [MODIFY] [platformio.ini](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/platformio.ini)
- NMEAセンテンスのパース用に `mikalhart/TinyGPSPlus` ライブラリを追加します。

#### [MODIFY] [pins.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/hal/pins.h)
- 適用済み: `SD_CS` を `D1`, `GNSS_PPS` を `D0` に変更。`MAX30102_INT` は `D2` のまま維持。
- 適用済み: `GNSS_TX` (D6), `GNSS_RX` (D7) の定義を追加。

---

### Core Data Structures
#### [MODIFY] [sensor_types.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/core/sensor_types.h)
- 緯度、経度、高度、速度、衛星数、HDOPなどを保持する `GnssData` 構造体を追加。
- `SensorId` と `DeviceState` に GNSS 用のエントリを追加。

#### [MODIFY] [sensor_snapshot.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/core/sensor_snapshot.h)
- `SensorSnapshot` 構造体に `GnssData gnss {};` を追加。
- `SystemStatus` に GNSS の状態を追加。

---

### Sensor Drivers
#### [NEW] [lc76g_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/lc76g_sensor.h)
#### [NEW] [lc76g_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/lc76g_sensor.cpp)
- `HardwareSerial` を用いたUART通信（デフォルトボーレート9600bps）。
- `TinyGPSPlus` を使用したバックグラウンドでのNMEAパース。
- GPIO1でのPPS割り込みハンドラの実装。

---

### Services & Application
#### [MODIFY] [main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- `Lc76gSensor` の初期化と定期的な `update()` 呼び出しの追加。
- `sensor_snapshot` へのデータ格納処理の追加。

#### [MODIFY] [sd_storage.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sd_storage.cpp)
- CSVのヘッダおよびデータ行にGNSS情報（緯度、経度、衛星数など）を追加。

#### [MODIFY] [display_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/display_manager.cpp)
- 画面にGNSSのフィックス状態や衛星数などを表示するUIの追加（表示項目の整理を伴う可能性あり）。

## Verification Plan

### Automated Tests
- コンパイルテスト: `pio run -e esp32s3` によるビルド成功の確認。

### Manual Verification
- ESP32-S3に書き込み、シリアルモニタでLC76Gの初期化とデータ取得（フィックスまで）が正常に行われるか確認。
- SDカードに生成されたCSVファイルにGNSSデータが正しく記録されているか確認。
- ユーザーに MAX30102_INT のピンについて回答いただき、コードへ反映したうえで動作を確認。
