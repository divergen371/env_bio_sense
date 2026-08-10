# MB85RC256V FRAM + SDカード 二層ストレージ導入 作業指示書

## 目的

既存のSDカードロギング機能にMB85RC256V FRAMモジュールを追加する。

FRAMは「小容量のSDカード」としてではなく、以下を担う不揮発ジャーナル／短期バッファとして使用する。

- センサ測定値の一時保持
- SDカード書き込み失敗時のデータ保護
- 電源断・再起動後の未処理データ復旧
- センサ異常・再初期化・SD障害などのイベント記録
- 高頻度更新される小さな永続状態の保存

SDカードは引き続き、長期保存用の主ストレージとする。

## 1. 目標アーキテクチャ

```text
Sensors
   |
   v
SensorManager
   |
   +--> LatestValues --> OLED
   |
   v
FRAM Journal / Ring Buffer
   |
   +--> Persistent State
   |
   +--> Event Journal
   |
   v
SD Flush Worker
   |
   v
SD Card
   |
   +--> CSV / Binary Log
```

基本原則:

1. 測定値はまずRAM上で正当性を確認する
2. 保存対象データはFRAMへcommitする
3. SDカードへflushする
4. SD保存成功後にFRAM側を「処理済み」とする
5. SD保存に失敗してもFRAM上の未処理データを残す
6. 再起動後はFRAMから未処理データを復旧する

## 2. 既存コードの確認

実装前に以下を調査する。

- SDカード初期化処理
- SDカード書き込み処理
- SensorManager
- I2C HAL
- Mutex実装
- センサ値のデータ構造
- RTC / NTP / GNSS由来の時刻管理
- エラーログ
- 起動・再起動処理
- PlatformIO設定
- 使用中のI2Cアドレス一覧

成果物として、現在の保存経路を簡潔に整理すること。

## 3. MB85RC256Vドライバ追加

新規クラス例:

```cpp
class FramStorage;
```

または:

```cpp
class Mb85rc256v;
```

最低限のAPI:

```cpp
bool begin();

bool read(
    uint16_t address,
    uint8_t* buffer,
    size_t length);

bool write(
    uint16_t address,
    const uint8_t* data,
    size_t length);

bool readByte(uint16_t address, uint8_t& value);
bool writeByte(uint16_t address, uint8_t value);

bool isPresent() const;
```

必要に応じて以下も追加する。

```cpp
bool fill(uint16_t address, uint8_t value, size_t length);
bool verify(uint16_t address, const uint8_t* data, size_t length);
```

## 4. I2C実装ルール

MB85RC256Vは既存I2Cバスへ追加する。

I2Cアクセスは必ず既存の `hal::I2cBus` を経由すること。

```text
FramStorage
    |
    v
hal::I2cBus
    |
    v
Mutex
    |
    v
Wire
```

FRAMドライバから`Wire`を直接触らない。

多層Mutexを作らない。MutexはHALのI2Cトランザクション単位へ集約する。

## 5. I2Cアドレス確認

起動時にI2Cスキャンを行い、FRAMの実アドレスを確認する。

アドレスはモジュール側のA0/A1/A2設定に依存するため、固定値を思い込みで決めない。

他センサとアドレス競合していないことを確認する。

## 6. FRAMメモリマップ

FRAM全体を用途別に固定領域へ分割する。

```text
0x0000
+------------------------------+
| Superblock / Metadata        |
+------------------------------+
| Persistent State             |
+------------------------------+
| Event Journal                |
+------------------------------+
| Sensor Ring Buffer           |
+------------------------------+
0x7FFF
```

正確なサイズ配分は、現在の保存レコードサイズを確認して決定する。

## 7. Superblock設計

FRAM先頭に管理情報を置く。

```cpp
struct FramSuperblock {
    uint32_t magic;
    uint16_t formatVersion;

    uint16_t writeIndex;
    uint16_t readIndex;

    uint32_t nextSequence;
    uint32_t bootCount;

    uint32_t lastSdFlushSequence;

    uint32_t crc32;
};
```

最低限保持するもの:

- magic
- format version
- ring buffer write position
- ring buffer read position
- sequence number
- boot count
- 最後にSDへ保存できたsequence
- 整合性検査値

FRAM内容を無条件に信用しない。

## 8. フォーマット識別

初回起動時、以下を確認する。

```text
magic
formatVersion
CRC
```

不正な場合のみ初期化する。

毎回起動時にFRAM全消去してはならない。

## 9. センサログレコード

FRAMに保存するセンサレコードは固定長を推奨する。

```cpp
struct EnvironmentalRecord {
    uint32_t sequence;
    uint64_t timestamp;

    float temperatureC;
    float humidityRh;
    float pressureHpa;

    uint16_t co2Ppm;

    float vocIndex;
    float noxIndex;

    float pm1;
    float pm25;
    float pm10;

    uint32_t validFlags;
    uint16_t crc16;
};
```

