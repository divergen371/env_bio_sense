# XIAO ESP32S3 センサプラットフォーム アーキテクチャ仕様書
**対象:** Antigravity  
**プラットフォーム:** PlatformIO / Arduino Framework / Seeed Studio XIAO ESP32S3

## 1. 目的

本仕様書は、XIAO ESP32S3に接続した複数センサとOLED表示を、将来の通信・記録・画面拡張へ無理なく発展させるためのソフトウェア構造を定義する。

初期構成は以下とする。

- SSD1306 OLED
- SHT45
- BMP585
- SCD41
- MAX30102

設計上は、BLE、Wi-Fi、MQTT、SDカード、LittleFS、LVGL、FreeRTOS、OTA、nRF54L15との通信追加を妨げないこと。

---

## 2. 設計原則

- `main.cpp`は起動処理と全体協調だけを担当する
- センサ固有APIをアプリケーション層へ漏らさない
- センサ1台の故障で全体停止しない
- 各モジュールは状態を明示的に保持する
- 時刻依存処理はブロッキングさせない
- `delay()`は初期化上どうしても必要な短時間用途を除き使用しない
- ISR内でI²C通信、Serial出力、動的メモリ確保を行わない
- 動的確保を避け、固定長コンテナまたは静的オブジェクトを優先する
- エラーは握り潰さず、状態とログへ反映する
- 各層の依存方向を一方向に保つ

---

## 3. レイヤ構成

```text
Application
  ├─ AppController
  ├─ ViewModel / Snapshot
  └─ Use Cases

Services
  ├─ SensorManager
  ├─ DisplayManager
  ├─ Logger
  ├─ Scheduler
  └─ EventBus

Drivers
  ├─ Sht45Sensor
  ├─ Bmp585Sensor
  ├─ Scd41Sensor
  ├─ Max30102Sensor
  └─ Ssd1306Display

Hardware Abstraction
  ├─ I2cBus
  ├─ InterruptFlags
  ├─ Clock
  └─ Pins
```

依存方向は上位層から下位層へのみとする。

---

## 4. 推奨ディレクトリ構成

```text
include/
  app/
    app_controller.h

  core/
    result.h
    sensor_types.h
    sensor_snapshot.h
    system_status.h

  hal/
    pins.h
    i2c_bus.h
    clock.h
    interrupt_flags.h

  services/
    sensor_manager.h
    display_manager.h
    scheduler.h
    logger.h
    event_bus.h
    configuration_manager.h

  drivers/
    sensors/
      sensor_interface.h
      sht45_sensor.h
      bmp585_sensor.h
      scd41_sensor.h
      max30102_sensor.h
    display/
      display_interface.h
      ssd1306_display.h

src/
  main.cpp

  app/
    app_controller.cpp

  hal/
    i2c_bus.cpp
    clock.cpp
    interrupt_flags.cpp

  services/
    sensor_manager.cpp
    display_manager.cpp
    scheduler.cpp
    logger.cpp
    event_bus.cpp
    configuration_manager.cpp

  drivers/
    sensors/
      sht45_sensor.cpp
      bmp585_sensor.cpp
      scd41_sensor.cpp
      max30102_sensor.cpp
    display/
      ssd1306_display.cpp

test/
  test_sensor_manager/
  test_scheduler/
  test_formatters/

docs/
  wiring.md
  architecture.md
  diagnostics.md
```

---

## 5. 共通データ型

## 5.1 センサ識別子

```cpp
enum class SensorId : uint8_t {
    Sht45,
    Bmp585,
    Scd41,
    Max30102
};
```

## 5.2 センサ状態

```cpp
enum class DeviceState : uint8_t {
    Unknown,
    Initializing,
    Ready,
    Degraded,
    Offline,
    Error
};
```

## 5.3 エラーコード

```cpp
enum class ErrorCode : uint8_t {
    None,
    NotFound,
    InitFailed,
    ReadFailed,
    Timeout,
    InvalidData,
    BusError,
    Unsupported
};
```

## 5.4 測定値

