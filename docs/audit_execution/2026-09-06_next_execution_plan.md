# 監査問題の次回実行計画

日付: 2026-09-06  
対象: 監査中の全27件  
状態: 2026-09-06の調査を終了。次回はR01から再開する。  
原則: 調査結果を固定する前に設計を確定せず、設計判断を記録する前に本修正へ入らない。

## 現在地

- 再修正または追加調査が必要: P0-03、P0-06、P1-01、P1-02、P1-06。
- 実装済みで実機Gate待ち: 上記以外の22件。
- P0-06はWebUI初回接続、F5、ファイル「一覧更新」の3経路で再現し、発火routeを`GET /api/files`へ確定した。route内の停止段階は未確定。
- P0-03は旧FRAM 75件のflushにより約29.3秒メインloopが停止した。
- P1-01／P1-02はPPS周期異常108回、UTC不一致84回、最短周期1.777 msを確認した。
- P1-06は初回smokeのOLED lock timeout 9回に加え、Web試験画面でI²C lock timeout 284、communication error 0を確認した。
- P0-05は約117秒の短時間Gateを通過し、StorageTask最小stack残量は3,200 bytesだった。負荷・長時間Gateは未完了。

## 実行順序

`R 調査` → `D 修正方針の確定` → `M 実装` → `V 実機検証` → `C 監査close`

優先順は次のとおり。

1. P0-03 保存処理の長時間排他
2. P0-06 `/api/files`による`async_tcp` Watchdog再起動
3. P1-01／P1-02 PPS入力とClock反映
4. P1-06 I²C lock timeout増加
5. 実装済み22件の機能別Gate
6. 48時間連続試験と全27件のclose判定

P0-03とP0-06はStorageManagerのI/O・mutex設計を共有するため、別々に場当たり修正せず、R01/R02の結果をD01で統合する。

## Stage R: 調査

### R01 `/api/files`停止段階の特定

対象: P0-06、P2-04、P2-05、P2-06  
依存: なし  
変更範囲: 計測用ログのみ。本修正は行わない。

作業:

1. `/api/files`のroute開始、StorageManager lock要求、lock取得、SD open、走査完了、filter完了、sort完了、JSON生成完了、send要求へmonotonic時刻を付ける。
2. mutex待ち時間、保持時間、directory数、調査したentry数、採用file数、応答byte数、heap最小値を1 requestにつき1件の診断へ集約する。
3. 空SD、少数file SD、現在のSDで、初回接続・F5・一覧更新を各1回だけ実施する。原因特定済みの条件を不必要に繰り返さない。
4. Web資産＋状態API 5本だけの初期表示と、`/api/files`を加えた初期表示を比較する。

完了条件:

- 10秒のWatchdog期限を消費した段階が、mutex待ち、SD走査、CPU上の整形、応答送信のいずれかへ特定される。
- 証拠にfirmware hash、SD条件、file／directory件数、各段階の所要時間が含まれる。

### R02 StorageManager長時間排他の全経路調査

対象: P0-03、P0-01、P0-05、P1-03、P1-14、P2-02  
依存: R01と並行調査可能

作業:

1. `lock()`から`unlock()`までにSD／FRAM I/O、再open検証、format、rotation、Archive処理を行う全経路を列挙する。
2. `appendRecord`、`flushPendingToSd`、`forceFlush`、download、Archive、PPG session、file一覧の呼出元taskと優先度を記録する。
3. 旧record 75件の条件で、1 record当たりのwrite・flush・close・reopen・read-back時間、mutex最大保持時間、main loop最大停止時間を測る。
4. Wi-Fi ON/OFF、PPG IDLE/記録中の差を測り、queue詰まり、FRAM pending増加、PPG dropへの影響を確認する。

完了条件:

- 全ての無期限待機点と長時間mutex保持点が一覧化される。
- P0-03とP0-06を同じStorage実行モデルで直せる範囲と、別workerが必要な範囲が明確になる。

