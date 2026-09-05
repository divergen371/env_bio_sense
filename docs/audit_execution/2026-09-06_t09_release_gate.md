# T09 統合試験・Release Gate実行手順

日付: 2026-09-06  
対象: 監査中の全25件  
状態: 実機試験待ち

## 方針

コード修正済みを監査closeへ変えるための実機Gateである。最初に非破壊のsmoke testを行い、その後に交換可能なSDカードで障害を注入し、最後に48時間連続試験を行う。シリアル全文の常時保存は不要とし、CSV v7、compact event、I²C内訳、PPG session、異常前後だけのシリアル抜粋を証拠にする。

## 試験前の安全条件

- SD抜去、低速化、破損、電源断試験には空の予備SDカードを使う。本番データ入りSDでは実施しない。
- firmware書込み前に現在のSD、FRAM、設定値を変更しない状態で保全する。既存ログは削除しない。
- `src/config/secrets.h`のAP／Web資格情報を確認する。値そのものは試験記録やGitへ貼らない。
- SCD41 FRCは信頼できる外部CO₂基準器と十分な安定時間がある場合だけ実施する。Factory Resetは校正・設定が消える前提で最後に単独実施する。
- 電源断はUSB電源を切る方法で行い、I²CやSPI端子を短絡させない。通信障害は対象moduleの切離し、または試験用switchで注入する。

## Gate 0: firmwareと自動試験の固定

1. `seeed_xiao_esp32s3`をbuildし、RAM／Flash使用量とfirmware hashを記録する。
2. native 11 suite、97件が全PASSすることを記録する。
3. `web/index.html`、`web/app.css`、`web/app.js`から生成したheaderを再生成し、差分が出ないことを確認する。
4. 試験firmwareを書込み、起動時に64 KiB FRAM、SD、全sensorが期待状態になることを確認する。

現時点の基準はRAM 98,264 / 327,680 bytes、Flash 1,314,441 / 3,342,336 bytes、Web生成header SHA-256 `24d52b3717190578a07a7341fde14510974766f786c7156750e910fb07a5b55e`である。

## Gate 1: 30分smoke test

1. 通常状態で30分記録する。Web APはOFFとする。
2. CSV v7が71列固定で、`Sequence`と`SampleMonotonicUs`が単調増加し、行切れがないことを確認する。
3. 欠測値が空欄で、そのsensorの`Valid`、`AgeMs`、`State`、`Error`と矛盾しないことを確認する。
4. `PPS_AgeMs`が全行0ではなく、GNSS/PPSの実状態に応じて変化することを確認する。
5. `SRAW_VOC`、`SRAW_NOX`、補償温湿度、I²C累積値、FRAM drop値が保存されることを確認する。NOx指数1の下限張り付き自体は不合格にしない。

合格対象: P0-03、P1-02、P1-16、P2-02、P2-07

## Gate 2: 時刻・位置・AMeDAS・高度

1. 屋外でGNSS Liveが成立し、3連続の良好fix後だけ現在地として採用されることを確認する。
2. GNSS/PPSを喪失させ、ClockがHoldover、`disciplined=0`へ移る一方、UTCが単調継続することを確認する。再受信後はGNSSへ戻ることを確認する。
3. GNSS時刻を得られない起動条件でNTP fallbackを確認し、1970年付近や1000分の1の時刻にならないことを確認する。
4. AMeDASが品質0、観測15分以内、3地点以上でValidとなり、観測所ID・距離・観測時刻・採否が`amedas_pressure_v1.csv`とAPIで一致することを確認する。
5. AMeDAS更新停止を作り、15分超でLastKnown、60分超でInvalidへ移ることを確認する。
6. 既知標高13.6 mで校正し、受入統計とFRAM読戻しを確認する。別時間帯に30分測定し、raw高度平均13.6 ±3.0 m、標準偏差とP0更新に対応しない5秒の大段差がないことを確認する。
7. 高度計算温度sourceが有効なSHT45値を優先し、SHT45無効時だけBMP581温度になることをAPI／CSVで確認する。

合格対象: P0-04、P1-01、P1-02、P1-07、P1-08、P1-09、P2-07

## Gate 3: I²C・鮮度・SCD41復旧

1. FRAM、SHT45、SCD41、SGP41、BMP581、BME690、MAX30102を同時稼働し、SHT45 heater実行中にも他deviceが長時間停止しないことを確認する。
2. device単位の切離しを順番に行い、古い値がvalidのまま固定されず、所定ageで無効化され、`State`／`Error`／連続error数が変化することを確認する。
3. `/api/i2c`とcompact eventでlock timeoutと通信errorをdevice・operation別に切り分ける。異常前後だけシリアル抜粋を保存する。
4. SCD41停止時に古いCO₂がvalid保存されず、stop/startからreinitへ段階昇格し、30／120／300秒backoffを守ることを確認する。
5. SCD41復旧後は3連続正常sampleまでStabilizingで隔離され、その後だけ`CO2_Valid=1`になることを確認する。
6. 外部基準器がある場合、3分以上の安定測定と新鮮な現地気圧でFRCを行い、補正値、再起動、eventを照合する。条件不足時に拒否されることも確認する。
7. Factory Resetを行う場合は最後に実施し、確認token、設定read-back、測定再開を確認する。