センサごとの値を一つの巨大構造体へ詰め込まず、用途別に分離する。

```cpp
struct EnvironmentData {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    uint16_t co2Ppm {};
    uint32_t timestampMs {};
    bool valid {};
};

struct PpgData {
    uint32_t red {};
    uint32_t ir {};
    float heartRateBpm {};
    float spo2Percent {};
    uint32_t timestampMs {};
    bool rawValid {};
    bool calculatedValid {};
};
```

---

## 6. センサ共通インターフェース

全センサへ完全に同じデータ型を強制しない。共通化するのはライフサイクルと状態管理に限定する。

```cpp
class ISensor {
public:
    virtual ~ISensor() = default;

    virtual SensorId id() const = 0;
    virtual bool begin() = 0;
    virtual void update(uint32_t nowMs) = 0;

    virtual DeviceState state() const = 0;
    virtual ErrorCode lastError() const = 0;
    virtual uint32_t lastSuccessMs() const = 0;
};
```

測定値取得は各具体インターフェースへ分ける。

```cpp
class IEnvironmentSensor : public ISensor {
public:
    virtual bool readEnvironment(EnvironmentData& out) const = 0;
};

class IPpgSensor : public ISensor {
public:
    virtual bool readPpg(PpgData& out) const = 0;
};
```

これにより、SCD41、SHT45、BMP585の差異を残しつつ、管理方法だけを揃える。

---

## 7. SensorManager

SensorManagerは以下を担当する。

- 全センサの初期化
- 周期的な`update()`
- 状態集約
- 最新値のスナップショット生成
- 再初期化制御
- オフライン判定
- 異常センサを切り離した継続運転

```cpp
class SensorManager {
public:
    bool begin();
    void update(uint32_t nowMs);

    const SensorSnapshot& snapshot() const;
    const SystemStatus& status() const;

private:
    Sht45Sensor sht45_;
    Bmp585Sensor bmp585_;
    Scd41Sensor scd41_;
    Max30102Sensor max30102_;

    SensorSnapshot snapshot_ {};
    SystemStatus status_ {};
};
```

### 更新周期の目安

| デバイス | 更新周期 |
|---|---:|
| SHT45 | 1000 ms |
| BMP585 | 100〜1000 ms |
| SCD41 | 5000 ms程度、使用ライブラリ推奨に従う |
| MAX30102 | FIFOまたはサンプルレートに従う |
| OLED | 250〜1000 ms |

各センサ内部で次回実行時刻を管理してもよいが、共通Schedulerへ登録する方針を推奨する。

---

## 8. Scheduler

初期段階ではFreeRTOSタスクを乱立させず、協調型スケジューラを使用する。

```cpp
struct ScheduledTask {
    uint32_t intervalMs;
    uint32_t nextRunMs;
    void (*callback)();
    bool enabled;
};
```

実装条件:

- `millis()`のオーバーフローに耐える比較を行う
- 長時間ブロッキング処理を登録しない
- タスク実行時間を診断可能にする
- 優先度が必要になった時点でFreeRTOSへ移行する

メンタルモデルとしては、各処理を「順番待ちの小さな仕事」に分解し、一つが道路を塞がない構造とする。

---

## 9. EventBus

EventBusは疎結合な通知にのみ使う。測定値の常時配送をすべてイベント化しない。

適用候補:

- センサ接続
- センサ切断
- 初期化失敗
- 測定値更新
- FIFOウォーターマーク
- 表示モード変更
- 設定変更
- ログ書き込み要求

```cpp
enum class EventType : uint8_t {
    SensorOnline,
    SensorOffline,
    SensorError,
    DataUpdated,
    Max30102Interrupt,
    Bmp585Interrupt
};

struct Event {
    EventType type;
    SensorId sensor;
    uint32_t timestampMs;
    ErrorCode error;
};
```

固定長リングバッファを使用し、満杯時の方針を明示する。

推奨:

- 重大イベントを優先
- 重複する更新イベントは集約
- 破棄件数を診断値として保持

---