### R03 PPS異常edgeの発生源特定

対象: P1-01、P1-02、P0-04  
依存: なし

作業:

1. LC76GのPPS pinをオシロスコープまたはlogic analyzerで観測し、正規1 Hz pulseの幅・電圧と、1.777 ms等の異常edgeが物理pinにも存在するか確認する。
2. GPIO番号、pull設定、interrupt mode、他機能とのpin競合、配線、GND、電源条件を確認する。
3. ISR受信時刻、interval、採否、NMEA UTC anchor、Clock反映時刻を短いring診断へ保存する。
4. GNSS fix喪失、PPSのみ喪失、NMEAのみ喪失を分け、Holdover遷移条件を再現する。

完了条件:

- 異常edgeが電気的入力、GPIO設定、ISR処理、時刻対応付けのどこで生じるか確定する。
- Clockへ渡してよいPPSの検証条件が数値で決まる。

### R04 I²C lock timeout 284件の内訳特定

対象: P1-06、P1-16、P2-03  
依存: R02の長時間Storage経路を考慮する

作業:

1. 再起動前に`/api/i2c`を取得し、device／operation別のlock timeoutを記録する。`/api/files`は呼ばない。
2. 60秒単位でOLED、FRAM append／flush、SHT45 heater、SCD41、SGP41、BMP581、BME690、MAX30102の増分を比較する。
3. lock所有者、取得時刻、保持時間、timeoutした要求元をrate-limitしたringへ追加し、最長保持者を特定する。
4. P0-03解消前後で同一条件を比較し、Storage長時間排他の派生か独立したI²C問題かを判定する。

完了条件:

- 284件の主なdevice／operationとlock所有者が特定される。
- 電気的通信error 0との違いを保ったまま、競合・優先度逆転・過短timeoutのどれかへ分類される。

### R05 実装済み22件の証拠不足確認

対象: P0-01、P0-04、P0-05、P1-03～P1-05、P1-07～P1-16、P2-02～P2-07から再修正5件を除く項目  
依存: R01～R04と独立

作業:

1. 各監査IDについて、コード根拠、自動試験、必要な実機条件、未取得証拠を1行で対応付ける。
2. 実機Gateを通すだけでcloseできる項目と、追加実装が必要な項目を再確認する。
3. 破壊試験が必要な項目には予備SD、外部CO₂基準器、logic analyzer等の前提を付ける。

完了条件:

- 27件全てに次の作業とclose条件があり、未割当の監査IDが0件になる。

## Stage D: 修正方針の確定

### D01 Storage実行モデル

入力: R01、R02  
対象: P0-03、P0-06を中心にStorage関連項目

第一候補:

- 遅いSD処理を優先度付きStorage workerへ集約し、main loopとAsyncTCP callbackは要求をqueueへ入れるだけにする。
- 環境record／PPG継続性を高優先、flushを中優先、file index／Archiveを低優先とする。
- flushは1回の件数または時間budgetで中断し、record境界ごとにdata-integrity contractを完了してから次へ進む。
- Web callbackでは`portMAX_DELAY`を使わず、cache取得またはjob受付だけを行う。

判断項目:

1. StorageTaskをSD唯一所有者へ拡張するか、FileIndex workerへ短時間lockを許すか。
2. PPG sessionの既存ringとwrite経路をStorage queueへ統合する範囲。
3. queue満杯時の優先順位、再試行、FRAM退避、利用者へのエラー契約。
4. 実測SD性能に基づくflush件数budgetと最大連続実行時間。

方針確定条件:

- データ原子性、PPG 100 Hz、Web応答性、既存Archiveを同時に満たすtask ownership図ができる。
- main loopとAsyncTCPに無期限待機が残らない。

### D02 Web file index/API/UI契約

入力: R01、D01  
対象: P0-06、P1-12、P1-13、P2-04、P2-05、P2-06

