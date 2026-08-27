# BME690追加作業実施書

| 項目 | 内容 |
|---|---|
| 文書種別 | 実装・配線・試験作業実施書 |
| 文書版 | 1.0 |
| 作成日 | 2026-08-27 |
| 対象 | `env_bio_sense` / Seeed Studio XIAO ESP32S3 |
| 接続方式 | 既存I2Cバスへ追加 |
| 採用I2Cアドレス | `0x76`（SDOをGND側へ固定） |
| 初期保存対象 | BME690補償済み温度・湿度・気圧・生ガス抵抗・有効性情報 |
| OLED | 変更しない |

## 1. 目的

既存のセンサ、PPG取得、FRAM＋SD保存、OLED表示を維持したまま、BME690を共通I2Cバスへ追加する。初期段階ではBME690の測定結果を独立データとして5秒周期の既存CSVへ保存し、実機での連続取得、ガスセンサの安定性、既存I2C機器への影響を評価できる状態にする。

本作業の最小完了状態は次のとおりとする。

1. 起動時にBME690を`0x76`で識別できる。
2. BME690を接続しても既存I2C機器がすべて動作する。
3. `BME690_GasResistance_Ohm`を含むBME690専用列がCSVへ継続記録される。
4. BME690が未接続または一時故障しても、既存センサ、PPG、FRAM、SD、Web、OLEDの動作を継続する。

## 2. 今回の決定事項

### 2.1 初期実装はSensorAPIによる生データ保存とする

初期実装ではBosch公式`BME690_SensorAPI`を使用し、次を取得する。

- BME690温度 `[°C]`
- BME690相対湿度 `[%RH]`
- BME690気圧 `[hPa]`
- 補償済み生ガス抵抗 `[Ω]`
- 新規データ、ガス測定有効、ヒーター安定の各状態
- ガスヒータープロファイル番号

BME690の生ガス抵抗は、IAQ、VOC濃度、CO2濃度ではない。湿度、温度、センサ履歴、設置状態の影響を受けるため、CSV列名に`RawVOC`、`IAQ`、`CO2`などの誤解を招く名称を使用しない。

### 2.2 BSECは第2段階とする

BME690からIAQ、bVOC相当値、CO2相当値、ガス分類を得る場合はBSEC 3.2.0.0以上が必要となる。BSECには構成、状態保存、呼出周期、ライセンス、対象CPUバイナリの管理が追加で必要なため、本作業では導入しない。

将来BSECを追加するときは、本書の生ガス抵抗列を残したまま別列として追加し、SGP41の`VOC_Index`／`NOx_Index`とも混同しない。

### 2.3 既存測定値を置き換えない

- 代表温湿度は引き続きSHT45とする。
- 代表気圧、絶対高度、SCD41気圧補償は引き続きBMP581を使用する。
- 既存のVOC／NOx指標は引き続きSGP41を使用する。
- BME690の温湿度・気圧は比較・診断用の独立列として保存する。
- OLEDのページ、レイアウト、表示項目は変更しない。
- WebグラフへのBME690系列追加も今回の完了条件に含めない。CSVの閲覧・ダウンロードは従来どおり可能とする。

## 3. I2Cアドレス計画

### 3.1 現行コードから確認した使用アドレス

| アドレス | デバイス | 根拠・備考 |
|---:|---|---|
| `0x3C` | SSD1306 OLED | `DisplayManager`で固定 |
| `0x44` | SHT45 | `Sht45Sensor`で指定 |
| `0x47` | BMP581 | Adafruit STEMMA QT構成 |
| `0x50` | MB85RC256V FRAM Bank 0 | 実装時の搭載数による |
| `0x51` | MB85RC256V FRAM Bank 1 | 2個目を搭載している場合 |
| `0x57` | MAX30102 | 固定指定 |
| `0x59` | SGP41 | デバイス既定アドレス |
| `0x62` | SCD41 | デバイス既定アドレス |
| `0x76` | **BME690** | **今回採用** |

BME690はSDOの状態により`0x76`または`0x77`を選択できる。現行構成では両方とも空いているが、今回は`0x76`へ固定する。SDOはフローティングにしない。

I2Cスキャンは同一アドレスへ複数デバイスが接続されていても、通常は1個のACKとしてしか見えない。同一アドレス衝突をスキャン結果だけで否定してはならず、上表、実配線、各デバイスの識別レジスタを組み合わせて確認する。

