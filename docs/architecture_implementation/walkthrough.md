# XIAO ESP32S3 アーキテクチャ実装 (Step 4) 完了報告

アーキテクチャ仕様書に基づくStep 4の実装が完了しました。

## 実装内容
- **PlatformIO Configuration**
  - `platformio.ini`: 気圧センサ用ライブラリとして、Bosch公式のGitHubリポジトリ `https://github.com/boschsensortec/BMP5_SensorAPI.git` を追加しました。
- **Drivers (ドライバ層)**
  - `include/drivers/sensors/bmp585_sensor.h` / `src/drivers/sensors/bmp585_sensor.cpp`: Bosch公式の `BMP5_SensorAPI` (C言語API) を用いて BMP585 の初期化と気圧測定を行うドライバを実装しました。
  - C言語APIとArduinoの `Wire` 通信を仲介するための、読み取り・書き込み・遅延のラッパー関数(HAL)を内部に実装しています。
- **Services (サービス層)**
  - `include/services/sensor_manager.h` / `src/services/sensor_manager.cpp`: SHT45と同様に `Bmp585Sensor` インスタンスを追加し、定期的にデータを読み出して `SensorSnapshot` の `pressureHpa` に値を反映するよう統合しました。

## 次のステップ
以下のコマンドでビルドが通るか、また実機に書き込んで **OLEDディスプレイの `P:` (気圧) が実際の BMP585 センサの測定値（およそ 1000〜1025 hPa）に更新されているか** をご確認ください。

```bash
pio run
pio run -t upload
pio device monitor
```

動作確認が取れ次第、Step 5（SCD41 CO2センサの統合）へ進みます。
