# MB85RC256V FRAM＋microSD 二層ストレージ統合作業指示書

| 項目 | 内容 |
|---|---|
| 文書種別 | 実装・試験作業指示書 |
| 文書版 | 1.0 |
| 対象 | XIAO ESP32S3、MB85RC256V、SPI接続microSD、環境センサ群、MAX30102 |
| PPGファイル形式 | PPG Binary Format v1 |
| 基準時刻 | UTC |

## 1. 目的

既存のMB85RC256V FRAM＋microSD二層ストレージ方針を維持したまま、環境履歴、PPGセッション、システムイベントを一貫した形式で保存し、SDカード障害、リセット、電源断後にも可能な範囲で自動復旧できる保存系を実装する。

本書では次を固定する。

- FRAMとSDカードの責務分担
- SDカード上のディレクトリ／ファイル構成
- PPG Binary Format v1
- PPG RAWのRAM/PSRAMからSDへの直接ストリーム
- FRAMをWAL（Write-Ahead Log）／短期バッファとして使用する処理順
- PPGセッションの一時ファイル作成、検証、renameによる確定手順
- 環境値の開始時・測定中・終了時の付帯保存
- FRAM上のsystemイベントのSDアーカイブ
- 起動時リカバリと重複防止
- SD障害試験、電源断試験、完了条件

## 2. 最重要方針

### 2.1 FRAMの役割

FRAMは「小容量のSDカード」として扱わない。以下を担う不揮発ジャーナル／短期バッファとして使用する。

- センサ測定値の一時保持
- SDカードへの書き込みに失敗した小規模データの保護
- 電源断・再起動後の未処理データ復旧
- センサ異常、再初期化、SD障害、復旧処理などのイベント記録
- 高頻度更新される小さな永続状態の保存
- PPGセッションの状態、最終確定ブロック、件数、CRCなどの復旧用チェックポイント

FRAMへ保存してからSDへ反映し、SDへの反映を確認した後にFRAMレコードを消費済みにする。

```text
小規模な永続化対象を生成
        ↓
FRAMへREADY状態でcommit
        ↓
SDへ書き込み、flush、close、必要な検証
        ↓
SD反映済みと判定
        ↓
FRAMレコードをCONSUMEDにする
```

### 2.2 SDカードの役割

SDカードは長期保存用の主ストレージとする。

- 月単位の環境履歴
- PPGのRed/IR RAWバイナリ
- PPG測定条件、結果、品質情報
- PPGセッションに同期した環境値
- FRAMイベントジャーナルの長期アーカイブ
- ストレージ処理、障害、復旧結果の監査記録

### 2.3 PPG RAWの例外

PPG RAWはFRAMへ逐次保存しない。MAX30102のFIFOから取得したサンプルをRAMまたはPSRAM上のリングバッファへ入れ、ブロック化してSDへ直接ストリームする。

```text
MAX30102 FIFO
      ↓
RAM/PSRAMリングバッファ
      ↓
PPGブロック生成＋CRC
      ↓
SDのraw.ppg.tmpへ連続書き込み
```

MB85RC256Vの容量は32 KiBであり、PPG RAW本体を保持すると、イベント、環境値、復旧状態という本来の責務を圧迫する。FRAMへ保存するのはPPG RAW本体ではなく、セッションID、ファイルパス、状態、最終確定ブロック番号、サンプル数、ストリームCRC、ドロップ数などの小さな復旧情報とする。

SDが一定時間復旧しない場合、RAM/PSRAMの有限バッファを越えてRAWを保持できない。隠れてデータを捨てず、測定を中止または部分セッションとして確定し、欠落サンプル数と理由を記録する。

## 3. 既存方針との整合条件

### 3.1 既存データと実装の保全

既存実装にFRAMのメモリマップ、レコードヘッダ、CRC、リング管理、superblockの二重化、I²Cアドレス設定が存在する場合、それらを破壊的に変更しない。本書の論理レコード種別を既存形式へ追加する。

- 既存FRAMデータを無条件に初期化しない。
- フォーマット版を確認してからマイグレーションする。
- 不明な版を検出した場合は読み取り専用の障害状態へ移行し、イベントを出す。
- MB85RC256VのI²Cアドレスは既存配線・既存設定を正とする。
- MAX30102の`0x57`と衝突するFRAMアドレスを選ばない。
- FRAMアクセスは既存I²Cバスマネージャ／HAL経由とし、上位サービスが直接`Wire`を操作しない。

### 3.2 ハードウェア前提の維持

既存文書で決定済みの配線・電源方針を維持する。現構成の基準は次とする。

```text
XIAO ESP32S3        microSD（SPI、3.3 Vロジック）
──────────────────────────────────────────────
3V3              → 3V3
GND              → GND
D8 / GPIO7       → SCK
D9 / GPIO8       → MISO
D10 / GPIO9      → MOSI
D1               → CS
```

- 3.3 V MCUへ不要な5 V用抵抗分圧レベル変換を持つSDモジュールは使用しない。
- SDモジュール近傍へ0.1 µFセラミックと47～100 µF程度のバルクコンデンサを置く。
- 現在のSD SPIクロックは`4 MHz`とし、実装定数と試験記録の基準値にも`4 MHz`を使用する。
- 初期検証は16 GBまたは32 GBのFAT32、High Endurance系microSDを基準とする。
- SD SPI配線、I²C配線、割り込み線を短く保ち、PPG連続取得中のSD書き込みで3.3 Vが不安定にならないことを測定する。
- MB85RC256Vは32 KiBのI²C FRAMとして扱う。アドレス選択端子を含む実装値は既存配線を正とし、I²C scanでMAX30102の`0x57`との非衝突を確認する。

ハードウェア構成を変更した場合は、使用モジュール、SPIクロック、電源コンデンサ、I²Cアドレス、カード形式を試験記録へ残す。

### 3.3 SD SPIクロックの運用

現在はブレッドボード上でジャンパーワイヤを使用しているため、配線のインダクタンス、寄生容量、GND品質、クロストークによって高速化時の信号品質が悪化しやすい。したがって、`4 MHz`で必要な書き込み性能を満たす間は、周波数を理由なく上げない。

次のいずれかが実測された場合に限り、SPIクロックの引き上げを検討する。

- PPG取得中にSD書き込みが追いつかず、リングバッファが危険水位へ達する。
- SD待ちに起因するサンプルdropまたはMAX30102 FIFO overflowが発生する。
- 最大SD write時間が設計したバッファ保持時間へ接近する。
- 将来のサンプルレート、チャンネル数、保存データ量の増加により、`4 MHz`では必要帯域を満たせない。

引き上げ前に、ロジックアナライザで`CS`、`SCK`、`MOSI`、`MISO`を同時取得し、SPIデコード結果、クロック周期、CSのタイミング、ビット化け、予期しないパルスを確認する。ロジックアナライザのサンプルレートはSCKの少なくとも4倍、可能なら10倍以上を使用する。

ロジックアナライザは論理タイミングとプロトコル確認には有効だが、立ち上がり時間、リンギング、オーバーシュート、電圧マージンそのものは十分に評価できない。波形品質に疑いがある場合はオシロスコープも使用し、SDモジュール側で`SCK`、`MOSI`、`MISO`、3.3 V電源を確認する。測定用プローブのGNDリードは短くし、測定系自身による波形悪化を避ける。

