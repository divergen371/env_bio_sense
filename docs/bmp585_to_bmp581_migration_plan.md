# BMP585 → BMP581移行計画・実装指示書

## 目的

現在使用しているBMP585モジュールを、STEMMA QT BMP581搭載圧力センサモジュールへ置き換える。

今回の移行は単なる部品交換ではなく、BMP5系センサドライバの品質向上、診断機能強化、I2Cアーキテクチャ整理を同時に行う。

将来的なセンサ交換やBosch BMP5シリーズへの展開を容易にすることも目的とする。

## 成果物

以下を完成させること。

- BMP581対応ドライバ
- BMP581単体テスト
- BMP581診断ツール
- 移行後の結合テスト
- 更新された設計資料
- 更新されたPlatformIO環境
- 回帰試験レポート

## フェーズ1 現状整理

まず現在のBMP585実装を調査する。

整理対象:

- ディレクトリ構成
- ドライバ層
- HAL層
- Bosch APIラッパ
- SensorManager
- DisplayManager
- 設定ファイル
- PlatformIO

出力例:

```text
Current Architecture

HAL
 ├── I2cBus
 └── ...

Drivers
 ├── Bmp585Sensor
 ├── SCD41
 ├── ...
```

## フェーズ2 BMP581仕様調査

以下を整理する。

- CHIP_ID
- I2C Address
- 電源条件
- Power Mode
- ODR
- OSR
- IIR
- FIFO有無
- Status Register
- INT_STATUS
- Soft Reset
- NVM
- Measurement Flow

BMP585との差分一覧を作成する。

| 項目           | BMP585 | BMP581 |
| -------------- | ------ | ------ |
| CHIP_ID        |        |        |
| Pressure Range |        |        |
| Register差分   |        |        |
| Power Mode     |        |        |

## フェーズ3 ドライバ設計見直し

現在の `Bmp585Sensor` を見直す。

以下の構造へ整理する。

```text
PressureSensor
        ▲
        │
Bmp5SensorBase
   ▲
   ├── Bmp581Sensor
   └── Bmp585Sensor
```

共通化対象:

- 初期化
- Reset
- Read
- Error handling
- Diagnostic
- Register Dump

製品固有:

- CHIP_ID
- Register定義
- 初期設定
- Variant情報

## フェーズ4 レジスタ定義整理

独自defineを削除する。

禁止例:

```cpp
#define BMP_STATUS 0x28
```

推奨:

Bosch公式ヘッダの定数を使用する。

どうしても独自定義する場合は `bmp5_registers.hpp` へ集約する。

## フェーズ5 診断機能改善

診断を一つのクラスへまとめる。

```cpp
Bmp5Diagnostic
```

機能:

- Register Dump
- Status Decode
- INT Decode
- Raw Pressure
- Raw Temperature
- CHIP_ID
- Revision
- Power Mode

起動時ログ例:

```text
===== BMP5 DIAGNOSTIC =====
Variant
CHIP_ID
Revision
Registers
Status
===========================
```

## フェーズ6 Register Dump改善

現在の問題:

- STATUSとINT_STATUSの取り違え疑惑がある

改善:

- レジスタ番号も表示する
- Reserved領域も表示する
- バースト読み出しと個別読み出しの一致を確認する

例:

```text
0x26 RESERVED   = 0x00
0x27 INT_STATUS = 0x01
0x28 STATUS     = 0x02
```

## フェーズ7 Bosch API更新

使用しているBosch SensorAPIを確認する。

- 現在のバージョンを記録する
- 最新版との差分を確認する
- 更新可能であれば更新する
- 修正範囲はラッパ層に限定する

## フェーズ8 I2C整理

MutexはHALのみが保持する。

避ける構造:

```text
SensorManager lock
    ↓
Driver lock
    ↓
HAL lock
```

目標構造:

```text
Sensor
    ↓
HAL
    ↓
Wire
```

MutexはI2Cトランザクションの直前に取得し、終了直後に解放する。

以下の処理中は保持しない。

- delay
- vTaskDelay
- 文字列生成
- シリアルログ
- OLED描画内容の構築
- センサ測定待機
- SDカード書き込み

## フェーズ9 エラー処理改善

BMP5センサが恒久異常の場合、短周期で再初期化を繰り返さない。

状態遷移を導入する。