### 3.2 起動時に期待するスキャン結果

2個のFRAMを含む現行構成では、BME690追加後に次が1回ずつ検出されることを期待する。

```text
0x3C, 0x44, 0x47, 0x50, 0x51, 0x57, 0x59, 0x62, 0x76
```

FRAMが1個だけの場合は`0x51`を除く。追加前に既に`0x76`が検出される場合は作業を中断し、接続中の機器を特定する。どうしても`0x76`を使用できない場合に限り、BME690のSDOをVDDIOへ固定してコードと本書の採用値を`0x77`へ変更する。

## 4. ハードウェア作業

### 4.1 配線

| XIAO ESP32S3 / 既存バス | BME690側 | 条件 |
|---|---|---|
| `3V3` | `VDD` / `VCC` | 使用するブレークアウト基板の電源仕様を事前確認する |
| `GND` | `GND` | 共通GND |
| `D4 / GPIO6` | `SDA` / `SDI` | 既存SDAへ並列接続 |
| `D5 / GPIO7` | `SCL` / `SCK` | 既存SCLへ並列接続 |
| `VDDIO` | `CSB` | I2Cモード選択。基板上で処理済みなら追加配線不要 |
| `GND` | `SDO` / `ADDR` | `0x76`選択。フローティング禁止 |

注意事項:

- 裸のBME690はVDD `1.71～3.6 V`、VDDIO `1.2～3.6 V`である。5 Vを直接印加しない。
- 市販ブレークアウト基板ではレギュレータ、レベル変換、CSB、SDO、プルアップの実装が異なる。使用基板の回路図を正とし、端子名だけで判断しない。
- 既存モジュールのI2Cプルアップ抵抗は並列になる。追加後の合成抵抗、立上り波形、通信エラー数を確認し、必要以上に強いプルアップにしない。
- BME690のガスヒーター動作時には瞬間的な電流増加と自己発熱がある。センサ近傍のデカップリングを確保し、SHT45から距離を取り、BME690の排気・通気口を筐体や接着剤で塞がない。
- ガスセンサをシリコーン、溶剤、接着剤、洗浄剤の蒸気へ不用意にさらさない。実装・筐体材料の影響を試験記録へ残す。

### 4.2 配線前後の確認手順

1. 現行ファームウェアでI2Cスキャン結果を保存する。
2. 電源を切る。
3. BME690のSDO／ADDR設定が`0x76`であることをテスターまたは回路図で確認する。
4. SDA、SCL、3.3 V、GND、CSB、SDOを配線する。
5. SDA-GND、SCL-GND、3.3 V-GNDの短絡がないことを確認する。
6. 通電し、`0x76`が追加され、既存アドレスが消えていないことを確認する。
7. BME690のチップID`0x61`とVariant ID`0x02`を読み、BME690であることを確認する。

## 5. ソフトウェア構成

### 5.1 公式ドライバー

Bosch公式`BME690_SensorAPI`のリリース`v1.1.0`を固定して使用する。`master`追従は行わない。PlatformIOから取得できない場合は、公式リポジトリの次のファイルと`LICENSE`を`lib/BME690_SensorAPI/`配下へ版固定で配置する。

```text
bme69x.c
bme69x.h
bme69x_defs.h
LICENSE
```

依存版を変更するときは、湿度補償を含むリリースノート、実機CSV比較、24時間試験を再実施する。

### 5.2 新規ドライバー

次を追加する。

```text
include/drivers/sensors/bme690_sensor.h
src/drivers/sensors/bme690_sensor.cpp
```

`Bme690Sensor`は`ISensor`を実装し、BME690専用の`readData(core::Bme690Data&)`を提供する。既存の`IEnvironmentSensor::readEnvironment()`へ直接統合しない。既存の集約処理では複数センサが同じ`EnvironmentData`を順に上書きしているため、そこへBME690を加えるとSHT45やBMP581の代表値を意図せず置き換えるおそれがある。

最低限の公開機能:

```cpp
class Bme690Sensor : public ISensor {
public:
    core::SensorId id() const override;
    bool begin() override;
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override;
    core::ErrorCode lastError() const override;
    uint32_t lastSuccessMs() const override;
    bool readData(core::Bme690Data& out) const;
};
```

### 5.3 I2Cコールバックと排他制御