周波数は一度に大きく上げず、例えば次の順で段階的に評価する。

```text
4 MHz（現行・基準）
  ↓
6 MHz
  ↓
8 MHz
  ↓
10 MHz
```

各周波数で次を実施する。

1. SDのmount、ファイル作成、write、flush、close、renameを反復する。
2. 既知データを書き戻してCRCを照合する。
3. 想定最大時間のPPG連続取得を行う。
4. SD初期化失敗、timeout、短いwrite、再試行、CRC不一致の件数を確認する。
5. 最大SD write時間、リング高水位、drop数、FIFO overflow数を`4 MHz`時と比較する。
6. 電源断・再起動・tmp復旧試験を行う。

1件でも再現性のあるエラーが増えた場合は、直前の安定周波数へ戻す。高速化しても最大書き込み停止時間やリング高水位が改善しない場合、原因はSPIクロックではなく、カード内部処理、電源、配線、ファイルシステム、flush頻度の可能性があるため、周波数をさらに上げない。

恒久的に高い周波数が必要になった場合は、ブレッドボードのまま周波数だけを上げるのではなく、短い配線、連続したGNDリターン、適切なデカップリングを持つ基板または専用配線へ移行する。採用周波数はカード銘柄、配線状態、ロジアナ／オシロ結果、長時間試験結果とともに記録する。

### 3.4 micros()によるSD処理時間の計測

SPIクロック変更の要否は推測ではなく、マイコンの`micros()`または`millis()`で実処理時間を測定して判断する。通常のSD書き込みは`micros()`を使用し、長時間のセッション全体は`millis()`も併用する。

`src/storage/storage_manager.cpp`の`flushPendingToSd()`では、少なくとも次を分けて測定する。

- FRAMレコードを読み、SDへ書くループの時間
- `file.flush()`に要した時間
- `file.close()`に要した時間
- ループ開始からclose完了までの総時間
- 処理レコード数と書き込みbytes

ループ終了直後だけで計測を止めると、SDカード内部の書き込み待ちが現れやすい`flush()`や`close()`の遅延を含まない。そのため、ループ時間と総時間を別々に記録する。

実装例:

```cpp
// fileはopen済み、bytesWrittenは実際にwriteできたbyte数を加算する。
const uint32_t totalStartUs = micros();
const uint32_t writeStartUs = totalStartUs;

while (currentIndex != superblock_.writeIndex) {
    // FRAMからレコードを読み出す。
    // SDへ書き、戻り値が要求byte数と一致することを確認する。
    // 成功分をflushedCountとbytesWrittenへ加算する。
}

const uint32_t writeEndUs = micros();

file.flush();
const uint32_t flushEndUs = micros();

file.close();
const uint32_t closeEndUs = micros();

// uint32_tの符号なし減算により、1回の計測区間がmicros()の
// wrap周期より短ければ、カウンタが途中でwrapしても差分を計算できる。
const uint32_t writeUs = writeEndUs - writeStartUs;
const uint32_t flushUs = flushEndUs - writeEndUs;
const uint32_t closeUs = closeEndUs - flushEndUs;
const uint32_t totalUs = closeEndUs - totalStartUs;

if (flushedCount > 0) {
    services::Logger::info(
        "StorageMgr",
        "Flushed %u records (%lu bytes): write=%lu us, flush=%lu us, "
        "close=%lu us, total=%lu us",
        static_cast<unsigned>(flushedCount),
        static_cast<unsigned long>(bytesWritten),
        static_cast<unsigned long>(writeUs),
        static_cast<unsigned long>(flushUs),
        static_cast<unsigned long>(closeUs),
        static_cast<unsigned long>(totalUs));
}
```

Arduino系の`micros()`を32 bit値として扱う場合、約71.6分でwrapする。ただし、開始値と終了値を`uint32_t`で保持し、`end - start`を符号なし減算すれば、1回の処理がwrap周期未満である限り、wrapをまたいでも正しい差分になる。差分を符号付き整数へ変換しない。

計測時の注意:

- `while`内でレコードごとのシリアルログを出さない。シリアル出力時間が測定値へ混入する。
- 結果ログはSD処理と`close()`の完了後に出す。
- ログ量が多い場合は毎回出さず、低速処理、最大値更新時、一定回数ごとに限定する。
- `write()`の戻り値が要求byte数より小さいshort writeは、時間値とは別にエラーとして扱う。
- `flush()`や`close()`に成功結果を返さないAPIでは、再open後のファイルサイズ、CRC、FRAMチェックポイントとの照合を継続する。
- デバッグログ有効時と無効時の両方で性能試験を行う。
- `micros()`の呼び出し自体にも小さなオーバーヘッドがあるため、非常に短い単発writeでは複数回の合計または平均も確認する。

最低限、次の統計をRAM上の診断カウンタへ保持する。

```text
flush_call_count
flushed_record_count
written_bytes_total
last_total_us
max_write_us
max_flush_us
max_close_us
max_total_us
slow_flush_count
short_write_count
```

必要に応じて、総時間のヒストグラムまたはp95／p99相当値も取得する。平均値だけではSDカードの数十～数百ms級の停止を見落とすため、SPIクロックとリングバッファ容量の判断には`max_total_us`を必ず使用する。

性能比較では、同じSDカード、同じレコード数、同じflush条件で`4 MHz`と候補周波数を測定する。周波数を上げても`writeUs`だけが短く、`flushUs`／`closeUs`や`max_total_us`が改善しない場合は、SPI転送速度が主因ではない。

## 4. 保存先の構成

SDカード上の正式な構成を次に固定する。

```text
/data/
├── environment/
│   └── YYYY-MM.csv
├── ppg/
│   └── YYYY-MM-DD/
│       └── HHMMSS/
│           ├── raw.ppg
│           ├── metadata.json
│           └── environment.csv
└── system/
    ├── events.csv
    └── storage.csv
```

例:

```text
/data/
├── environment/
│   └── 2026-08.csv
├── ppg/
│   └── 2026-08-11/
│       └── 140900/
│           ├── raw.ppg
│           ├── metadata.json
│           └── environment.csv
└── system/
    ├── events.csv
    └── storage.csv
```

### 4.1 命名規則

- ファイル名・ディレクトリ名はASCIIのみとする。
- 日付と時刻はUTCから生成する。
- `YYYY-MM`、`YYYY-MM-DD`、`HHMMSS`はゼロ埋めする。
- セッションIDは`YYYYMMDDTHHMMSSZ`形式とし、例は`20260811T140900Z`とする。
- 同一秒に複数セッションを開始しない。既存パスと衝突した場合は新規セッションを開始せず、次の秒まで待つかエラーを返す。
- 予約したセッションIDとパスは、最初のSDファイル作成前にFRAMへ記録する。
- 起動時復旧では新しいIDやパスを生成せず、FRAMに記録済みのIDとパスを再使用する。
- RTC/NTP時刻が無効な場合は正式なPPGセッションを開始しない。時刻異常をFRAMへ記録する。

### 4.2 一時ファイル