確定する仕様:

1. 初期画面は状態APIだけを読み、ファイルtab初回表示時にfile indexを要求する。
2. 一覧更新は走査を同期実行せずrefresh jobを開始する。
3. cache未生成・更新中は`202 Accepted`、状態、進捗、再試行間隔を返す。完了後はcache済みpageを返す。
4. sort／filter／pageはcache上で行い、1 responseの上限を固定する。
5. 現在CSV、active PPG、Archive対象の保護情報は応答直前に短時間snapshotとして合成する。
6. request中断、AP OFF、SDなし、走査失敗、cache世代交代時のUI表示を定義する。

方針確定条件:

- AsyncTCP callbackにSD走査、Archive、force flush、無期限mutex待ちが存在しないAPI契約になる。

### D03 PPS採用・Clock状態機械

入力: R03  
対象: P1-01、P1-02、P0-04

確定する仕様:

1. ISRはedge時刻の記録だけを行い、Clockへ直接disciplineを報告しない。
2. 許容周期、glitch除外幅、連続正常edge数、NMEA UTCとの対応窓を定義する。
3. 検証済みPPSだけで`PPS_AgeMs`とdisciplineを更新する。
4. 異常継続時はHoldoverへ遷移し、UTC単調性を保ちながら`disciplined=0`を保存する。
5. 物理波形に問題がある場合だけRC／Schmitt等のhardware対策を別項目にする。

### D04 I²C transaction規約

入力: R02、R04  
対象: P1-06、P1-16、P2-03

確定する仕様:

1. mutex保持中はWire transactionだけを行い、sensor内部待機、delay、format、SD処理を置かない。
2. device別の最大待ち時間、再試行、stale遷移、再初期化、優先度を定義する。
3. timeout時に要求元とlock所有者を残し、累積値だけでなく増加率を診断できるようにする。
4. OLED等の表示更新はsensor／FRAMを飢餓状態にしない頻度へ制限する。

### D05 Release Gateと証拠形式

入力: R05、D01～D04  
対象: 全27件

確定する仕様:

- firmware hash、試験条件、開始／終了時刻、合否、CSV範囲、event、serial抜粋、残余リスクを監査IDごとに記録する。
- P0/P1再修正後は該当する短時間Gateを先に行い、通過するまで長時間・破壊試験へ進まない。
- 失敗時は同じGateから再開し、別項目のcloseを巻き戻さない。

## Stage M: 実装

### M01 診断計測の実装と回収

対象: R01～R04  
内容: bounded ring、route段階時間、mutex wait／hold、I²C所有者、PPS edge採否を実装する。通常ログを大量保存せず、異常前後と集計だけを残す。  
完了条件: native試験・firmware buildが通り、各Rタスクの必要証拠を実機で回収できる。

### M02 Storage queue／flush budget再実装

対象: P0-03、P0-01、P0-05、P1-03、P1-14、P2-02  
依存: D01  
内容: append要求の非同期化、優先度queue、flushの件数／時間budget、record境界でのlock解放、queue満杯時のFRAM保護を実装する。既存のwrite・flush・close・reopen・read-back後だけtail更新する契約は維持する。  
完了条件: 75件backlogでもmain loop停止1秒未満、説明不能なSequence欠落0、StorageTask stack残量2,048 bytes以上。

### M03 Web file index非同期化

対象: P0-06、P1-12、P1-13、P2-04、P2-05、P2-06  
依存: D01、D02、M02のStorage API  
内容: file index job、cache世代、進捗API、page応答、files tab遅延load、poll、失敗表示を実装する。同期SD全件走査をAsyncTCP callbackから除去する。  
完了条件: 初回接続、F5、状態更新、一覧更新を各10回行ってresetなし。HTTP callback最大時間が決定した上限内で、一覧・検索・選択・downloadが動作する。

### M04 PPS検証とClock反映修正