実際のセンサ構成に合わせて調整する。

## 10. validFlagsを設ける

センサ値そのものだけで正常・異常を推定しない。

```cpp
enum SensorValidFlags : uint32_t {
    VALID_TEMP     = 1u << 0,
    VALID_HUMIDITY = 1u << 1,
    VALID_PRESSURE = 1u << 2,
    VALID_CO2      = 1u << 3,
    VALID_VOC      = 1u << 4,
    VALID_NOX      = 1u << 5,
    VALID_PM       = 1u << 6,
};
```

SCD41の0 ppm、BMP5の範囲外値などを正常値として保存しない。

必要なら無効であること自体を記録する。

## 11. Ring Buffer設計

Sensor Ring Bufferは循環バッファとする。

管理値:

```text
writeIndex
readIndex
sequence
```

- `writeIndex`: 次に書く位置
- `readIndex`: 次にSDへflushする位置
- `sequence`: レコードの単調増加ID

満杯時の挙動を明確にする。

推奨:

- 未flushデータを勝手に上書きしない
- バッファFULLをイベントとして記録する
- SD flushを優先する
- 古いデータ破棄が必要ならポリシーを明示する

## 12. Commit方式

FRAMへの書き込み途中で電源断しても、壊れたレコードを正常データとして扱わない。

推奨方式:

1. レコード本体を書き込む
2. CRCを書く
3. 最後にcommit markerを書く

```cpp
struct FramRecordHeader {
    uint32_t sequence;
    uint16_t length;
    uint16_t crc;
    uint8_t committed;
};
```

`committed == 1` のレコードのみ有効とする。

## 13. SD Flush

FRAMからSDへのflush処理を独立させる。

```cpp
class StorageManager {
public:
    bool appendToFram(const EnvironmentalRecord&);
    void flushPendingToSd();
};
```

基本処理:

```text
FRAM readIndex
    |
    v
record read
    |
    v
CRC check
    |
    v
SD append
    |
    +-- success --> readIndex advance
    |
    +-- failure --> FRAM保持
```

## 14. SDカード障害時

以下を想定する。

- SDカード未挿入
- mount失敗
- file open失敗
- write失敗
- flush失敗
- filesystem error

SD障害時:

- FRAM記録は継続する
- 未処理データを消さない
- SDへ短周期で永久再試行しない
- バックオフする
- OLEDまたはログでSD異常を明示する

例:

```text
SD: OFFLINE
FRAM: BUFFERING
```

## 15. SD再接続後の復旧

SDが復旧したら、FRAMに残っている未処理データを古い順にflushする。

sequence番号を利用し、SD側で重複記録が発生しないようにする。

## 16. 再起動時の復旧

起動時:

1. FRAM初期化
2. Superblock検証
3. bootCount更新
4. 未commitレコードを無効扱い
5. 未flushレコード数を確認
6. SD初期化
7. SDが正常ならrecovery flush
8. 通常測定開始

## 17. Event Journal

センサ時系列値とは別に、小さなイベントジャーナルを持たせる。

対象例:

- boot
- watchdog reset
- brownout
- SD mount failure
- SD write failure
- SD recovery
- BMP581 offline
- BMP581 recovery
- SCD41 error
- I2C timeout
- FRAM error
- ring buffer full

```cpp
struct EventRecord {
    uint32_t sequence;
    uint64_t timestamp;
    uint16_t eventCode;
    int32_t detail;
    uint16_t crc;
};
```

## 18. Persistent State

高頻度更新される小さな永続状態をFRAMへ保存する。

候補:

- boot count
- last sequence
- last successful SD flush
- current log file sequence
- センサoffline状態
- エラーカウンタ
- 未処理レコード位置

## 19. MAX30102等の高頻度RAWデータ

高頻度RAWデータをFRAMへ長時間保存しない。

基本構成:

```text
MAX30102 RAW
    |
    v
RAM buffer
    |
    v
SD direct stream
```

FRAMへ保存するのは、セッション開始情報、終了情報、ファイル名、未完了フラグ、最終flush位置、障害イベントなどに限定する。

## 20. SD書き込み頻度

センサ値1件ごとにSDへ同期書き込みしない。

推奨:

- FRAMにはレコード単位でcommit
- SDへは複数レコードをまとめて書く
- 一定時間または一定件数でflush

実際の周期は測定頻度と安全性要件から決定する。

## 21. 電源断試験

以下のタイミングで意図的に電源断する。

1. FRAM書き込み前
2. FRAMレコード書き込み途中
3. commit marker書き込み前
4. commit完了後
5. SD書き込み中
6. SD成功後、readIndex更新前
7. readIndex更新後

再起動後に以下を確認する。

- 壊れたレコードを採用しない
- commit済みデータを失わない
- SDへの重複書き込みを防げる
- ring buffer位置が復旧する