測定中は次の名前を使用する。

```text
raw.ppg.tmp
metadata.json.tmp
environment.csv.tmp
```

一時ファイルを同じセッションディレクトリ内で正式名へrenameする。別ディレクトリ間の移動を確定操作として使用しない。

## 5. SDファイル共通規約

- CSVおよびJSONはUTF-8、BOMなし、改行はLFとする。
- CSVの先頭行にはヘッダを置く。
- CSVフィールドにカンマ、二重引用符、CR、LFが含まれる場合はRFC 4180相当でエスケープする。
- 浮動小数点の欠測値は空欄とし、別の`status`または`status_bits`で理由を表す。`0`を欠測値の代用にしない。
- JSONに`NaN`、`Infinity`を出力しない。欠測は`null`とする。
- 絶対時刻はISO 8601 UTC形式、例`2026-08-11T14:09:00.123Z`とする。
- 内部識別子`record_id`と`session_id`を表示用時刻とは別に保持する。
- SDへの書き込み成功は、APIが返したバイト数、`flush()`、`close()`、再open後のサイズまたは内容検証を組み合わせて判定する。
- `flush()`の成功だけで電源断耐性を保証したとみなさない。最終判断は電源断試験で行う。

## 6. 月次環境履歴

### 6.1 保存先

```text
/data/environment/YYYY-MM.csv
```

通常の環境スナップショットは、FRAMへcommitしてから月次CSVへappendする。append後に書き込み結果を確認し、FRAMレコードを消費済みにする。

### 6.2 推奨列

```csv
record_id,timestamp,temp_c,rh_pct,pressure_pa,co2_ppm,voc_index,nox_index,pm1_0_ug_m3,pm2_5_ug_m3,pm4_0_ug_m3,pm10_ug_m3,status_bits
```

例:

```csv
record_id,timestamp,temp_c,rh_pct,pressure_pa,co2_ppm,voc_index,nox_index,pm1_0_ug_m3,pm2_5_ug_m3,pm4_0_ug_m3,pm10_ug_m3,status_bits
0000002A-00000117,2026-08-11T12:00:00.000Z,24.82,48.31,100842,612,108,2,3.1,5.2,7.4,8.1,0
```

`record_id`は再送時にも変えない。起動時リプレイで同じ`record_id`が保存先に存在すれば再appendせず、FRAM側だけを消費済みにする。

## 7. PPGセッション保存

### 7.1 セッション成果物

1回の測定を1ディレクトリへまとめる。

```text
/data/ppg/YYYY-MM-DD/HHMMSS/
├── raw.ppg
├── metadata.json
└── environment.csv
```

- `raw.ppg`: Red/IR RAWサンプルを格納したPPG Binary Format v1
- `metadata.json`: 測定条件、時刻、結果、品質、復旧状態、ファイル整合情報
- `environment.csv`: 開始時、測定中、終了時の環境値

### 7.2 RAM/PSRAMストリーム

PPG取得経路を次のように分離する。

```text
割り込み／poll通知
        ↓
MAX30102 FIFOの読み出し
        ↓
サンプルをリングバッファへ格納
        ↓
StorageServiceが連続領域を取得
        ↓
PPGブロックへシリアライズ
        ↓
raw.ppg.tmpへ書き込み
```

実装条件:

- ISRではフラグまたは通知だけを設定し、I²C、SD、CRC、JSON処理を行わない。
- PSRAMが利用可能ならPPGリングバッファをPSRAMへ確保する。
- PSRAMが利用できない場合は内部RAMへ縮小して確保し、バッファ時間を診断ログへ出す。
- 測定開始前に必要なバッファを確保し、測定中の動的確保を避ける。
- 単一リングまたはping-pongバッファを用い、取得側をSD待ちで長時間停止させない。
- SD書き込みは可能な限り4 KiB以上、推奨4～16 KiBのまとまりで行う。
- 1サンプルごとに`write()`や`flush()`を呼ばない。
- `flush()`は設定したブロック数、4～16 KiB、または1～5秒のうち先に到達した条件で実行できるようにする。
- バッファ高水位、最大水位、SD書き込み時間、ドロップ数、FIFO overflow数を計測する。
- バッファ枯渇・overflow時は件数を増やし、`metadata.json`とsystemイベントへ反映する。

バッファ保持時間は次で見積もる。

```text
buffer_seconds = buffer_bytes / (sample_rate_hz × sample_size_bytes)
```

v1の標準サンプルが8 bytes、100 Hzなら800 bytes/sである。実際のバッファサイズはSDカードの最大書き込み停止時間を実測して決める。

## 8. PPG Binary Format v1

### 8.1 共通規約

- マルチバイト整数はlittle-endianとする。
- 構造体のメモリイメージをそのまま書かず、各フィールドを明示的にシリアライズする。
- パディングを挿入しない。
- `uint32`サンプル値ではMAX30102の有効ビットだけを使用し、未使用上位ビットは0にする。
- CRCはCRC-32/ISO-HDLCを使用する。

CRCパラメータ:

```text
width   = 32
poly    = 0x04C11DB7
refin   = true
refout  = true
init    = 0xFFFFFFFF
xorout  = 0xFFFFFFFF
check("123456789") = 0xCBF43926
```

実装で使用する反転多項式は`0xEDB88320`である。

### 8.2 ファイル構造

```text
[File Header: 64 bytes]
[Block Header: 28 bytes]
[Block Payload: sample_count × 8 bytes]
[Block Header: 28 bytes]
[Block Payload: sample_count × 8 bytes]
...
[Footer: 40 bytes]
```

### 8.3 File Header

File Headerは64 bytes固定とする。

| Offset | Size | 型 | フィールド | 内容 |
|---:|---:|---|---|---|
| 0 | 4 | char[4] | magic | ASCII `PPG1` |
| 4 | 2 | uint16 | version | `1` |
| 6 | 2 | uint16 | header_size | `64` |
| 8 | 4 | uint32 | flags | 下記フラグ |
| 12 | 8 | uint64 | start_unix_us | UTC開始時刻、Unix epochからのµs |
| 20 | 2 | uint16 | sample_rate_hz | 設定サンプルレート |
| 22 | 2 | uint16 | sample_size_bytes | v1では`8` |
| 24 | 2 | uint16 | channel_mask | bit0=Red、bit1=IR |
| 26 | 1 | uint8 | sample_average | MAX30102設定値 |
| 27 | 1 | uint8 | reserved0 | `0` |
| 28 | 2 | uint16 | pulse_width_us | パルス幅、例`411` |
| 30 | 2 | uint16 | adc_range_na | ADCレンジ設定の実値 |
| 32 | 2 | uint16 | red_led_current_x10_ma | 0.1 mA単位 |
| 34 | 2 | uint16 | ir_led_current_x10_ma | 0.1 mA単位 |
| 36 | 4 | uint32 | expected_samples | 不明なら`0` |
| 40 | 4 | uint32 | block_payload_max | writerが使用する最大payload bytes |
| 44 | 8 | uint64 | session_id_hash | session_idの安定した64 bitハッシュ |
| 52 | 8 | uint8[8] | reserved1 | すべて`0` |
| 60 | 4 | uint32 | header_crc32 | offset 0～59のCRC |

