# XIAO ESP32S3 アーキテクチャ実装 (Step 3) タスクリスト

- `[x]` `platformio.ini` に `Sensirion I2C SHT4x` と `Sensirion Core` ライブラリ依存を追加する
- `[x]` `include/drivers/sensors/sensor_interface.h` を作成する
- `[x]` `include/drivers/sensors/sht45_sensor.h` を作成する
- `[x]` `src/drivers/sensors/sht45_sensor.cpp` を作成する
- `[x]` `include/services/sensor_manager.h` にSHT45インスタンスを追加する
- `[x]` `src/services/sensor_manager.cpp` で実データ取得ロジックを追加する
- `[x]` `README.md` を更新する
- `[ ]` コンパイルと実機確認を依頼する