合格対象: P1-04、P1-05、P1-06、P1-15、P1-16、P2-03

## Gate 4: FRAM・SD・電源断

1. 予備SDで通常flush、SDなし起動、記録中抜去、再挿入、書込み失敗を試す。FRAM tailはSD行のflush／close／再open／内容照合前に進んではならない。
2. SD transactionの開始、SD append、SD検証、FRAM consume、checkpoint更新の各境界で各3回以上電源を切る。再起動後に欠落と重複がなく、orphan行は同一sequenceとして回収されることを確認する。
3. FRAM checkpoint片側破損、最新側破損、未知version、非blank破損を注入し、旧正常copyの採用またはread-only移行を確認する。無条件初期化は禁止する。
4. Wi-Fi APまたはSD停止を40分超継続し、FRAM満杯時に既存tailを上書きせず、新規drop数とeventが増えることを確認する。復旧後は古い順にflushする。
5. 構造破損slotが二度読み一致後にquarantineへ保存・再読照合されてからだけskipされることを確認する。
6. 最終CSVの全`Sequence`を走査し、欠落・重複が0件、または全件が`FRAM_DroppedRecords`と永続eventで説明できることを確認する。

合格対象: P0-01、P0-03、P1-03、P1-14、P2-02、P2-03

## Gate 5: PPG計測・RAW session

1. 指なし、装着直後、正常波形、低灌流、体動、二峰性波形を測り、PI 0.2%未満やstale時にHR／SpO2がvalidにならないことを確認する。
2. PPG sessionを確定し、`raw.ppg`、`environment.csv`、`metadata.json`が同じdirectoryへ揃い、metadataが最後のcommit markerになることを確認する。
3. `tools/ppg1_reader.py`で100 Hz時刻、logical index、block／sample件数、CRC、footer、metadata、環境行数を検証する。
4. 連続記録を想定時間の2倍以上行い、drop／FIFO overflowが0または実欠落と一致し、最大SD write時間がring保持時間を超えないことを確認する。
5. RAW header、block、payload、footer、環境CSV、各renameの17地点以上で各3回電源断し、partialをcompleteとして公開せず、正常prefixを別ファイルへ復旧し、元partialを保持することを確認する。
6. PSRAM確保失敗時の内部RAM fallback、MAX30102通信停止後の再初期化を確認する。

合格対象: P1-10、P1-11、P2-03、P2-07

## Gate 6: Web・Archive

1. 未認証UI／API／downloadが401、Digest認証後だけ成功することを確認する。変更POSTはCSRF tokenなし／誤tokenで403となることを確認する。
2. traversal、encoded separator、backslash、未知root、tmp、許可外拡張子を送信し、全て拒否されSD内容が変わらないことを確認する。
3. PCとスマートフォンで状態、再帰一覧、検索、種類、sort、page、ページ跨ぎ選択、1件取得、複数ZIP、一括削除、CSV graphを確認する。
4. 現在CSVとactive PPGが保護され、現在CSVの取得後も行境界・71列が正常で、PPG dropが増えないことを確認する。
5. 1件、64件、種類混在のZIPを作り、全entryをPCで展開して元ファイルとbyte一致することを確認する。
6. ZIP write、finalize、rename、最終検証中にSD抜去・低速化・電源断を行い、未検証ZIPを公開せず元ファイルを削除しないことを確認する。
7. 成功／失敗から5秒後にIdleへ戻り、再実行できることを確認する。
8. Internetなし・cold cacheでUIが表示され、firmware更新後に旧UIが残らないことを確認する。

合格対象: P1-12、P1-13、P2-04、P2-05、P2-06

## Gate 7: 48時間連続試験

1. 全device、GNSS、通常SD flushを有効にして48時間連続記録する。
2. SCD41 recovery stormが再発せず、CO₂固定区間がある場合も`CO2_Valid`、age、state、errorで妥当性を説明できることを確認する。
3. I²C error、FRAM drop、PPG drop、FIFO overflow、CSV列不整合、説明不能なSequence欠落、時刻逆行がないことを確認する。
4. AMeDAS P0更新と高度の変化を時刻で突合し、説明不能な5秒段差がないことを確認する。
5. 48時間終了後にcompact event、I²C JSON、CSV v7、AMeDAS履歴、校正履歴、PPG metadataを一式保全する。

合格対象: 全25件。特にP1-15のclose条件。

## Close判定

- 自動試験PASSだけでは監査項目をcloseしない。対応する実機Gateと証拠が揃った項目だけを「解決済み」へ変更する。
- 不合格時は症状、開始時刻、直前の正常時刻、関連event、I²C内訳、CSV範囲、再現回数を記録し、コード修正後にそのGateから再開する。
- 最終的に25件すべてへ、firmware hash、試験日、合否、証拠ファイル、残余リスクを紐付ける。