`flags`:

| Bit | 意味 |
|---:|---|
| 0 | little-endian、常に1 |
| 1 | Red/IRをuint32で保存、v1では1 |
| 2 | Footerを持つ予定、常に1 |
| 3～31 | 予約、0 |

未知の予約bitが1のファイルを読む場合、readerは無条件に解釈せず、対応可否を判定する。

`session_id_hash`はUTF-8ではなくASCIIの`session_id`文字列に対するFNV-1a 64 bitとする。offset basisは`0xCBF29CE484222325`、primeは`0x00000100000001B3`とする。この値は高速な照合補助であり、session_idそのものの一意性保証や暗号学的検証には使用しない。

### 8.4 サンプルpayload

1サンプルは8 bytes固定とする。

| Offset | Size | 型 | フィールド |
|---:|---:|---|---|
| 0 | 4 | uint32 | red |
| 4 | 4 | uint32 | ir |

時刻は次で復元する。

```text
sample_time_us = start_unix_us + first_sample_index × 1,000,000 / sample_rate_hz
```

サンプル欠落が発生した場合でも`first_sample_index`は論理サンプル位置を維持する。正確な不連続位置を保存できない実装では、少なくともFooterと`metadata.json`へ総欠落数を記録する。

### 8.5 Block Header

Block Headerは28 bytes固定とする。

| Offset | Size | 型 | フィールド | 内容 |
|---:|---:|---|---|---|
| 0 | 4 | char[4] | magic | ASCII `BLK1` |
| 4 | 4 | uint32 | block_index | 0始まりの連番 |
| 8 | 4 | uint32 | first_sample_index | ブロック先頭の論理サンプル番号 |
| 12 | 2 | uint16 | sample_count | payload内サンプル数 |
| 14 | 2 | uint16 | sample_size_bytes | v1では`8` |
| 16 | 4 | uint32 | payload_size | `sample_count × 8` |
| 20 | 4 | uint32 | payload_crc32 | payload全体のCRC |
| 24 | 4 | uint32 | block_header_crc32 | offset 0～23のCRC |

検証条件:

- `magic == "BLK1"`
- `block_index`が前ブロック＋1
- `sample_size_bytes == 8`
- `payload_size == sample_count × sample_size_bytes`
- `payload_size <= block_payload_max`
- header CRCとpayload CRCが一致

1条件でも失敗したブロックと、それ以降は自動復旧時の有効データに含めない。

### 8.6 Footer／終端情報

Footerは40 bytes固定とする。

| Offset | Size | 型 | フィールド | 内容 |
|---:|---:|---|---|---|
| 0 | 4 | char[4] | magic | ASCII `END1` |
| 4 | 2 | uint16 | version | `1` |
| 6 | 2 | uint16 | footer_size | `40` |
| 8 | 8 | uint64 | end_unix_us | UTC終了時刻 |
| 16 | 4 | uint32 | block_count | 有効ブロック総数 |
| 20 | 4 | uint32 | sample_count | 保存済みサンプル総数 |
| 24 | 4 | uint32 | dropped_samples | 既知の欠落数 |
| 28 | 4 | uint32 | fifo_overflows | MAX30102 FIFO overflow回数 |
| 32 | 4 | uint32 | stream_crc32 | 全Block Payloadを順番に連結したCRC |
| 36 | 4 | uint32 | footer_crc32 | offset 0～35のCRC |

正常セッションの`raw.ppg`は、有効なHeader、0個以上の有効Block、有効なFooterを持つ。PPG測定として正式成功にするには1サンプル以上を要求する。

復旧された部分セッションにもFooterを付けるが、正常完了と混同しない。`metadata.json`の`completion.status`を`recovered_partial`とし、復旧理由と切り捨てたバイト数を記録する。

### 8.7 v1 readerの必須動作

- magic、version、サイズ、CRCを検証する。
- 未対応versionをv1として推測解釈しない。
- 最初の不正ブロックで停止し、最後の有効ブロック境界を返す。
- Footerがないファイルを正常完了扱いしない。
- ファイル末尾に余分なデータがある場合は警告または不正とする。
- 0件、最大値、切れたHeader、切れたBlock Header、切れたPayload、CRC不一致を安全に処理する。

## 9. metadata.json

### 9.1 目的

`metadata.json`にはRAWを解釈し、測定結果と保存品質を評価するために必要な情報を保存する。巨大な時系列配列は入れない。

### 9.2 必須例

```json
{
  "schema_version": 1,
  "session_id": "20260811T140900Z",
  "time_basis": "UTC",
  "start_time": "2026-08-11T14:09:00.000Z",
  "end_time": "2026-08-11T14:10:15.000Z",
  "completion": {
    "status": "complete",
    "recovered": false,
    "reason": null
  },
  "ppg_format": {
    "name": "PPG1",
    "version": 1,
    "endianness": "little",
    "sample_encoding": "uint32_red_uint32_ir",
    "sample_size_bytes": 8,
    "block_crc": "CRC-32/ISO-HDLC"
  },
  "max30102": {
    "sample_rate_hz": 100,
    "sample_average": 4,
    "pulse_width_us": 411,
    "adc_range_na": 16384,
    "red_led_current_ma": 6.4,
    "ir_led_current_ma": 6.4
  },
  "result": {
    "heart_rate_bpm": 67.4,
    "spo2_percent": 97.8,
    "valid": true
  },
  "quality": {
    "block_count": 75,
    "stored_samples": 7500,
    "dropped_samples": 0,
    "fifo_overflows": 0,
    "valid_sample_ratio": 0.98,
    "ring_high_water_bytes": 8192,
    "max_sd_write_ms": 42
  },
  "environment_summary": {
    "start_recorded": true,
    "periodic_records": 74,
    "end_recorded": true
  },
  "files": {
    "raw": {
      "name": "raw.ppg",
      "size_bytes": 62104,
      "stream_crc32": "A1B2C3D4"
    },
    "environment": {
      "name": "environment.csv",
      "rows": 76
    }
  },
  "firmware": {
    "version": "0.1.0",
    "build_id": "example"
  }
}
```

`completion.status`は少なくとも次を使用する。

- `complete`: 通常終了し、全検証と確定処理が完了
- `recovered_complete`: 内容は完了していたが、起動時にrename等を完了
- `recovered_partial`: 最終有効ブロックまでを部分復旧
- `aborted`: 実行中に明示的に中止
- `failed`: 有効な成果物として確定できない

`metadata.json`はセッション確定マーカーである。正式名の`metadata.json`が存在し、内容と他の2ファイルが検証できた場合にのみセッションを確定済みと判定する。

## 10. PPGセッション付帯environment.csv

### 10.1 保存タイミング

最低限、次の3種類を保存する。

1. `START`: PPG取得開始直前または直後の環境スナップショット
2. `PERIODIC`: 測定中。標準は1秒周期、または各センサの新規データ取得時
3. `END`: PPG取得停止直前または直後の環境スナップショット

環境センサの更新周期が異なる場合は、各値の`age_ms`またはステータスで鮮度を判断できるようにする。SCD41など新規値がないセンサについて、同じ値を記録する場合はstale状態を明示する。

### 10.2 推奨列