```text
Initializing
    ↓
Running
    ↓
Warning
    ↓
Offline
    ↓
RetryWait
    ↓
Running
```

再試行バックオフ:

| 回数      | 待機時間 |
| --------- | -------: |
| 1回目     |     即時 |
| 2回目     |     10秒 |
| 3回目     |     30秒 |
| 4回目以降 |      5分 |

一定回数失敗したら、BMP581を一時的にOFFLINE扱いとし、SCD41など他のI2Cデバイスを優先する。

## フェーズ10 BMP581単体テスト

新規作成:

```text
scratch_test_bmp581.cpp
```

PlatformIO環境例:

```ini
[env:bmp581_test]
```

確認項目:

- CHIP_ID
- STATUS
- INT_STATUS
- Pressure
- Temperature
- Soft Reset
- Register Dump
- 30分連続測定

集計項目:

```text
samples
success
read_errors
range_errors
reset_count
```

## フェーズ11 結合試験

以下の順序で段階的に構成を戻す。

1. BMP581のみ
2. BMP581 + OLED
3. BMP581 + SCD41
4. BMP581 + SHT45
5. BMP581 + SGP41
6. BMP581 + SPS30
7. 全センサ

各段階で以下を確認する。

- I2Cエラー
- Mutexタイムアウト
- 値の停止
- 0固定
- 異常な再初期化
- OLED表示停止
- タスクのブロッキング

## フェーズ12 回帰試験

確認対象:

- BMP581
- OLED
- SCD41
- SHT45
- SGP41
- SPS30
- RTC
- SDカード
- Display
- I2C Mutex

各デバイスについて以下を記録する。

- 初期化成否
- 測定周期
- 読み出し成功数
- 読み出し失敗数
- 無効値数
- 再初期化回数
- 30分以上の連続動作結果

## フェーズ13 設計改善

抽象化の目標例:

```text
EnvironmentalSensor
├── Pressure
├── Temperature
├── Humidity
├── CO2
├── VOC
└── PM
```

BMP581については、必要以上に汎用化せず、次のどちらかを採用する。

### 案A: 製品固有クラス

```cpp
Bmp581Sensor
```

可読性を優先する。

### 案B: BMP5系列クラス

```cpp
Bmp5Sensor
```

内部でvariantを保持する。

```cpp
enum class Variant {
    Bmp581,
    Bmp585,
};
```

現時点では、既存コード量と将来のBMP585再検証予定を考慮して選択する。

## フェーズ14 ドキュメント更新

更新対象:

- README
- Architecture
- I2C設計
- Sensor構成
- Diagnostic
- Register一覧
- PlatformIO
- Troubleshooting
- 移行履歴

## フェーズ15 最終報告

以下を提出すること。

### 1. 変更ファイル一覧

### 2. 新規追加ファイル一覧

### 3. 削除ファイル一覧

### 4. BMP585との差分

### 5. BMP581初期化ログ

### 6. Register Dump

### 7. 30分測定結果

### 8. I2C競合確認

### 9. SCD41との同時動作確認

### 10. 既知の制約

## 完了条件

以下をすべて満たすこと。

- BMP581が正常な気圧を安定取得できる
- Bosch APIで正常動作する
- Register Dumpが正しく解釈される
- STATUSとINT_STATUSの取り違えがない
- I2C MutexがHAL層へ整理される
- SCD41を含む全センサが同時動作する
- 30分以上エラーなく測定できる
- BMP585固有コードが不要な箇所で残っていない
- 診断機能がBMP581とBMP585の双方で利用可能な構成になっている
- 異常時に短周期の再初期化ループへ陥らない
- BMP581無効時にも他センサが継続動作する

## 実装上の注意

BMP585対応コードは直ちに完全削除しないこと。

BMP585は今回の故障解析および別個体比較の対象として価値がある。共通部分を `Bmp5SensorBase` または共通HALへ整理しつつ、BMP585固有実装は隔離して残す。

STEMMA QTモジュール導入時は、以下も確認する。

- 使用電圧
- STEMMA QTケーブルの向き
- I2Cアドレス
- モジュール上のプルアップ抵抗
- 他モジュールとのプルアップ合成値
- ケーブル長
- ケース内で圧力導入口を塞いでいないこと
- 汗や結露が直接センサ開口部へ到達しないこと
