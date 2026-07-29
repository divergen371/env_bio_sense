# XIAO ESP32S3 アーキテクチャ実装計画 (環境異常・空気循環の検出)

## 目的
SHT45（外気用・高精度温湿度）と SCD41（筐体内部・CO2測定用NDIR）の温湿度データを比較し、温度差（ΔT）から「筐体内の熱ごもり（通気不良）」などの設置環境の異常を検出する機能を実装します。

## 背景
SCD41は内部にNDIR光源（赤外線ヒーター）を持つため、稼働中は自己発熱します。通常、空気の循環が良い状態（あるいは基板設計で熱分離ができている状態）であれば SHT45（外気）との温度差は 2〜3℃ 程度に収まります。しかし、密閉されたケースに入れたり、空気の流れがない場所に置いたりすると、SCD41の熱が筐体内にこもり、温度差が 5℃ 以上に拡大します。これを利用して「エアフローの異常」を検知します。

## User Review Required
> [!IMPORTANT]
> - 筐体内温度（SCD41）が外気（SHT45）より **5.0℃以上** 高い場合を「熱ごもり（Heat Trapped）」として警告を出す仕様とします。
> - この温度差の閾値（5.0℃）は、実際に動作させてみて後から微調整可能です。この方針で実装してよろしいでしょうか？

## Proposed Changes

### 1. センサデータの拡張 (`include/core/sensor_types.h`)
- `EnvironmentData` に SCD41 側の温湿度（`scd41TemperatureC`, `scd41HumidityRh`）を記録するフィールドを追加します。
- 異常状態を示す列挙型 `EnclosureWarning`（`Normal`, `HeatTrapped`）を追加します。

### 2. SCD41 ドライバの更新
#### [MODIFY] [include/drivers/sensors/scd41_sensor.h](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/include/drivers/sensors/scd41_sensor.h)
#### [MODIFY] [src/drivers/sensors/scd41_sensor.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/drivers/sensors/scd41_sensor.cpp)
- これまでは CO2 のみを取得していましたが、`readMeasurement` で同時に取得できている温度・湿度の補償値もクラス内に保存し、`readEnvironment()` 時に `EnvironmentData` に書き込むように変更します。

### 3. センサマネージャーでの比較・判定ロジック
#### [MODIFY] [src/services/sensor_manager.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/services/sensor_manager.cpp)
- `update()` メソッド内で各センサからのデータ取得後、SHT45 と SCD41 の温度差（ΔT = SCD41 Temp - SHT45 Temp）を計算します。
- ΔT が `5.0` 以上の場合、`enclosureWarning = HeatTrapped` と判定します。

### 4. 警告の出力
#### [MODIFY] [src/main.cpp](file:///Users/atsushi/Documents/PlatformIO/Projects/env_bio_sense/src/main.cpp)
- メインループのシリアルログ出力にて、`HeatTrapped` 状態の場合は `[WARN] MAIN: Enclosure Heat Trapped (Diff: +X.X C)` といった警告を出力するようにします。

## Verification Plan
1. そのまま机の上に置いて動作させ、SHT45とSCD41の温度差（ΔT）が通常どれくらいか（例: +2.5℃等）をシリアルログで確認する。
2. センサ全体に指をかぶせたり、小さな箱や袋を被せたりして意図的に通気を悪くする。
3. 筐体内に熱がこもり、ΔT が 5.0℃ を超えた時点で `Heat Trapped` の警告ログが出ることを確認する。