```csv
record_id,phase,offset_ms,timestamp,temp_c,rh_pct,pressure_pa,co2_ppm,voc_index,nox_index,pm1_0_ug_m3,pm2_5_ug_m3,pm4_0_ug_m3,pm10_ug_m3,status_bits
```

例:

```csv
record_id,phase,offset_ms,timestamp,temp_c,rh_pct,pressure_pa,co2_ppm,voc_index,nox_index,pm1_0_ug_m3,pm2_5_ug_m3,pm4_0_ug_m3,pm10_ug_m3,status_bits
0000002A-00000120,START,0,2026-08-11T14:09:00.000Z,24.82,48.31,100842,612,108,2,3.1,5.2,7.4,8.1,0
0000002A-00000121,PERIODIC,1000,2026-08-11T14:09:01.000Z,24.81,48.35,100841,612,109,2,3.1,5.2,7.4,8.1,0
0000002A-00000196,END,75000,2026-08-11T14:10:15.000Z,24.80,48.40,100838,614,110,2,3.2,5.3,7.5,8.2,0
```

STARTとENDはベストエフォートではなく、明示的に取得を試みる。取得できなかった場合も行を省略するだけでなく、`metadata.json`とsystemイベントへ失敗を記録する。

セッション中の環境行は小規模データとしてFRAM WALを経由できる。ただしPPG取得を阻害しないよう、FRAMキューが満杯の場合の優先順位を次とする。

```text
セッション状態／障害イベント
    > START・END環境値
    > PERIODIC環境値
    > 通常月次環境値
```

低優先度行を保存できなかった場合は欠落件数を記録する。

## 11. systemイベントのSDアーカイブ

### 11.1 events.csv

FRAMイベントジャーナルの未処理イベントを次へ追記する。

```text
/data/system/events.csv
```

推奨列:

```csv
record_id,timestamp,severity,source,event,session_id,detail
```

例:

```csv
record_id,timestamp,severity,source,event,session_id,detail
0000002A-000001A0,2026-08-11T02:15:22.000Z,WARN,SCD41,REINITIALIZED,,read_timeout
0000002A-000001A1,2026-08-11T14:09:42.000Z,ERROR,SD,WRITE_FAILED,20260811T140900Z,raw.ppg.tmp
0000002B-00000003,2026-08-11T14:10:01.000Z,INFO,RECOVERY,PPG_PARTIAL_RECOVERED,20260811T140900Z,last_valid_block=41
```

イベントはFRAMでREADYになった順にSDへアーカイブする。SDへ同じ`record_id`が存在する場合は再追記せず、FRAM側を消費済みにする。

### 11.2 storage.csv

ストレージ処理と復旧結果を次へ保存する。

```text
/data/system/storage.csv
```

推奨列:

```csv
record_id,timestamp,operation,session_id,path,result,bytes,duration_ms,retry_count,detail
```

記録対象:

- SD mount失敗／復旧
- ファイル作成、append、close、renameの失敗
- PPGセッション開始、確定、中止、部分復旧
- 起動時のtmp検出と判定結果
- FRAM WALのreplay件数と結果
- CRC不一致、truncate位置、欠落件数
- SD空き容量警告

`storage.csv`自身の書き込みを1件ごとに再帰的に`storage.csv`へ記録しない。`storage.csv`書き込み失敗はFRAMのsystemイベントへ1件だけ記録し、無限再帰を防ぐ。

## 12. FRAM WAL設計

### 12.1 論理レコード種別

既存FRAM形式へ少なくとも次を表現できるようにする。

| 種別 | 用途 | 主な内容 |
|---|---|---|
| `ENV_MONTHLY` | 月次環境値 | record_id、時刻、各測定値、status |
| `ENV_SESSION` | PPG付帯環境値 | record_id、session_id、phase、offset、各測定値 |
| `SYSTEM_EVENT` | 障害・診断イベント | record_id、severity、source、event、detail |
| `PPG_SESSION` | PPGトランザクション | session_id、path、状態、開始／終了時刻 |
| `PPG_CHECKPOINT` | RAW復旧位置 | block、sample、file size、stream CRC、drop数 |
| `PERSISTENT_STATE` | 小さな永続状態 | boot sequence、record sequence、最終処理位置など |

PPG RAW payloadはレコード種別に追加しない。

### 12.2 record_id

`record_id`は最低でも次の2要素から生成する。

```text
record_id = boot_sequence + monotonic_record_sequence
```

- `boot_sequence`は起動ごとにFRAMで更新する。
- `monotonic_record_sequence`は同一起動中に単調増加させる。
- 再送時にrecord_idを再生成しない。
- CSVでは固定幅の16進表現など、文字列比較可能な形へする。
- wrapを考慮し、少なくとも64 bit相当の一意性を確保する。

### 12.3 レコードcommit順

既存形式が同等以上の耐障害性を持つ場合はそれを使用する。新規実装では次を満たす。

```text
1. slotをWRITINGとして記録
2. headerとpayloadを書く
3. payload CRC／record CRCを書く
4. 全体をread-backして検証
5. 最後に状態をREADYへ更新
6. SD反映後、状態をCONSUMEDへ更新またはtailを進める
```

電源断で中途半端になった`WRITING`レコードはREADYとして処理しない。リングのhead/tailや世代番号を持つ管理領域は二重化し、世代番号とCRCで新しい有効コピーを選べるようにする。

### 12.4 容量不足

- 未処理レコードを暗黙に上書きしない。
- 高優先度の障害／セッション状態用に予約領域または予約slotを確保する。
- 使用率に警告・危険水位を設ける。
- 危険水位では通常環境値やPERIODIC環境値を抑制できる。
- 抑制件数そのものを高優先度カウンタへ残す。
- SD復旧後は古いREADYレコードから排出する。

## 13. PPGセッション処理順

### 13.1 開始

1. RTC/NTPの有効性を確認する。
2. SDがmount済みで書き込み可能か確認する。
3. SD空き容量が設定した最低値以上か確認する。
4. PPGリングバッファ、ブロックバッファ、CRC状態を初期化する。
5. `session_id`と最終パスを決定する。
6. 同じパスが存在しないことを確認する。
7. `PPG_SESSION: PREPARING`をFRAMへcommitする。
8. セッションディレクトリを作る。
9. `raw.ppg.tmp`、`environment.csv.tmp`、`metadata.json.tmp`を作る。
10. `raw.ppg.tmp`へHeaderを書き、書き込みサイズを検証する。
11. `environment.csv.tmp`へヘッダを書く。
12. START環境値を取得し、FRAMを経由して`environment.csv.tmp`へ反映する。
13. `PPG_SESSION: RECORDING`をFRAMへcommitする。
14. MAX30102のFIFO取得を開始する。

途中で失敗した場合は無理に測定を開始せず、FRAMへ失敗状態とイベントを残す。

### 13.2 測定中