- 既存の`Wire`と`hal::I2cLockGuard`を使用する。
- BME690ドライバー内で`Wire.begin()`を再実行しない。
- BME690ドライバー内で`Wire.setClock()`を実行しない。
- read/writeコールバックは、設定済みの`0x76`だけへアクセスする。
- readではレジスタアドレス送信後にrepeated startを使用し、要求バイト数との一致を確認する。
- writeでは`Wire.endTransmission()`の戻り値を確認する。
- I2Cロックを保持したままヒーター時間や測定完了を待たない。
- ロック取得失敗はバス故障とせず、その周期の測定をスキップして次回再試行する。

現行バスは`I2cBus::begin()`で400 kHzに設定される。BME690はStandard／Fast modeに対応するため、今回の追加を理由にバス周波数を変更しない。MAX30102初期化後を含め、実際のSCL周波数と全デバイスの連続動作を実機で確認する。

### 5.4 非ブロッキング測定状態機械

Bosch公式forced-mode例の初期設定を基準に、次を採用する。

```text
filter      = BME69X_FILTER_OFF
odr         = BME69X_ODR_NONE
os_hum      = BME69X_OS_16X
os_pres     = BME69X_OS_16X
os_temp     = BME69X_OS_16X
heater      = enabled
heater temp = 300 °C
heater dur  = 100 ms
mode        = BME69X_FORCED_MODE
```

初期測定周期は5秒とし、既存のFRAM記録周期と揃える。実装は次の状態を持つ。

```text
Idle
  └─ 5秒周期到達 → forced measurementを開始 → Waiting

Waiting
  ├─ 測定期限前 → 何もせずreturn
  └─ 測定期限後 → データ読出し → Idle
```

測定期限は`bme69x_get_meas_dur()`の戻り値とヒーター時間から計算する。`delay()`や長い`delayMicroseconds()`でメインループを停止しない。これによりMAX30102 FIFO取得、SCD41／SGP41、OLED、Wi-Fi、ストレージ処理を妨げない。

### 5.5 測定値の採用条件

温湿度・気圧は、新規データがあり、数値が有限で次の範囲内の場合に保存対象とする。

```text
temperature: -40 ～ +85 °C
humidity   :   0 ～ 100 %RH
pressure   : 300 ～ 1100 hPa
```

SensorAPIの気圧出力はPaであるため、CSV・スナップショットへ格納する前に`/ 100.0f`してhPaへ変換する。

ガス抵抗は次をすべて満たす場合だけ有効とする。

- 新規データあり
- `GASM_VALID`あり
- `HEAT_STAB`あり
- 数値が有限かつ正値

条件を満たさない行ではガス抵抗を`NaN`、`BME690_GasValid`を`0`として保存する。センサAPIのstatus値自体は診断列へ残す。

### 5.6 エラー分離

- BME690初期化失敗は`SystemStatus::bme690State`だけを`Error`または`Offline`にする。
- `SensorManager::begin()`全体はBME690の成否にかかわらず継続する。
- 単発のNACKやロックタイムアウトでセンサを永久停止しない。
- 連続読出し失敗回数を保持し、ログを毎周期出さず、状態遷移時と一定回数ごとに限定する。
- オフライン後は60秒程度の低頻度で再初期化する。再初期化時も他デバイスの処理を長時間ブロックしない。

## 6. データモデル変更

### 6.1 スナップショット

`include/core/sensor_types.h`へ次相当を追加する。

```cpp
struct Bme690Data {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    float gasResistanceOhm {};
    uint32_t timestampMs {};
    uint8_t gasIndex {};
    uint8_t status {};
    bool tphValid {false};
    bool gasValid {false};
    bool heaterStable {false};
};
```

併せて次を追加する。

- `SensorId::Bme690`
- `SensorSnapshot::bme690`
- `SystemStatus::bme690State`

`EnvironmentData`の既存`temperatureC`、`humidityRh`、`pressureHpa`、`vocIndex`は変更しない。

### 6.2 SensorManager

`SensorManager`へ`Bme690Sensor bme690_;`を追加し、次の順で統合する。

1. `begin()`で既存センサと独立して初期化する。
2. `update()`から毎ループ呼び出し、ドライバー内部の非ブロッキング状態機械を進める。
3. `readData()`の結果を`snapshot_.bme690`へ反映する。
4. `status_.bme690State`を更新する。
5. 既存`EnvironmentData`集約順序には追加しない。

## 7. FRAM・CSV保存変更

### 7.1 FRAMレコード