## 22. FRAM単体テスト

新規テスト環境例:

```text
scratch_test_fram.cpp
```

```ini
[env:fram_test]
```

テスト項目:

- device detection
- 1 byte write/read
- block write/read
- 先頭アドレス
- 最終アドレス
- 境界跨ぎ
- pattern test
- CRC test
- 電源再投入後保持
- 1000回以上の連続read/write
- I2C error handling

パターン例:

```text
0x00
0xFF
0xAA
0x55
incrementing pattern
random pattern
```

## 23. FRAM + SD結合テスト

順番に確認する。

1. FRAMのみ
2. FRAM + OLED
3. FRAM + BMP581
4. FRAM + SCD41
5. FRAM + 全I2Cセンサ
6. FRAM + SD
7. 全システム

各段階で以下を記録する。

- I2C timeout
- lock timeout
- sensor read error
- FRAM read/write error
- SD error
- lost records
- duplicate records

## 24. I2C回帰試験

FRAM追加後に以下を必ず確認する。

- BMP581
- SCD41
- SHT45
- SGP41
- OLED
- RTC
- その他I2Cデバイス

特に以下を重点確認する。

- Mutex多重化
- unlock漏れ
- 長時間lock
- FRAM連続書き込みによるバス占有

## 25. 性能計測

以下をログで測定する。

```text
FRAM write latency
FRAM read latency
SD batch write latency
I2C lock wait time
pending record count
maximum pending record count
```

FRAM書き込みがセンサ周期を妨害していないか確認する。

## 26. エラー状態

最低限以下を区別する。

```text
FRAM_OK
FRAM_OFFLINE

SD_OK
SD_OFFLINE

BUFFER_OK
BUFFER_FULL

RECOVERY_IDLE
RECOVERY_RUNNING
RECOVERY_FAILED
```

一つのboolですべてを表現しない。

## 27. OLED表示

必要に応じてストレージ状態を表示可能にする。

通常時:

```text
SD   OK
FRAM OK
BUF  12
```

異常時:

```text
SD   ERROR
FRAM BUFFERING
BUF  143
```

## 28. ログ出力

例:

```text
[FRAM] init OK addr=0x50
[FRAM] seq=1024 committed slot=14
[SD] flush seq=1016..1024 OK
[SD] write failed
[FRAM] buffering pending=9
[SD] recovered
[FRAM] replay pending=9
```

## 29. ファイル構成案

```text
src/
  storage/
    fram_storage.cpp
    fram_storage.hpp
    storage_manager.cpp
    storage_manager.hpp
    storage_records.hpp

  hal/
    i2c_bus.cpp
    i2c_bus.hpp

tests/
  scratch_test_fram.cpp
```

既存構成に合わせて調整する。

## 30. 実装順序

1. 現在のSD保存経路を確認
2. I2Cアドレス確認
3. FRAM単体ドライバ実装
4. FRAM単体テスト
5. Superblock実装
6. Ring Buffer実装
7. CRC / commit処理実装
8. StorageManager実装
9. SD flush統合
10. 起動時recovery実装
11. Event Journal実装
12. I2C回帰試験
13. 電源断試験
14. 30分以上の結合試験
15. ドキュメント更新

## 31. 完了条件

以下をすべて満たすこと。

- MB85RC256Vが安定してread/writeできる
- I2Cアドレス競合がない
- FRAM追加後も既存I2Cセンサが正常動作する
- センサレコードをFRAMへcommitできる
- SD保存成功後のみFRAM側を消費済みにできる
- SD障害中もFRAMへ保存を継続できる
- SD復旧後に未処理データをflushできる
- 再起動後に未処理データを復旧できる
- 書き込み途中のレコードを正常データとして採用しない
- 永久再試行ループへ入らない
- 高頻度RAWデータを無計画にFRAMへ保存しない
- 電源断試験でデータ構造が破綻しない
- 30分以上の全体試験で既存センサに回帰不具合がない

## 32. 最終報告

以下を報告すること。

### 変更ファイル
- 新規
- 修正
- 削除

### FRAM構成
- I2Cアドレス
- 使用容量
- メモリマップ
- レコードサイズ
- 最大保持件数

### 試験結果
- FRAM単体
- SD結合
- 電源断
- 再起動復旧
- I2C回帰
- 全体連続動作

### エラー試験
- SD抜去
- SD書き込み失敗
- FRAM異常
- buffer full
- 再起動

### 未解決事項
- 未確認のハードウェア条件
- 容量上の制約
- 将来改善候補

## 最重要方針

FRAMはSDカードの代替ではない。

FRAMは、**SDへ安全に渡すまでの不揮発な待機場所**として設計すること。

一つのストレージ障害によって、センサ取得・表示・他センサ通信まで巻き込まない構成にする。