1. MAX30102 FIFOを適切な優先度で読み出す。
2. Red/IRサンプルをRAM/PSRAMリングへ格納する。
3. 設定数が溜まったらPPGブロックを生成する。
4. Block Header CRCとPayload CRCを計算する。
5. `raw.ppg.tmp`へHeader、Payloadの順で書く。
6. 全payloadを対象に`stream_crc32`を更新する。
7. 設定した間隔で`flush()`する。
8. flush後、最後の確定block、sample数、file size、stream CRCを`PPG_CHECKPOINT`としてFRAMへcommitする。
9. START後は標準1秒周期でPERIODIC環境値を取得し、FRAMから`environment.csv.tmp`へ排出する。
10. SD書き込み遅延、リング高水位、ドロップ、FIFO overflowを更新する。

FRAMチェックポイントは毎サンプル更新しない。SD flush単位または数秒単位で更新し、I²CバスとFRAM書き込みの負荷を抑える。

### 13.3 正常終了

1. MAX30102の新規取得を停止する。
2. FIFO内の残りとリングバッファ内の残りを回収する。
3. 最終ブロックを書き込む。
4. END環境値を取得し、`environment.csv.tmp`へ反映する。
5. Footerを書き込む。
6. `raw.ppg.tmp`を`flush()`、`close()`する。
7. `environment.csv.tmp`を`flush()`、`close()`する。
8. `raw.ppg.tmp`を再openし、Header、全Block、Footer、CRC、サイズを検証する。
9. `environment.csv.tmp`のヘッダと最終完全行を検証する。
10. 最終情報を使って`metadata.json.tmp`を生成する。
11. `metadata.json.tmp`を`flush()`、`close()`し、JSONとして再読込検証する。
12. FRAMへ`PPG_SESSION: FINALIZING`をcommitする。
13. `raw.ppg.tmp`を`raw.ppg`へrenameする。
14. `environment.csv.tmp`を`environment.csv`へrenameする。
15. `metadata.json.tmp`を最後に`metadata.json`へrenameする。
16. 3ファイルを再openして、相互のsession_id、件数、サイズを検証する。
17. FRAMへ`PPG_SESSION: COMMITTED`をcommitする。
18. 対応するFRAM環境値、イベント、セッションレコードを消費済みにする。
19. `storage.csv`へセッション確定結果を記録する。

`metadata.json`のrenameを最後にする。FAT上の複数renameは単一トランザクションではないため、途中電源断は起動時リカバリで整合させる。

## 14. tmp→renameによるセッション確定規則

| 状態 | 判定 |
|---|---|
| 3つの`.tmp`のみ | 測定中または未確定 |
| `raw.ppg`、`environment.csv`が正式名、`metadata.json.tmp` | 最終確定途中 |
| 3つが正式名、`metadata.json`が有効 | 確定候補。相互検証後に確定済み |
| `metadata.json`のみ正式名 | 不整合。確定済みと扱わない |
| `.tmp`と正式名が混在 | 起動時リカバリ対象 |

既存の正式名へrenameで上書きしない。同名正式ファイルが存在する場合は内容を照合し、同一なら成功済みとして扱い、異なる場合は衝突イベントを記録して自動上書きを停止する。

## 15. 起動時リカバリ

### 15.1 起動順

1. reset reasonを取得する。
2. I²CとFRAMを初期化する。
3. FRAM superblock／管理領域のCRCと世代を検証する。
4. boot sequenceを安全に更新する。
5. RTC時刻の有効性を確認する。
6. SDをmountする。失敗しても装置全体を永久停止しない。
7. FRAM内のREADYレコードと未完了PPGセッションを列挙する。
8. SD上の対象セッションディレクトリと`.tmp`を照合する。
9. PPGセッションを先に整合させる。
10. 月次環境値、セッション環境値、systemイベントをrecord_idで重複確認しながらreplayする。
11. 結果を`events.csv`と`storage.csv`へ記録する。
12. 未解決データが残る場合は通常動作へ移行してよいが、診断状態とFRAMレコードを保持する。

### 15.2 raw.ppg.tmpの検査

1. 64-byte Headerを読み、magic、version、サイズ、CRCを検証する。
2. Blockを先頭から順に検証する。
3. 最初の不完全BlockまたはCRC不一致で停止する。
4. 最後の有効Blockの末尾offsetを求める。
5. Footerがあり、内容とCRCが有効か確認する。

判定:

- Header、全Block、Footerが有効: `RECOVERABLE_COMPLETE`
- Headerと1つ以上のBlockが有効、Footerなし／末尾不正: `RECOVERABLE_PARTIAL`
- Header不正または有効Blockなし: `FAILED`

### 15.3 部分復旧

`RECOVERABLE_PARTIAL`では、使用するSDライブラリが安全なtruncateを提供し、その動作を試験済みの場合にのみ、最後の有効Block末尾へtruncateする。その後、復旧時刻、件数、CRCを用いたFooterを付ける。

truncateが安全に利用できない場合は、同じディレクトリ内に新しい回復用一時ファイルを作り、有効なHeaderとBlockだけをコピーしてFooterを付け、元ファイルは検証完了まで保持する。コピー先を正式化した後であっても、元ファイルの自動削除は別途明示された保守方針に従う。

復旧後:

- `environment.csv.tmp`は最後の完全なLF終端行までを採用する。
- END行がなければ復旧イベントをEND相当の情報としてmetadataへ記録し、実測値を捏造しない。
- `metadata.json.tmp`を再生成または補正する。
- `completion.status = "recovered_partial"`とする。
- raw、environment、metadataの順でrenameし、metadataを最後にする。

### 15.4 復旧不能

Header不正、session_id衝突、別内容の正式ファイルとの衝突などは自動上書き・自動削除しない。

- FRAMのセッション状態を`RECOVERY_FAILED`にする。
- 詳細をsystemイベントへ記録する。
- 対象を毎起動時に無限再試行しないよう、試行回数と最終理由をFRAMへ保持する。
- ユーザーが回収できるよう一時ファイルを保持する。
- 新しい測定が安全に開始できるかを別途判定する。

## 16. 重複防止

### 16.1 CSVレコード

電源断が「SD append成功後、FRAMをCONSUMEDにする前」に発生すると、同じFRAMレコードが再送される。このため全WAL由来CSV行に`record_id`を保存する。

replay時は対象CSVをストリーム走査し、同じ`record_id`が存在するか確認する。

- 存在する: 再appendせずFRAMを消費済みにする。
- 存在しない: append、flush、close、確認後にFRAMを消費済みにする。
- 同じrecord_idで内容が異なる: 整合性エラーとして自動処理を停止する。

月次環境CSVは規模が小さいため全走査を許容する。`events.csv`と`storage.csv`が大きくなり性能問題が出た場合は、正式な索引仕様またはローテーション仕様を別版で追加する。根拠なく末尾数行だけを見て重複なしと判定しない。

### 16.2 PPGセッション

- session_idと最終パスをFRAMで固定する。
- `metadata.json`内のsession_idとパス由来の時刻を照合する。
- 有効な同一session_idの確定済みセッションがあれば再作成しない。
- rename先が存在する場合は上書きせず、サイズ、CRC、session_idを照合する。
- 同じパスに異なるsession_idがある場合は`PATH_COLLISION`として停止する。
- START命令の二重受付を状態機械で防ぐ。

### 16.3 replay順序