`SensorRecordV4`を`SensorRecordV5`へ更新し、末尾へ次を追加する。

```cpp
float bme690TemperatureC;
float bme690HumidityRh;
float bme690PressureHpa;
float bme690GasResistanceOhm;
uint8_t bme690GasIndex;
uint8_t bme690Status;
```

`SensorValidFlags`へ少なくとも次を追加する。

```cpp
VALID_BME690_TPH = 1u << 9;
VALID_BME690_GAS = 1u << 10;
```

現行のpacked `SensorRecordV4`は99 byte、`PersistentRecordV4`は108 byteである。上記18 byteを追加したpacked V5は126 byteとなる見込みで、現行`RECORD_SLOT_SIZE = 128`へ収まる。実装後は必ず次の`static_assert`を維持し、コンパイラ実測サイズも起動ログまたは試験記録へ残す。

```cpp
static_assert(sizeof(PersistentRecordV5) <= RECORD_SLOT_SIZE,
              "PersistentRecordV5 exceeds RECORD_SLOT_SIZE");
```

`RECORD_SLOT_SIZE`は128 byteのまま変更しないため、FRAMの最大480レコード、5秒周期で約40分のバッファ時間を維持できる。

### 7.2 FRAMフォーマット移行

`FRAM_FORMAT_VERSION`を`5`へ上げる。既存V4レコードを新しい構造体として読み出してはならない。

書込み前の必須手順:

1. 旧ファームウェアでFRAM pending件数を0までflushする。
2. SD上に旧CSVが存在することを確認する。
3. V5ファームウェアを適用する。

V5ファームウェア側ではV4 superblockを検出した場合、V4の`readIndex`／`writeIndex`からpending件数を計算する。

- pendingが0: リングインデックスを0にしてV5へ移行し、CRCを再計算する。
- pendingが1以上: FRAMを初期化せず、保存系をDegradedとして「旧ファームでflushが必要」と明示する。
- 不明なformatVersion: 無条件初期化せず、エラーとして停止する。

未flushレコードを消すための自動初期化は禁止する。

### 7.3 CSV列

既存列の順番を維持し、`ValidFlags`の後ろへ次の列を追加する。

```text
BME690_Temp_C
BME690_RH_pct
BME690_Pressure_hPa
BME690_GasResistance_Ohm
BME690_GasValid
BME690_HeaterStable
BME690_GasIndex
BME690_StatusHex
```

出力例:

```text
...,0x0000061F,24.83,48.21,1008.42,182340,1,1,0,0xB0
```

無効時は次のように出力する。

```text
...,0x0000001F,nan,nan,nan,nan,0,0,0,0x00
```

現行`formatCsvLine()`は256 byteバッファを使用しているため、BME690列追加時は512 byteへ拡張し、`snprintf()`の戻り値が`buffer size`以上ならその行をSDへ書かず、切詰めエラーとして記録する。途中で切れたCSV行を正常データとして保存してはならない。

### 7.4 CSVスキーマ混在防止

ファームウェア更新日の`/log_YYYYMMDD.csv`が既に旧ヘッダーで存在する場合、同じファイルへ新しい列数の行を追記しない。

次のいずれかを実装し、本作業では前者を推奨する。

1. 新スキーマでは`/log_YYYYMMDD_v5.csv`を使用する。
2. 既存ファイルの先頭行がV5ヘッダーと完全一致するときだけ追記し、不一致なら`_v5`サフィックスの別ファイルを作る。

翌日ファイルの事前作成、Webからの現在ファイル保護、アーカイブ対象判定も新ファイル名で動作することを確認する。`log_`で始まり日付が同じ位置にあるため、現行アーカイブの日付抽出との互換性を維持できる。

### 7.5 旧DataLoggerの扱い

現行`main.cpp`の実運用保存経路は`StorageManager`であり、`DataLogger`は使用されていない。完了判定はFRAMを経由する`StorageManager`のV5 CSVを対象とする。

`DataLogger`を残す場合は、将来誤って旧スキーマを生成しないよう同じBME690列へ更新するか、レガシーであることをヘッダーへ明記する。実運用の保存経路を`DataLogger`へ切り替えない。

## 8. 変更対象ファイル