## 10. 割り込み処理

## MAX30102 INT

- XIAO D2 / GPIO3
- アクティブLowを前提
- ISRではフラグ設定のみ

## BMP585 INT

- XIAO D3 / GPIO4
- 極性と出力形式はドライバ初期化時に明示
- ISRではフラグ設定のみ

```cpp
volatile bool gMax30102IrqPending = false;
volatile bool gBmp585IrqPending = false;

void IRAM_ATTR onMax30102Interrupt() {
    gMax30102IrqPending = true;
}

void IRAM_ATTR onBmp585Interrupt() {
    gBmp585IrqPending = true;
}
```

メインループ側でフラグを回収し、I²CレジスタやFIFOを読む。

---

## 11. DisplayManager

DisplayManagerは描画ライブラリの詳細をアプリケーション層から隠す。

責務:

- OLED初期化
- 画面状態管理
- 表示周期管理
- データ整形
- センサ異常表示
- OLED未接続時の無害化

```cpp
enum class ScreenId : uint8_t {
    Overview,
    Environment,
    Ppg,
    Diagnostics
};
```

```cpp
class DisplayManager {
public:
    bool begin();
    void setScreen(ScreenId screen);
    void render(const SensorSnapshot& snapshot,
                const SystemStatus& status,
                uint32_t nowMs);

    bool isAvailable() const;
};
```

初期OLED画面例:

```text
CO2   620 ppm
T     24.8 C
RH    48.2 %
P   1008.4 hPa
HR     -- bpm
SpO2   -- %
```

128×64の制約上、全情報を一画面へ押し込まず、必要なら画面を切り替える。

---

## 12. Logger

ログレベルを定義する。

```cpp
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};
```

形式例:

```text
[001245][INFO][SHT45] initialized
[001300][WARN][MAX30102] INT remained low
[005420][ERROR][SCD41] read timeout
```

条件:

- 現段階ではSerial出力
- 後からSD、LittleFS、BLE、MQTTへ出力先を追加可能
- ドライバから`Serial.print()`を直接呼ばない
- ログ生成失敗で本処理を止めない
- 高頻度RAW値を無制限に出力しない

---

## 13. ConfigurationManager

初期段階ではコンパイル時定数を使用する。

```cpp
struct AppConfig {
    uint32_t displayIntervalMs;
    uint32_t environmentIntervalMs;
    uint32_t diagnosticsIntervalMs;
    uint32_t i2cFrequencyHz;
};
```

将来はLittleFSまたはNVSへ移行可能な形にする。

設定候補:

- 各センサ周期
- OLED明るさ
- 表示画面
- ログレベル
- Wi-Fi資格情報
- BLE有効化
- データ保存有効化

---

## 14. AppController

AppControllerはシステム全体の指揮役とする。

```cpp
class AppController {
public:
    bool begin();
    void update();

private:
    SensorManager sensorManager_;
    DisplayManager displayManager_;
    Scheduler scheduler_;
    Logger logger_;
    EventBus eventBus_;
    ConfigurationManager config_;

    uint32_t lastDiagnosticsMs_ {};
};
```

処理の流れ:

```text
起動
  ↓
HAL初期化
  ↓
I²C診断
  ↓
各サービス初期化
  ↓
センサ初期化
  ↓
通常ループ
  ├─ 割り込みフラグ回収
  ├─ Scheduler実行
  ├─ センサ更新
  ├─ Snapshot更新
  ├─ OLED描画
  ├─ イベント処理
  └─ 診断ログ
```

---

## 15. main.cpp

`main.cpp`は以下の程度に抑える。

```cpp
#include <Arduino.h>
#include "app/app_controller.h"

namespace {
AppController app;
}

void setup() {
    app.begin();
}

void loop() {
    app.update();
}
```

---

## 16. Result型

単純な`bool`で原因が失われる箇所には軽量Result型を使う。

```cpp
template <typename T>
struct Result {
    T value {};
    ErrorCode error {ErrorCode::None};

    bool ok() const {
        return error == ErrorCode::None;
    }
};
```

