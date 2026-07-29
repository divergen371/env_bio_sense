# XIAO ESP32S3 アーキテクチャ実装 (Step 5: SCD41 CO2センサの統合) タスクリスト

- `[x]` `platformio.ini` に `sensirion/Sensirion I2C SCD4x` を追加する
- `[x]` `include/drivers/sensors/scd41_sensor.h` を作成し、クラスインターフェースを定義する
- `[x]` `src/drivers/sensors/scd41_sensor.cpp` に初期化と定期読み取り(Periodic Measurement)処理を実装する
- `[x]` `SensorManager` で SCD41 の初期化と更新を行い、CO2濃度を反映させる
- `[x]` `DisplayManager` に CO2濃度の表示 (`C: XXXX ppm`) を追加する
- `[x]` `README.md` と `walkthrough.md` を更新する
- `[x]` コンパイルと実機確認を依頼する