| ファイル | 作業内容 |
|---|---|
| `platformio.ini` | BME690 SensorAPI v1.1.0の版固定追加 |
| `include/core/sensor_types.h` | `SensorId`、`Bme690Data`追加 |
| `include/core/sensor_snapshot.h` | BME690スナップショット、状態追加 |
| `include/drivers/sensors/bme690_sensor.h` | 新規ドライバー宣言 |
| `src/drivers/sensors/bme690_sensor.cpp` | I2Cコールバック、初期化、非ブロッキング測定、診断 |
| `include/services/sensor_manager.h` | BME690メンバー追加 |
| `src/services/sensor_manager.cpp` | 初期化、更新、スナップショット反映 |
| `include/storage/storage_records.h` | V5レコード、valid flags、format version |
| `src/storage/storage_manager.cpp` | V4→V5移行、FRAM格納、CSVヘッダー、CSV行、バッファ長、スキーマ分離 |
| `src/storage/fram_storage.cpp` | エラーメッセージの実スキャン範囲`0x50 - 0x51`への訂正 |
| `README.md` | BME690、`0x76`、CSV保存内容の追記 |
| `test/test_bme690_logic/` | 状態・変換・CSV有効性判定のnativeテスト |

OLED関連ファイルは変更しない。

## 9. 実施手順

### Phase 0: 事前保全

- [ ] 作業前のコミットIDを記録する。
- [ ] SDカードを装着し、FRAM pendingを0までflushする。
- [ ] 現在のCSVを退避する。
- [ ] BME690未接続時のI2Cスキャン結果を保存する。
- [ ] 使用するBME690基板の型番、回路図、ADDR設定、プルアップ有無を記録する。

### Phase 1: 配線と単体疎通

- [ ] 電源OFFでBME690を配線する。
- [ ] SDO／ADDRを`0x76`へ固定する。
- [ ] I2Cスキャンで`0x76`の追加と既存アドレスの維持を確認する。
- [ ] チップID`0x61`、Variant ID`0x02`を確認する。
- [ ] 10分以上、温湿度・気圧・ガス抵抗・statusをシリアルへ出し、NACKやリセットがないことを確認する。

### Phase 2: ドライバー統合

- [ ] 公式SensorAPI v1.1.0を版固定する。
- [ ] `Bme690Sensor`とI2Cコールバックを追加する。
- [ ] 非ブロッキングforced-mode状態機械を実装する。
- [ ] 測定値の単位変換、有効性、status判定を実装する。
- [ ] BME690未接続時のフォールト分離と再試行を実装する。
- [ ] `SensorSnapshot::bme690`へ反映する。

### Phase 3: FRAM・CSV統合

- [ ] `SensorRecordV5`、V5 valid flags、format version 5を実装する。
- [ ] V4 pending保護付き移行を実装する。
- [ ] レコードサイズが128 byte以下であることを確認する。
- [ ] BME690専用CSV列を末尾へ追加する。
- [ ] CSV行バッファを512 byteへ拡張し、切詰め検出を実装する。
- [ ] 旧ヘッダーとV5行を同一ファイルへ混在させない。
- [ ] SD障害中はBME690値もFRAMへ残り、復旧後にCSVへflushされることを確認する。

### Phase 4: 回帰・連続試験

- [ ] 通常ビルドを行う。
- [ ] nativeテストを行う。
- [ ] BME690あり／なしの両方で起動する。
- [ ] PPG測定中に24時間連続運転する。
- [ ] SD抜去、再挿入、Wi-Fi閲覧、日付ローテーションを試験する。
- [ ] OLED表示が変更されていないことを確認する。
- [ ] WebからV5 CSVをダウンロードできることを確認する。

## 10. 試験項目

| No. | 試験 | 操作 | 合格条件 |
|---:|---|---|---|
| 1 | アドレス追加 | BME690接続前後でscan | 既存アドレスを維持し`0x76`だけ追加 |
| 2 | デバイス識別 | IDレジスタ読出し | chip ID=`0x61`、variant ID=`0x02` |
| 3 | CSV基本保存 | 1分動作 | 5秒周期でBME690列に行が追加される |
| 4 | 単位 | 気圧値を確認 | Paのままではなく概ね300～1100 hPa |
| 5 | ガス有効性 | 起動直後から観察 | statusに応じてGasValid／HeaterStableが正しく変化 |
| 6 | 欠損表現 | BME690を未接続で起動 | BME690列は`nan`／0、他の列は記録継続 |
| 7 | 一時NACK | 安全な試験方法で通信失敗を発生 | 他センサとPPGを停止させず再試行 |
| 8 | SDなし | SDを外して動作 | FRAM pendingが増え、既存バッファ能力内で保持 |
| 9 | SD復旧 | SDを戻してflush | BME690列を含む行がCRC正常でCSVへ反映 |
| 10 | CSV長 | GNSS fix等すべて有効にする | 行末`BME690_StatusHex`まで切れない |
| 11 | スキーマ | 旧CSVがある日に更新 | 旧ヘッダーへV5行を混在させない |
| 12 | Web | CSVを閲覧・ダウンロード | ファイル一覧、従来グラフ、ダウンロードが動作 |
| 13 | PPG回帰 | 指装着で連続測定 | FIFO overflow、I2C error、ジッタの有意増加なし |
| 14 | 長時間 | 24時間連続運転 | WDT reset、バス固着、CSV破損、異常な欠損なし |
| 15 | 電源回帰 | 10回再起動 | FRAM初期化や旧データ消失が起きない |