ただし、組み込み環境で過剰なテンプレート化やヒープ利用を招かないこと。

---

## 17. センサ別責務

## SHT45

- begin
- 温湿度読み出し
- CRCまたはライブラリエラー確認
- 最終成功時刻保持

## BMP585

- begin
- 温度、気圧読み出し
- 将来FIFO対応
- 将来INT対応
- オーバーサンプリング設定をドライバ内へ閉じ込める

## SCD41

- periodic measurement開始
- data ready確認
- CO₂、温度、湿度読み出し
- 推奨周期を守る
- 不要な連続コマンド送信を避ける

## MAX30102

- begin
- LED電流、サンプルレート、FIFO設定
- RED/IR RAW取得
- 将来INT駆動
- 心拍、SpO₂計算は別コンポーネントへ分離してもよい

心拍・SpO₂アルゴリズムはセンサドライバそのものと分離することを推奨する。

```text
Max30102Sensor
  └─ RAW取得

PpgProcessor
  ├─ フィルタ
  ├─ ピーク検出
  ├─ 心拍推定
  └─ SpO₂推定
```

---

## 18. I²Cバス管理

I²Cバスは一箇所で初期化する。

```cpp
constexpr uint8_t I2C_SDA_PIN = D4;
constexpr uint8_t I2C_SCL_PIN = D5;
constexpr uint32_t I2C_FREQUENCY_HZ = 100000;
```

条件:

- 初期確認は100 kHz
- 安定確認後に400 kHzを検討
- ドライバ個別に`Wire.begin()`を呼ばない
- バスエラー回数を記録する
- 必要に応じてバス再初期化手順を追加する
- 予約アドレスはスキャンしない
- 通常スキャン範囲は0x08〜0x77

期待アドレス:

| アドレス | デバイス |
|---|---|
| 0x3C | SSD1306 |
| 0x44 | SHT45 |
| 0x46 / 0x47 | BMP585 |
| 0x57 | MAX30102 |
| 0x62 | SCD41 |

---

## 19. 状態遷移

各デバイスは概ね以下の状態遷移を持つ。

```text
Unknown
  ↓
Initializing
  ├─ success → Ready
  └─ failure → Error
                  ↓
             retry timeout
                  ↓
             Initializing

Ready
  ├─ temporary failure → Degraded
  ├─ repeated failure → Offline
  └─ recovery → Ready
```

単発の読み出し失敗で即座にOfflineへ落とさない。

推奨:

- 1回失敗: Warning
- 連続3回失敗: Degraded
- 一定時間成功なし: Offline
- 再初期化成功: Ready

---

## 20. テスト方針

## 単体テスト

ホスト環境で可能なもの:

- Scheduler
- 状態遷移
- データ整形
- OLED表示文字列生成
- EventBus
- エラー集約
- PPG演算

## 実機テスト

- I²C全デバイス検出
- センサ1台ずつ切断
- OLED切断
- INT Low/High確認
- 長時間運転
- USB再接続
- センサ読み出し失敗からの復帰
- I²C 100 kHz / 400 kHz比較

---

## 21. 診断情報

最低限保持する。

```cpp
struct Diagnostics {
    uint32_t uptimeMs;
    uint32_t loopCount;
    uint32_t i2cErrorCount;
    uint32_t eventDropCount;
    uint32_t displayUpdateCount;
    uint32_t sensorReadFailures;
    uint32_t maxLoopDurationUs;
};
```

OLED診断画面またはSerialへ定期表示できること。

---

## 22. メモリ管理

- `String`の頻繁な連結を避ける
- `snprintf()`と固定長バッファを優先
- 長寿命オブジェクトを静的確保
- コールバックでキャプチャ付き`std::function`を乱用しない
- 大きなRAWバッファはサイズを明示する
- MAX30102のFIFO処理では配列境界を厳守する
- スタック使用量を必要に応じて計測する

---

## 23. FreeRTOS移行条件

初期版では単一ループを推奨する。

以下の条件が出た場合にタスク分離を検討する。