原則としてFRAMの論理順序を維持する。同じ保存先に対して後続レコードを先にCONSUMEDにしない。レコードごとの成功・失敗を記録し、1件の不正データが無関係な高優先度イベントの排出を永久に妨げないよう、隔離状態を設ける。

## 17. 障害時の動作

### 17.1 SD未挿入／mount失敗

- 起動を永久停止しない。
- 月次環境値とsystemイベントはFRAM容量内で保護する。
- PPG RAWを保存できないため、PPGセッションは開始しない。
- 測定中にSDが失われた場合はリングバッファ内のデータを保ったまま有限回再試行する。
- 復旧しなければ部分セッションとして終了処理を試み、実行不能ならFRAMにチェックポイントと障害を残す。

### 17.2 SD容量不足

- 測定前に必要容量の見積もりと安全余裕を確認する。
- 測定中に容量不足となった場合は書き込み失敗として扱い、無限再試行しない。
- 既存の正式データを自動削除しない。
- 容量不足イベントをFRAMへ保存する。

### 17.3 FRAM異常

- CRC不一致やI²C失敗を無視しない。
- WAL保証がない状態で「保護済み」と表示しない。
- SDが正常でも、復旧保証が低下したdegraded状態を通知する。
- FRAMを自動初期化する前に、既存版、複製管理領域、read-backを確認する。

### 17.4 時刻異常

- 無効時刻から`1970-01-01`などの正式パスを作らない。
- 時刻が後退した場合は既存パス衝突を確認する。
- 時刻補正をsystemイベントへ記録する。
- PPG測定中のwall clock補正があっても、サンプル同期は開始時刻＋sample index／offset_msで維持する。

## 18. 実装構成

既存の一方向依存レイヤを維持する。

```text
Application
    ↓
Services
    ↓
Drivers
    ↓
HAL
```

推奨責務:

| コンポーネント | 責務 |
|---|---|
| `StorageCoordinator` | WAL→SD処理、優先順位、replay |
| `FramJournal` | FRAMレコードcommit、列挙、consume、CRC |
| `SdArchive` | ディレクトリ、CSV、JSON、rename、検証 |
| `PpgSessionManager` | セッション状態機械、開始／停止／復旧 |
| `PpgBinaryWriter` | PPG1の明示的シリアライズ、CRC、Footer |
| `PpgBinaryReader` | 検証、最後の有効Block検出 |
| `PpgRingBuffer` | RAM/PSRAMバッファと高水位管理 |
| `EnvironmentSnapshotService` | 月次／PPG付帯スナップショット生成 |
| `SystemEventService` | イベント生成とSDアーカイブ |

禁止事項:

- センサドライバから`SD.h`を直接呼ばない。
- ApplicationからFRAMの物理アドレスを直接操作しない。
- PPG取得ISRからファイル操作をしない。
- `main.cpp`へファイル形式ロジックを埋め込まない。
- 保存失敗時に`abort()`や無限ループで全センサを停止しない。

## 19. セッション状態機械

```text
IDLE
  ↓ start
PREPARING
  ↓ header/tmp/START成功
RECORDING
  ↓ stop
FINALIZING
  ↓ validate＋rename
COMMITTED

PREPARING / RECORDING / FINALIZING
  ├─ recoverable error → RECOVERY_PENDING
  ├─ explicit stop     → ABORTING
  └─ fatal error       → FAILED

起動時:
RECOVERY_PENDING
  ├─ 全体復旧 → COMMITTED
  ├─ 部分復旧 → COMMITTED_PARTIAL
  └─ 復旧不能 → RECOVERY_FAILED
```

各遷移をFRAMへcommitしてから、遷移先に対応する破壊的または外部的な処理を進める。再起動後はFRAM状態とSD実体を照合し、どちらか一方だけで判断しない。

## 20. 実装作業手順

### Phase 1: 既存実装の保全と定数化

- [ ] 既存FRAMメモリマップ、format version、CRC、head/tail、初期化条件を文書化する。
- [ ] MAX30102とFRAMのI²Cアドレス非衝突を確認する。
- [ ] `/data`以下のパス生成を専用関数へ集約する。
- [ ] UTC時刻の有効性判定を実装する。
- [ ] record_id、session_idの生成と再利用規則を実装する。

### Phase 2: PPG1 codec

- [ ] Header、Block Header、Sample、Footerをフィールド単位でserialize／deserializeする。
- [ ] CRC-32/ISO-HDLCを実装または既存実装と統一する。
- [ ] `123456789`のCRCが`0xCBF43926`になることを確認する。
- [ ] golden fileを生成し、PC側readerと組み込み側readerで一致させる。
- [ ] 切れたファイルとCRC破損のテストを作る。

### Phase 3: FRAM WAL拡張

- [ ] 論理レコード種別を既存形式へ追加する。
- [ ] WRITING→READY→CONSUMEDまたは同等のcommit手順を実装する。
- [ ] read-backとCRC検証を実装する。
- [ ] 容量水位、優先順位、予約領域を実装する。
- [ ] boot sequenceとrecord sequenceを電源断に耐える形で保持する。

### Phase 4: SDアーカイブ

- [ ] 必要ディレクトリを冪等に作成する。
- [ ] 月次environment.csvのヘッダ生成とappendを実装する。
- [ ] system/events.csvとsystem/storage.csvを実装する。
- [ ] CSV escape、欠測値、statusの単体試験を行う。
- [ ] append済みrecord_idの重複検出を実装する。

### Phase 5: PPG直接ストリーム

- [ ] 測定前のRAM/PSRAMリング確保を実装する。
- [ ] FIFO読み出しとSD writerを分離する。
- [ ] ブロック書き込み、flush、FRAMチェックポイントを実装する。
- [ ] 高水位、最大SD停止時間、drop、overflowを計測する。
- [ ] START／PERIODIC／END環境値を付帯保存する。

### Phase 6: セッション確定

- [ ] 3つのtmpファイルを生成する。
- [ ] raw、environment、metadataの順で検証する。
- [ ] raw→environment→metadataの順でrenameする。
- [ ] metadataを確定マーカーとして扱う。
- [ ] rename途中の混在状態を判定できるようにする。

### Phase 7: 起動時リカバリ

- [ ] FRAMの未処理レコードを列挙する。
- [ ] raw.ppg.tmpの最後の有効Blockを検出する。
- [ ] 完了済みだがrename前のセッションを確定する。
- [ ] 部分セッションを安全に復旧する。
- [ ] 復旧不能データを上書き・削除せず隔離状態にする。
- [ ] FRAM WALをrecord_idで冪等にreplayする。

### Phase 8: 統合試験

- [ ] 正常な長時間PPG取得を実施する。
- [ ] SD未挿入、途中抜去、書き込み失敗、容量不足を試験する。
- [ ] 各commit境界で電源断試験を行う。
- [ ] 再起動後の重複、欠落、CRC、tmp残留を検証する。
- [ ] systemイベントとstorage監査ログを照合する。

## 21. 試験仕様

### 21.1 正常系

- 月境界をまたいで正しい`YYYY-MM.csv`へ保存される。
- 日境界をまたいでもセッション開始時のパスとsession_idが維持される。
- 100 Hz以上の設定サンプルレートで想定時間のPPGを取得できる。
- PC側readerが全Block CRC、Footer、sample countを検証できる。
- environment.csvにSTART、PERIODIC、ENDが存在する。
- metadataの件数、サイズ、CRCが実ファイルと一致する。
- PPG中も環境センサと表示の必要な処理が継続する。