## 11. 合格基準

以下をすべて満たした場合に作業完了とする。

- BME690は`0x76`でのみ初期化され、chip IDとvariant IDが一致する。
- 既存の8アドレス構成に衝突や消失がない。
- BME690の温度、湿度、気圧、生ガス抵抗、状態がV5 CSVへ保存される。
- 有効でないガス抵抗を正常値として保存しない。
- CSV行が切り詰められず、ヘッダーとデータ列数が全行で一致する。
- 旧V4 CSVとV5行が同じファイルへ混在しない。
- 旧FRAM pendingがある状態でV5初期化によりデータを消失させない。
- BME690未接続・故障時にも既存機能が継続する。
- OLEDに変更がない。
- 24時間試験でI2C、PPG、FRAM、SD、WDTに新規の継続障害がない。

## 12. 初期運用上の注意

- BME690のヒーターにより内蔵温度は周囲温度より高くなることがある。SHT45との差を故障と即断しない。
- 生ガス抵抗の絶対値だけで空気質を判定しない。時間変化、温湿度、設置環境、GasValid、HeaterStableと一緒に評価する。
- ガスセンサ仕様は、新品センサを主に周囲空気中で6日以上動作させ、同じ履歴を持たせた条件を基準としている。導入直後の値は立上げデータとして扱い、少なくとも最初の6日間は基準化・傾向観察期間としてCSVを保持する。
- BME690とSGP41は異なるセンサ／アルゴリズムである。`BME690_GasResistance_Ohm`と`VOC_Index`／`NOx_Index`を数値の大小で直接比較しない。
- 将来BSECを採用する場合はBSEC stateをFRAMへ永続化し、保存周期、状態CRC、format migration、ライセンスを別作業書で定義する。

## 13. ロールバック

1. V5 CSVと試験ログを退避する。
2. BME690の電源を切り、配線を外す。
3. V4へ戻す前にV5 pendingをすべてSDへflushする。V5レコードをV4で読まない。
4. 旧ファームウェアへ戻す。
5. I2Cスキャンから`0x76`だけが消え、既存アドレスが維持されることを確認する。
6. 既存センサ、PPG、OLED、FRAM、SDの回帰試験を行う。

## 14. 作業結果記録欄

| 項目 | 記録 |
|---|---|
| 実施者 |  |
| 実施日時 |  |
| 開始コミット |  |
| 完了コミット |  |
| BME690基板型番 |  |
| 実アドレス |  |
| 追加前scan |  |
| 追加後scan |  |
| SensorAPI版 |  |
| `sizeof(SensorRecordV5)` |  |
| `sizeof(PersistentRecordV5)` |  |
| CSVファイル名 |  |
| 24時間試験開始／終了 |  |
| I2C error増分 |  |
| PPG FIFO overflow増分 |  |
| WDT／再起動回数 |  |
| 判定 | 合格 / 条件付合格 / 不合格 |
| 備考 |  |

## 15. 参照資料

- [Bosch Sensortec BME690 Datasheet, rev. 1.4](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme690-ds001.pdf)
- [Bosch Sensortec BME690 SensorAPI](https://github.com/boschsensortec/BME690_SensorAPI)
- [Bosch Sensortec BME690 SensorAPI forced-mode example](https://github.com/boschsensortec/BME690_SensorAPI/blob/master/examples/forced_mode/forced_mode.c)
- [Bosch Sensortec BME688 and BME690 Software](https://www.bosch-sensortec.com/en/software-tools/software/bme688-and-bme690-software)