- MAX30102処理が他センサ周期を妨げる
- Wi-Fi通信が長時間ブロックする
- LVGL描画周期を保証する必要がある
- SDカード書き込みとサンプリングを分離したい
- 通信と計測で異なる優先度が必要

移行例:

| Task | 役割 |
|---|---|
| SensorTask | センサ取得 |
| PpgTask | PPG処理 |
| UiTask | LVGL/OLED |
| StorageTask | SD保存 |
| CommsTask | BLE/Wi-Fi/MQTT |

タスク間通信にはQueueまたはMessageBufferを使用し、共有可変状態を増やさない。

---

## 24. 通信抽象化

将来の通信追加では以下のインターフェースを想定する。

```cpp
class IDataPublisher {
public:
    virtual ~IDataPublisher() = default;
    virtual bool begin() = 0;
    virtual void update(uint32_t nowMs) = 0;
    virtual bool publish(const SensorSnapshot& snapshot) = 0;
};
```

実装候補:

- BlePublisher
- MqttPublisher
- SerialPublisher
- ProprietaryRadioPublisher

アプリケーション層は通信方式を意識しない。

---

## 25. 保存抽象化

```cpp
class IDataStore {
public:
    virtual ~IDataStore() = default;
    virtual bool begin() = 0;
    virtual bool append(const SensorSnapshot& snapshot) = 0;
    virtual void flush() = 0;
};
```

実装候補:

- SdCardStore
- LittleFsStore
- NullStore

---

## 26. Antigravityへの実装指示

1. 現在のI²C診断コードを失わないこと
2. 一度に全アーキテクチャを実装しないこと
3. まず共通型、HAL、Logger、SensorManagerの骨格を作ること
4. 次にSSD1306をDisplayManagerへ移すこと
5. SHT45から順番にドライバを統合すること
6. 各フェーズで必ずビルドすること
7. 実機未確認のAPIを推測だけで確定しないこと
8. 採用ライブラリ名とバージョンをREADMEへ記載すること
9. ライブラリAPIが異なる場合は、仕様に合わせて薄いアダプタを作ること
10. ISRではフラグ設定以外を行わないこと
11. `main.cpp`を肥大化させないこと
12. エラー発生時も可能な範囲で継続動作させること

---

## 27. 段階的実装計画

### Step 1

- pins.h
- i2c_bus
- logger
- I²Cスキャナ
- README更新

### Step 2

- 共通状態型
- SensorManager骨格
- DisplayManager骨格
- SSD1306統合

### Step 3

- SHT45ドライバ
- EnvironmentData
- OLED表示

### Step 4

- BMP585ドライバ
- 気圧表示
- INTフラグ骨格

### Step 5

- SCD41ドライバ
- CO₂表示
- 測定周期管理

### Step 6

- MAX30102 RAW取得
- FIFO処理
- INTフラグ回収

### Step 7

- PpgProcessor
- 心拍
- SpO₂
- 品質フラグ

### Step 8

- EventBus
- Diagnostics
- 再初期化戦略

---

## 28. Definition of Done

各Stepで以下を満たすこと。

- PlatformIOビルド成功
- 新規Warningを残さない
- XIAO ESP32S3へUpload成功
- シリアルログで状態確認可能
- 接続済みデバイスを正しく認識
- 未接続デバイスがあっても全停止しない
- README更新
- 変更点と既知の問題を記録
- `main.cpp`へセンサ固有処理を直接追加していない
- 次Stepへ進める状態である

---

## 29. 最終目標

最終的にアプリケーション層は次の関心だけを持つ。

- 現在の測定値
- 各デバイスの状態
- 表示すべき情報
- 保存すべき情報
- 送信すべき情報

I²Cレジスタ、ライブラリ固有クラス、FIFO処理、OLED描画APIは下位層へ封じ込める。

この境界を守ることで、SSD1306をLVGL画面へ置き換えたり、Serial出力をBLEやMQTTへ置き換えたりしても、センサ取得ロジックの大部分を変更せずに済む。