対象: P1-01、P1-02、P0-04  
依存: D03  
内容: ISR capture、task側interval検証、NMEA対応付け、検証済みPPSのみのClock反映、Holdover遷移、診断counterを実装する。  
完了条件: glitch edgeでdisciplineとPPS ageが更新されず、正常1 Hz復帰後だけ所定連続数で再disciplineする。

### M05 I²C公平性・診断修正

対象: P1-06、P1-16、P2-03  
依存: D04、M02  
内容: R04で特定した長時間保持を分割し、所有者・待ち時間診断、必要な優先度・再試行・表示頻度制限を実装する。  
完了条件: 全device同時稼働30分で説明不能なlock timeout増加、stale固定値、FRAM未回収、PPG FIFO欠落がない。

### M06 回帰試験と生成物固定

対象: 全コード変更  
依存: M02～M05

1. native全suite、全PlatformIO environment、Web JavaScript、HTML parser、gzip再生成一致を実行する。
2. CSV v7 71列、FRAM v6 128 byte、旧v5 pending非破壊移行を再確認する。
3. RAM、Flash、task stack high-water、firmware SHA-256を記録する。
4. 失敗があれば該当Mタスクへ戻り、実機Gateへ進まない。

## Stage V: 実機Gate

### V01 30分基盤smoke

対象: P0-03、P0-05、P1-01、P1-02、P1-06、P1-16、P2-02、P2-07  
条件: resetなし、loop長時間停止なし、CSV列・時刻・鮮度・I²C値が整合する。

### V02 Web／Archive

対象: P0-06、P1-12、P1-13、P2-04、P2-05、P2-06  
条件: 初回接続・F5・各更新、認証、CSRF、path拒否、一覧、複数操作、ZIP、cache更新を実SDで通す。

### V03 時刻・位置・AMeDAS・高度

対象: P0-04、P1-01、P1-02、P1-07、P1-08、P1-09、P2-07  
条件: PPS喪失／復帰、NTP fallback、GNSS位置、AMeDAS品質・鮮度、既知標高13.6 mでの精度を確認する。

### V04 I²C・SCD41・SGP41

対象: P1-04、P1-05、P1-06、P1-15、P1-16、P2-03  
条件: device切離し、stale無効化、段階復旧、backoff、FRC／Reset安全条件を確認する。

### V05 FRAM・SD・電源断

対象: P0-01、P0-03、P0-05、P1-03、P1-14、P2-02、P2-03  
条件: 予備SDで抜去・再挿入・満杯・checkpoint破損・commit境界電源断を試し、未保存消費と説明不能な欠落を0にする。

### V06 PPG

対象: P1-10、P1-11、P2-03、P2-07  
条件: 指なし、正常、低灌流、体動、二峰性、長時間RAW、partial復旧、PC readerを確認する。

### V07 48時間連続試験

対象: 全27件  
条件: reset、SCD41 recovery storm、説明不能なI²C／PPG／FRAM drop、CSV不整合、時刻逆行、高度段差がない。

## Stage C: Close判定

1. 自動試験だけではcloseしない。対応する実機Gateと証拠が揃った監査IDだけをcloseする。
2. 27件全てにfirmware hash、試験日、合否、証拠、残余リスクを紐付ける。
3. P2-01はActionButtonへ変更済みで解決扱いを維持し、P0-06のAP消失とは関連付けない。
4. P0-02はFRAM v4利用および未flush v3なしという現行条件を維持し、再発証拠がない限り再オープンしない。
5. 全27件がclose条件を満たした時点でRelease Gate完了とする。

## 次回の開始点

次回はコード変更前にR01とR02を実施する。最初の成果物は次の2点とする。

1. `/api/files`各段階の所要時間表と停止段階の確定
2. StorageManagerのtask／lock／I/O ownership図と最大待ち・保持時間一覧

この2点をレビューしてD01/D02を確定するまで、P0-03／P0-06の本修正には着手しない。
