# XIAO ESP32S3 アーキテクチャ実装 (Step 4: Bosch公式APIへの移行) タスクリスト

- `[x]` `platformio.ini` の `Adafruit BMP5xx` を `https://github.com/boschsensortec/BMP5_SensorAPI.git` に置き換える
- `[x]` `include/drivers/sensors/bmp585_sensor.h` を Bosch `bmp5_dev` 構造体を使用するように修正する
- `[x]` `src/drivers/sensors/bmp585_sensor.cpp` にI2Cラッパー(HAL)を実装し、初期化とデータ取得を公式APIに切り替える
- `[x]` `README.md` を更新する
- `[ ]` コンパイルと実機確認を依頼する