### 21.2 CRC／破損系

次を個別に破損させ、readerと起動時リカバリの結果を確認する。

- Header magic
- Header CRC
- Block Header magic
- Block Header CRC
- Block Payload 1 byte
- Block Payload末尾の切断
- Footer CRC
- Footer欠落
- Footer後の余分なbytes
- metadata.jsonの途中切断
- environment.csvの途中行

### 21.3 SD障害

- 起動時にSDなし
- mount後にカードを抜去
- PPG Header書き込み中の失敗
- Block書き込み中の失敗
- flush／close失敗
- rename失敗
- 空き容量不足
- 再挿入後のmountとFRAM排出

PPG中の物理抜去はファイルシステムを破損する可能性があるため、試験用カードと回収可能な電源構成で行う。

### 21.4 電源断注入点

最低でも次の各点で電源を遮断し、各点を複数回実施する。

1. FRAMレコードをWRITINGにした直後
2. payload書き込み中
3. READYへ更新した直後
4. CSV append後、FRAM consume前
5. PPG session pathをFRAMへ保存した直後
6. raw Header書き込み中
7. Block Header書き込み中
8. Block Payload書き込み中
9. Block書き込み後、FRAM checkpoint前
10. checkpoint後、次Block前
11. Footer書き込み中
12. Footer書き込み後、close前
13. close後、raw rename前
14. raw rename後、environment rename前
15. environment rename後、metadata rename前
16. metadata rename直後、FRAM COMMITTED前
17. FRAM COMMITTED後、旧レコードconsume中

各試験で次を確認する。

- 起動不能や無限リセットにならない。
- READYでないFRAMレコードを誤ってreplayしない。
- 同一record_idのCSV行が重複しない。
- 確定済みPPGセッションを重複作成しない。
- raw.ppgは最後の有効Blockまで検証できる。
- 部分復旧は`complete`と偽装されない。
- 不明なデータを自動削除・上書きしない。
- 復旧内容がevents.csv／storage.csvへ残る。

### 21.5 長時間・性能試験

- 想定最大時間の2倍以上、PPGを連続取得する。
- 現行の`4 MHz`で必要帯域を満たし、dropとFIFO overflowが発生しないことを最初に確認する。
- `4 MHz`がボトルネックと判断された場合のみ、3.3節の手順でロジックアナライザ／オシロスコープ確認と段階的な周波数評価を行う。
- `flushPendingToSd()`のwrite、flush、close、total時間を3.4節の方法で計測する。
- 平均値だけでなく`max_total_us`とslow flush回数を記録し、リングバッファ保持時間と比較する。
- 低速なSDカードでもリング高水位が許容値内か確認する。
- PPG中の最大SD write時間を記録する。
- FRAM使用率が安定して戻ることを確認する。
- ヒープ、PSRAM、ファイルハンドルが測定ごとに減少しないことを確認する。
- 数百回のセッション開始／終了でパス衝突やtmp残留が増えないことを確認する。

## 22. 完了条件（Definition of Done）

以下をすべて満たしたとき完了とする。

### 保存構成

- [ ] `/data/environment/YYYY-MM.csv`へ月次環境履歴を保存できる。
- [ ] `/data/ppg/YYYY-MM-DD/HHMMSS/{raw.ppg,metadata.json,environment.csv}`を生成できる。
- [ ] `/data/system/{events.csv,storage.csv}`へアーカイブできる。
- [ ] 正式ファイルのほかに理由不明のファイルを生成しない。

### PPG形式

- [ ] PPG Binary Format v1のmagic、version、64-byte Headerを実装済みである。
- [ ] すべてのBlockにHeader CRCとPayload CRCがある。
- [ ] 正常ファイルに有効なFooter／終端情報がある。
- [ ] 組み込み側とPC側のreaderで同じ件数・CRCになる。
- [ ] 未対応versionと破損ファイルを安全に拒否できる。

### データ経路

- [ ] PPG RAWがRAM/PSRAM→SDへ直接ストリームされ、FRAMへRAW本体を書いていない。
- [ ] リング高水位、ドロップ、FIFO overflow、SD書き込み時間を記録できる。
- [ ] 開始時、測定中、終了時の環境値がPPGセッションへ付帯する。
- [ ] 現行`4 MHz`で帯域要件を満たすか実測し、不足時に限って信号確認と段階的な周波数評価を実施している。
- [ ] SD書き込みループとflush／closeを分離計測し、最大総時間をバッファ設計とSPIクロック判断へ反映している。

### 確定と復旧

- [ ] 3つのtmpファイルを使用している。
- [ ] raw、environment、metadataの順にrenameし、metadataを最後に確定している。
- [ ] 起動時にtmpとFRAM状態を照合できる。
- [ ] 最後の有効PPG Blockまで部分復旧できる。
- [ ] 復旧不能データを自動上書き・削除しない。
- [ ] 部分復旧を正常完了と区別できる。

### WALと重複防止

- [ ] 小規模データはFRAM READY→SD反映→FRAM CONSUMEDの順で処理する。
- [ ] SD失敗時に未処理FRAMレコードが残る。
- [ ] systemイベントをFRAMからevents.csvへ排出できる。
- [ ] record_idによるreplayでCSV行が重複しない。
- [ ] session_idと内容照合によりPPGセッションが重複しない。

### 試験

- [ ] 正常系、破損系、SD障害、長時間試験が合格している。
- [ ] 17か所の電源断注入点を各3回以上実施し、重大な不整合がない。
- [ ] 電源断後に起動不能、無限リセット、無限replayが発生しない。
- [ ] 復旧結果とデータ損失範囲がevents.csv／storage.csv／metadata.jsonで説明できる。
- [ ] 試験結果、使用SDカード、SPIクロック、バッファサイズ、firmware buildを記録している。

## 23. 成果物

実装完了時に次を提出またはリポジトリへ追加する。

- 本作業指示書
- PPG Binary Format v1のcodec実装
- PC側のPPG検証／抽出ツール
- golden PPGファイルと期待値
- FRAM WAL／SD／recoveryの単体試験
- 電源断・SD障害試験記録
- 生成されたサンプルディレクトリ一式
- `events.csv`、`storage.csv`、`metadata.json`のサンプル

## 24. 実装時に変更してはならない判断

次は設計変更として扱い、実装都合だけで変更しない。

- FRAMを長期保存用主ストレージへ変更すること
- PPG RAW全体をFRAMへ保存すること
- SDへ先に書き、小規模WAL対象をFRAMへ後書きすること
- PPG RAWをCSV化すること
- CRCなしのRAW連続列へ変更すること
- `.tmp`を使わず、測定中から正式名を公開すること
- `metadata.json`より先にセッション確定済みと扱うこと
- record_idなしでWALをreplayすること
- 復旧時に既存正式ファイルを無条件上書きすること
- 破損／未完了データを記録なしに削除すること

上記を変更する場合は、ファイル形式version、移行方法、既存データ互換性、電源断時の性質をADRで明示してから実装する。
