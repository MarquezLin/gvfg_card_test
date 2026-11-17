# GVFG 擷取卡測試

在待測的 Windows x64 電腦解壓完整資料夾並開啟 `bin/GVFG_CardTest.exe`。待測電腦需已安裝 GVFG 驅動程式。程式直接用隨附的 GVFG runtime 讀取擷取卡資料。

1. 勾選要測試的卡和各卡 CH0／CH1 的測項。只勾卡片時執行擷取卡偵測。選取一張卡後可將它的勾選套用到其他卡。
2. 在「測項設定」調整共用的解析度、FPS、PCM 格式等門檻。程式依序測試各卡及通道。

「8-bit 擷取（YUY2）」與「10-bit 擷取（Y210）」可在同一通道同時勾選。程式會分別設定 SDK 的擷取輸出格式，每種格式各自檢查影格格式、位元深度和最低 FPS。勾選音訊擷取時，也會在各格式下分別判定。通道開啟、鎖定和輸入格式等共用測項只列一次。這兩項驗證 SDK 的輸出影格。若要比對訊號狀態回報的位元深度，可另勾「解析度與輸入格式」。
3. 按「試跑 · 不關機」執行單輪。每輪保存 CSV，記錄包含裝置介面路徑、通道、測項、結果及實測值或錯誤。
4. 要循環冷開機時，在「Relay Agent 連線設定」確認控制端網址並測試連線（預設 http://192.168.50.20:8080），設定總輪數，再按「開始冷開機測試」。第 1 輪在當前開機執行，其後每兩輪之間做一次斷電復電。任一測項失敗時，會依「測項失敗仍繼續下一輪」的設定決定是否續跑。

Relay Agent 應在另一台維持供電的電腦執行。沿用既有 Agent 的 RS485 與待測端通道設定，確認 BIOS 來電自動開機及 Windows 自動登入已設定。每次關機前會保存報告和續跑進度。復電登入後，程式依保存的輪次向 Agent 確認斷電指令已送出，再執行下一輪。Agent 重啟或連線中斷可能使排程狀態無法確認，畫面會顯示待確認的狀態。

測試以 Windows 裝置介面路徑辨識多張卡。同一台電腦上的裝置介面路徑若因插槽或驅動安裝變更，請重新掃描並選卡。報告保存在 EXE 同一層的 `reports\trial` 和 `reports\cold`，匯出預設放在 `reports\exports`。新版啟動時會將原先 AppData 裡的舊報告移入對應分類。請將程式解壓到可寫入的資料夾。

主畫面的「測試報告」可查看歷史測試及各輪結果、匯出整場 HTML 或單輪 CSV，也可開啟報告所在資料夾。刪除會將整場報告移至 Windows 資源回收筒。等待冷開機續跑的紀錄會受到保護。

## 開發

在 Visual Studio x64 Developer Command Prompt，從 `gvfg_card_test` 目錄執行：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
cmake --build build
cmake --install build --prefix "%CD%\build\portable"
```

`card_test_window.ui` 可在 Qt Designer 編輯。`card_checks.cpp` 的測項清單提供勾選項目與報表標籤。新增測項時加入穩定的測項 ID、SDK 判定邏輯，以及需要的設定欄位。`session.cpp` 負責 Windows 開機識別、排程及每輪 CSV。

`GVFG_CardTest.exe --self-test` 測試多卡離線結果、報告和本機模擬 Relay Agent 的通訊。`--ui-self-test` 透過主畫面執行一次離線試跑並驗證結果和 CSV。兩種自測均使用暫存資料。





## 乾淨編譯及打包

雙擊 `package_release.bat`，或在命令列執行 `package_release.bat /nopause`。
腳本會使用全新的暫存目錄進行 Release 編譯、部署 Qt、執行自測，並輸出
`release\GVFG_CardTest-Windows-x64.zip`。結束時會清除本次編譯與打包暫存。
可用環境變數 `QT_ROOT`、`CMAKE_EXE`、`NINJA_EXE`、`VS_ROOT` 指定工具位置。

將相容的 x64 DLL 放入 `runtime\gvfg.dll`，下次打包會採用該檔。
請使用與工作區 SDK 標頭相容的 x64 DLL。打包前會使用置放的 DLL 執行自測。
程式保留 Windows 裝置介面路徑，開啟通道前會重新對照 SDK 的裝置索引。
留空時會直接從目前 SDK 原始碼編譯 DLL。
若要從新版 SDK 原始碼更新 DLL，更新工作區 SDK 後直接執行 BAT 即可。
執行 `package_release.bat /source-runtime`，讓本次打包使用原始碼編譯的 DLL，並保留原本的置放檔。可與 `/nopause` 一起使用。

試跑及正式測試的擷取會在獨立子程序執行。子程序異常結束時，主畫面和本輪報告會顯示結束代碼與最後收到的步驟，詳細紀錄可在場次資料夾的 `round_*.log` 查看。
