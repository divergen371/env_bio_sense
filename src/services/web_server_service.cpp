#include "services/web_server_service.h"
#include "services/logger.h"
#include <FS.h>
#include <SPIFFS.h>
#include "hal/clock.h"
#include <ArduinoJson.h>
#include <SD.h>
#include "services/weather_service.h"
#include "services/sensor_manager.h"

extern services::WeatherService weatherService;
extern services::SensorManager sensorManager;

namespace services {

const char* htmlContent = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Env Bio Sense - Files</title>
  <style>
    body { font-family: sans-serif; margin: 20px; background: #f4f4f9; }
    h1 { color: #333; }
    ul { list-style: none; padding: 0; }
    li { background: #fff; margin: 10px 0; padding: 15px; border-radius: 5px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); display: flex; justify-content: space-between; align-items: center; }
    a { text-decoration: none; color: #007bff; font-weight: bold; }
    a:hover { text-decoration: underline; }
    .status { margin-bottom: 20px; padding: 10px; background: #e0f7fa; border-radius: 5px; }
    .modal { display: none; position: fixed; z-index: 1000; left: 0; top: 0; width: 100%; height: 100%; overflow: auto; background-color: rgba(0,0,0,0.8); }
    .modal-content { background-color: #fefefe; margin: 2% auto; padding: 20px; border: 1px solid #888; width: 95%; max-width: 1200px; height: 85vh; border-radius: 8px; position: relative; display: flex; flex-direction: column; }
    .close { color: #aaa; align-self: flex-end; font-size: 28px; font-weight: bold; cursor: pointer; margin-top: -10px; }
    .close:hover { color: black; }
    .chart-container { position: relative; flex-grow: 1; width: 100%; min-height: 0; }
    .link-name { color: #007bff; text-decoration: underline; cursor: pointer; font-weight: bold; }
    .link-name:hover { color: #0056b3; }
  </style>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/hammerjs@2.0.8"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.0.1/dist/chartjs-plugin-zoom.min.js"></script>
</head>
<body>
  <h1>Data Logs</h1>
  
  <div id="capacityAlert" style="margin-bottom: 15px; padding: 10px; border-radius: 5px; font-weight: bold; display: none;">
    FRAM Buffer: <span id="capacityText"></span>
  </div>

  <div id="status" class="status">Connecting...</div>
  
  <div style="margin-bottom: 15px; padding: 10px; background-color: #f1f8ff; border: 1px solid #c8e1ff; border-radius: 4px; font-size: 14px;">
    <strong>Altitude Calibration:</strong> 
    <span id="sealevelStatus">Waiting for connection...</span>
  </div>

  <div id="scd41RecBadge" style="display: none; margin-bottom: 15px; padding: 10px; background-color: #f8d7da; border: 1px solid #f5c6cb; border-radius: 4px; font-size: 14px; color: #721c24; font-weight: bold;">
    ⚠️ SCD41 Manual Calibration is recommended! (Last calibration > 7 days ago)
  </div>

  <div style="margin-bottom: 15px; padding: 10px; background-color: #fff3cd; border: 1px solid #ffeeba; border-radius: 4px; font-size: 14px;">
    <strong>SCD41 Manual Calibration:</strong><br>
    <small>Expose sensor to fresh air (>3 mins) before calibrating.</small><br>
    Target CO2 (ppm): <input type="number" id="scd41Target" value="400" style="width: 80px; margin-top: 5px;">
    <button onclick="calibrateSCD41()" style="padding: 4px 8px; font-size: 12px; cursor: pointer;">Calibrate</button>
    <span id="scd41CalibStatus" style="margin-left: 10px;"></span>
  </div>
  
  <div style="margin-bottom: 15px;">
    <button onclick="flushAndReload()" style="padding: 10px 15px; font-size: 16px; font-weight: bold; cursor: pointer; border-radius: 5px; border: none; background-color: #28a745; color: white; box-shadow: 0 2px 4px rgba(0,0,0,0.2);">
      🔄 Refresh & Flush
    </button>
  </div>
  <ul id="file-list"></ul>

  <div id="chartModal" class="modal">
    <div class="modal-content">
      <span class="close" onclick="closeChart()">&times;</span>
      <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; margin-top: -10px;">
        <h2 id="chartTitle" style="margin: 0;">Graph</h2>
        <div>
          <button id="resetZoomBtn" onclick="if(currentChart) currentChart.resetZoom()" style="padding: 5px 15px; font-size: 14px; cursor: pointer; border-radius: 4px; border: 1px solid #007bff; background-color: transparent; color: #007bff; margin-right: 10px;">
            🔍 Reset Zoom
          </button>
          <button id="reloadChartBtn" style="padding: 5px 15px; font-size: 14px; cursor: pointer; border-radius: 4px; border: none; background-color: #007bff; color: white; display: none;">
            🔄 Reload Data
          </button>
        </div>
      </div>
      <div class="chart-container">
        <canvas id="myChart"></canvas>
      </div>
    </div>
  </div>

  <script>
    async function syncTime() {
      const now = Math.floor(Date.now() / 1000);
      try {
        const response = await fetch('/api/time', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ epoch: now })
        });
        if (response.ok) {
          document.getElementById('status').innerText = 'システム時刻同期完了 / Time Synchronized.';
        }
      } catch (err) {
        document.getElementById('status').innerText = '時刻同期に失敗しました / Time Sync Failed.';
      }
    }

    async function calibrateSCD41() {
      const targetPpm = document.getElementById('scd41Target').value;
      const statusSpan = document.getElementById('scd41CalibStatus');
      statusSpan.innerText = 'Calibrating... (takes ~500ms)';
      statusSpan.style.color = '#856404';
      
      try {
        const response = await fetch('/api/scd41/calibrate', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ target_ppm: parseInt(targetPpm) })
        });
        const data = await response.json();
        if (response.ok) {
          statusSpan.innerText = 'Success! Correction: 0x' + data.correction.toString(16).toUpperCase();
          statusSpan.style.color = 'green';
          document.getElementById('scd41RecBadge').style.display = 'none';
        } else {
          statusSpan.innerText = 'Failed: ' + (data.error || 'Unknown error');
          statusSpan.style.color = 'red';
        }
      } catch (err) {
        statusSpan.innerText = 'Network error';
        statusSpan.style.color = 'red';
      }
    }

    function formatBytes(bytes) {
      if (bytes === 0) return '0 B';
      const k = 1024;
      const sizes = ['B', 'KB', 'MB', 'GB'];
      const i = Math.floor(Math.log(bytes) / Math.log(k));
      return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    async function loadFiles() {
      try {
        const response = await fetch('/api/files');
        const data = await response.json();
        const list = document.getElementById('file-list');
        list.innerHTML = '';
        data.files.forEach(f => {
          const li = document.createElement('li');
          
          const nameSpan = document.createElement('span');
          if (f.size > 0 && f.name.endsWith('.csv')) {
            nameSpan.innerHTML = `<span class="link-name" onclick="openChart('${f.name}')">${f.name}</span> (${formatBytes(f.size)})`;
          } else {
            nameSpan.innerText = f.name + ' (' + formatBytes(f.size) + ')';
          }
          
          if (f.isCurrent) {
            nameSpan.innerHTML += ' <span style="color:#d32f2f; font-weight:bold;">[Recording...]</span>';
          }
          
          const actionsDiv = document.createElement('div');
          
          const dlLink = document.createElement('a');
          dlLink.href = '/download?file=' + encodeURIComponent(f.name);
          dlLink.innerText = 'Download';
          dlLink.download = f.name;
          actionsDiv.appendChild(dlLink);

          if (!f.isCurrent) {
            const delBtn = document.createElement('button');
            delBtn.innerText = 'Delete';
            delBtn.style.marginLeft = '15px';
            delBtn.style.color = '#fff';
            delBtn.style.backgroundColor = '#dc3545';
            delBtn.style.border = 'none';
            delBtn.style.padding = '5px 10px';
            delBtn.style.borderRadius = '3px';
            delBtn.style.cursor = 'pointer';
            delBtn.onclick = async () => {
              if (confirm(f.name + ' を完全に削除しますか？ / Delete this file?')) {
                try {
                  const res = await fetch('/api/delete?file=' + encodeURIComponent(f.name), { method: 'DELETE' });
                  if (res.ok) {
                    await loadFiles();
                  } else {
                    alert('削除に失敗しました / Failed to delete');
                  }
                } catch (e) {
                  alert('エラー / Error');
                }
              }
            };
            actionsDiv.appendChild(delBtn);
          }

          li.appendChild(nameSpan);
          li.appendChild(actionsDiv);
          list.appendChild(li);
        });
      } catch (err) {
        document.getElementById('status').innerText += ' ファイル取得エラー / Failed to load files.';
      }
    }

    let currentChart = null;
    let activeFileName = '';

    async function fetchStatus() {
      try {
        const response = await fetch('/api/status');
        const data = await response.json();
        const alertDiv = document.getElementById('capacityAlert');
        const textSpan = document.getElementById('capacityText');
        
        const pct = Math.round((data.pending / data.max) * 100);
        
        alertDiv.style.display = 'block';
        if (pct >= 80) {
          alertDiv.style.backgroundColor = '#ffebee';
          alertDiv.style.color = '#c62828';
          alertDiv.style.border = '1px solid #c62828';
          textSpan.innerText = `⚠️ FRAM Buffer is almost full: ${data.pending} / ${data.max} (${pct}%) - Please click "Refresh & Flush" soon!`;
        } else {
          alertDiv.style.backgroundColor = '#e8f5e9';
          alertDiv.style.color = '#2e7d32';
          alertDiv.style.border = '1px solid #2e7d32';
          textSpan.innerText = `${data.pending} / ${data.max} (${pct}%)`;
        }
        
        const scdBadge = document.getElementById('scd41RecBadge');
        if (data.scd41CalibRecommended) {
          scdBadge.style.display = 'block';
        } else {
          scdBadge.style.display = 'none';
        }
      } catch (e) {
        console.error("Status fetch failed", e);
      }
    }

    async function flushAndReload() {
      const btn = document.querySelector('button[onclick="flushAndReload()"]');
      const originalText = btn.innerText;
      btn.innerText = '⏳ Flushing...';
      try {
        await fetch('/api/flush', { method: 'POST' });
        await loadFiles();
        await fetchStatus();
      } catch(e) {
        alert('フラッシュに失敗しました / Flush failed');
      }
      btn.innerText = originalText;
    }

    async function openChart(fileName) {
      activeFileName = fileName;
      document.getElementById('chartTitle').innerText = 'Loading ' + fileName + '...';
      document.getElementById('chartModal').style.display = 'flex';
      
      const reloadBtn = document.getElementById('reloadChartBtn');
      reloadBtn.style.display = 'block';
      reloadBtn.onclick = async () => {
        reloadBtn.innerText = '⏳ Reloading...';
        try {
          await fetch('/api/flush', { method: 'POST' });
          const response = await fetch('/download?file=' + encodeURIComponent(activeFileName));
          const text = await response.text();
          drawChart(activeFileName, text);
          await loadFiles(); // 裏でファイルリストのサイズも更新
        } catch(e) {}
        reloadBtn.innerText = '🔄 Reload Data';
      };
      
      try {
        const response = await fetch('/download?file=' + encodeURIComponent(fileName));
        const text = await response.text();
        drawChart(fileName, text);
      } catch (e) {
        document.getElementById('chartTitle').innerText = 'Error loading ' + fileName;
      }
    }

    function closeChart() {
      document.getElementById('chartModal').style.display = 'none';
      if (currentChart) {
        currentChart.destroy();
        currentChart = null;
      }
    }

    function drawChart(fileName, csvText) {
      document.getElementById('chartTitle').innerText = fileName;
      
      const lines = csvText.trim().split('\n');
      if (lines.length < 2) return;
      
      // Sequence,UptimeMs,Timestamp,CO2_ppm,Temp_C,RH_pct,Pressure_hPa,VOC_Index,NOx_Index,HR_bpm,SpO2_pct,ValidFlags
      const labels = [];
      const co2 = [];
      const temp = [];
      const rh = [];
      const press = [];
      const voc = [];
      const nox = [];
      const hr = [];
      const spo2 = [];
      const altitude = [];
      
      for (let i = 1; i < lines.length; i++) {
        const cols = lines[i].split(',');
        if (cols.length < 11) continue;
        
        let timeLabel = cols[2];
        if (!timeLabel || timeLabel === "") {
          timeLabel = (parseInt(cols[1]) / 1000).toFixed(1) + 's';
        }
        
        labels.push(timeLabel);
        co2.push(parseFloat(cols[3]));
        temp.push(parseFloat(cols[4]));
        rh.push(parseFloat(cols[5]));
        press.push(parseFloat(cols[6]));
        voc.push(parseFloat(cols[7]));
        nox.push(parseFloat(cols[8]));
        hr.push(parseFloat(cols[9]));
        spo2.push(parseFloat(cols[10]));
        altitude.push(parseFloat(cols[11]));
      }

      const ctx = document.getElementById('myChart').getContext('2d');
      if (currentChart) currentChart.destroy();
      
      currentChart = new Chart(ctx, {
        type: 'line',
        data: {
          labels: labels,
          datasets: [
            { label: 'CO2 (ppm)', data: co2, borderColor: '#e53935', backgroundColor: '#e53935', yAxisID: 'y' },
            { label: 'Temp (°C)', data: temp, borderColor: '#fb8c00', backgroundColor: '#fb8c00', yAxisID: 'y1' },
            { label: 'Humidity (%)', data: rh, borderColor: '#1e88e5', backgroundColor: '#1e88e5', yAxisID: 'y1' },
            { label: 'Pressure (hPa)', data: press, borderColor: '#43a047', backgroundColor: '#43a047', yAxisID: 'y3', hidden: true },
            { label: 'VOC Index', data: voc, borderColor: '#8e24aa', backgroundColor: '#8e24aa', yAxisID: 'y2', hidden: true },
            { label: 'NOx Index', data: nox, borderColor: '#5e35b1', backgroundColor: '#5e35b1', yAxisID: 'y2', hidden: true },
            { label: 'HR (bpm)', data: hr, borderColor: '#d81b60', backgroundColor: '#d81b60', yAxisID: 'y', hidden: true },
            { label: 'SpO2 (%)', data: spo2, borderColor: '#00acc1', backgroundColor: '#00acc1', yAxisID: 'y1', hidden: true },
            { label: 'Altitude (m)', data: altitude, borderColor: '#78909c', backgroundColor: '#78909c', yAxisID: 'y4', hidden: false }
          ]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          interaction: { mode: 'index', intersect: false },
          plugins: {
            zoom: {
              pan: {
                enabled: true,
                mode: 'x',
              },
              zoom: {
                wheel: {
                  enabled: true,
                },
                pinch: {
                  enabled: true
                },
                mode: 'x',
              }
            }
          },
          scales: {
            x: { ticks: { maxTicksLimit: 15 } },
            y: { type: 'linear', display: true, position: 'left', title: { display: true, text: 'CO2 / HR' } },
            y1: { type: 'linear', display: true, position: 'right', title: { display: true, text: 'Temp/RH/SpO2' }, grid: { drawOnChartArea: false } },
            y2: { type: 'linear', display: false, position: 'right' },
            y3: { type: 'linear', display: false, position: 'left' },
            y4: { type: 'linear', display: true, position: 'right', title: { display: true, text: 'Altitude (m)' }, grid: { drawOnChartArea: false } }
          },
          elements: { point: { radius: 0, hitRadius: 10, hoverRadius: 5 } },
          animation: false // ESP32からのロード直後の重さを軽減
        }
      });
    }

    async function calibrateSeaLevelPressure() {
      const statusEl = document.getElementById('sealevelStatus');
      statusEl.innerText = 'Fetching current sea level pressure from JMA AMeDAS (IDW)...';
      try {
        const myLat = 35.503788;
        const myLon = 139.650497;
        
        // 1. Fetch station list
        const tableRes = await fetch('https://www.jma.go.jp/bosai/amedas/const/amedastable.json');
        if (!tableRes.ok) throw new Error('Failed to fetch amedastable');
        const table = await tableRes.json();
        
        // 2. Calculate distances for all A or B stations
        let stations = [];
        for (const [id, station] of Object.entries(table)) {
          if (station.type === 'A' || station.type === 'B') {
            if (station.lat && station.lon && station.lat.length >= 2 && station.lon.length >= 2) {
              const lat = station.lat[0] + station.lat[1] / 60.0;
              const lon = station.lon[0] + station.lon[1] / 60.0;
              const distSq = Math.pow(lat - myLat, 2) + Math.pow(lon - myLon, 2);
              stations.push({ id, distSq });
            }
          }
        }
        
        // Sort by distance and pick top 5
        stations.sort((a, b) => a.distSq - b.distSq);
        const topStations = stations.slice(0, 5);
        if (topStations.length === 0) throw new Error('No valid AMeDAS station found');
        
        // 3. Fetch latest time
        const timeRes = await fetch('https://www.jma.go.jp/bosai/amedas/data/latest_time.txt');
        if (!timeRes.ok) throw new Error('Failed to fetch latest_time');
        const timeText = await timeRes.text();
        const dt = timeText.trim().replace(/[-T:]/g, '').substring(0, 14);
        
        // 4. Fetch pressure data
        const mapRes = await fetch(`https://www.jma.go.jp/bosai/amedas/data/map/${dt}.json`);
        if (!mapRes.ok) throw new Error('Failed to fetch map data');
        const mapData = await mapRes.json();
        
        // 5. Calculate IDW (Inverse Distance Weighting)
        let sumWeight = 0.0;
        let sumWeightedPressure = 0.0;
        let validCount = 0;
        let usedStations = [];
        let minP = 9999.0;
        let maxP = -9999.0;
        let minDistSq = 999999.0;
        let maxDistSq = 0.0;
        
        for (const st of topStations) {
          const stationData = mapData[st.id];
          if (stationData && stationData.normalPressure && stationData.normalPressure[0] != null) {
            const p = stationData.normalPressure[0];
            if (st.distSq < 1e-6) { // Exactly at station
              sumWeight = 1.0;
              sumWeightedPressure = p;
              validCount = 1;
              usedStations = [st.id];
              minP = maxP = p;
              minDistSq = maxDistSq = 0;
              break;
            }
            const w = 1.0 / st.distSq;
            sumWeight += w;
            sumWeightedPressure += p * w;
            validCount++;
            usedStations.push(st.id);
            if (p < minP) minP = p;
            if (p > maxP) maxP = p;
            if (st.distSq < minDistSq) minDistSq = st.distSq;
            if (st.distSq > maxDistSq) maxDistSq = st.distSq;
          }
        }
        
        if (validCount === 0) {
          throw new Error('No pressure data available for top stations');
        }
        
        let slpNum = sumWeightedPressure / sumWeight;
        // Clamp to min/max range
        if (slpNum < minP) slpNum = minP;
        if (slpNum > maxP) slpNum = maxP;
        
        const slp = slpNum.toFixed(1);
        const minDistKm = (Math.sqrt(minDistSq) * 111.0).toFixed(1);
        const maxDistKm = (Math.sqrt(maxDistSq) * 111.0).toFixed(1);
        statusEl.innerText = `Retrieved IDW: ${slp} hPa (Used ${validCount}/5 stations: ${usedStations.join(',')}, MinDist: ${minDistKm}km, MaxDist: ${maxDistKm}km). Updating ESP32...`;
        
        const postRes = await fetch('/api/sealevel', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `pressure=${slp}`
        });
        
        if (postRes.ok) {
          statusEl.innerHTML = `<span style="color: green;">✔ IDW Calibration successful! Base pressure set to ${slp} hPa.</span>`;
        } else {
          throw new Error('ESP32 rejected update');
        }
      } catch (err) {
        statusEl.innerHTML = `<span style="color: red;">✖ Failed: ${err.message}</span>`;
      }
    }

    window.onload = async () => {
      await syncTime();
      await fetchStatus();
      await loadFiles();
      calibrateSeaLevelPressure();
      setInterval(fetchStatus, 10000); // 10秒ごとに容量を更新
    };
  </script>
</body>
</html>
)rawliteral";

WebServerService::WebServerService(storage::StorageManager& storageManager)
    : storageManager_(storageManager) {
    server_.reset(new AsyncWebServer(80));
}

void WebServerService::begin() {
    setupRoutes();
    server_->begin();
    Logger::info("WebServer", "Server started on port 80");
}

void WebServerService::setupRoutes() {
    server_->on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/html", htmlContent);
    });

    server_->on("/api/files", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        JsonArray files = doc["files"].to<JsonArray>();

        String currentPath = storageManager_.getCurrentFilename();
        
        storageManager_.lock();
        File root = SD.open("/");
        if (root) {
            File file = root.openNextFile();
            while(file){
                if (!file.isDirectory()) {
                    String fileName = String(file.name());
                    if (fileName.endsWith(".csv")) {
                        JsonObject fObj = files.add<JsonObject>();
                        fObj["name"] = fileName;
                        fObj["size"] = file.size();
                        
                        // StorageManagerから取得するパスは "/log_xxx.csv" なので、
                        // fileName が "log_xxx.csv" または "/log_xxx.csv" と一致するかチェック
                        fObj["isCurrent"] = (fileName == currentPath || ("/" + fileName) == currentPath);
                    }
                }
                file = root.openNextFile();
            }
            root.close();
        }
        storageManager_.unlock();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    server_->on("/api/time", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, data, len);
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (!doc["epoch"].isNull()) {
                time_t epoch = doc["epoch"].as<time_t>();
                hal::Clock::setEpoch(epoch);
                Logger::info("WebServer", "Time synced via HTTP: %s", hal::Clock::getFormattedDate().c_str());
                request->send(200, "application/json", "{\"status\":\"ok\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing epoch\"}");
            }
    });

    server_->on("/api/flush", HTTP_POST, [this](AsyncWebServerRequest *request){
        Logger::info("WebServer", "Manual flush requested via HTTP");
        storageManager_.forceFlush();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server_->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        String response = "{";
        response += "\"pending\":" + String(storageManager_.getPendingCount()) + ",";
        response += "\"max\":" + String(storageManager_.getMaxRecords()) + ",";
        
        bool scd41Rec = false;
        if (hal::Clock::isTimeSet()) {
            uint32_t lastEpoch = storageManager_.getScd41LastCalibrationEpoch();
            if (lastEpoch == 0) {
                scd41Rec = true;
            } else {
                uint32_t nowEpoch = hal::Clock::getEpoch();
                if (nowEpoch > lastEpoch && (nowEpoch - lastEpoch) >= 604800) {
                    scd41Rec = true;
                }
            }
        }
        
        response += "\"scd41CalibRecommended\":" + String(scd41Rec ? "true" : "false");
        response += "}";
        request->send(200, "application/json", response);
    });

    server_->on("/api/scd41/calibrate", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, data, len);
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc.containsKey("target_ppm")) {
                uint16_t targetPpm = doc["target_ppm"].as<uint16_t>();
                uint16_t frcCorrection = 0;
                // 注意: この呼び出しは約500msブロックします
                if (sensorManager.calibrateScd41(targetPpm, frcCorrection)) {
                    String resp = "{\"status\":\"ok\",\"correction\":" + String(frcCorrection) + "}";
                    request->send(200, "application/json", resp);
                } else {
                    request->send(500, "application/json", "{\"error\":\"Calibration failed\"}");
                }
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing target_ppm\"}");
            }
    });

    server_->on("/api/sealevel", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("pressure", true)) {
            String pStr = request->getParam("pressure", true)->value();
            float p = pStr.toFloat();
            if (p > 800.0f && p < 1200.0f) {
                weatherService.forceUpdate(p);
                request->send(200, "text/plain", "Sea level pressure updated");
                return;
            }
        }
        request->send(400, "text/plain", "Invalid pressure parameter");
    });

    server_->on("/api/bmp581/calibrate", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, data, len);
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc.containsKey("reference_altitude_m")) {
                float refAlt = doc["reference_altitude_m"].as<float>();
                if (sensorManager.startBmp581Calibration(refAlt)) {
                    request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Calibration started (takes 6 minutes)\"}");
                } else {
                    request->send(400, "application/json", "{\"error\":\"Cannot start calibration. Pressure field might not be Valid.\"}");
                }
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing reference_altitude_m\"}");
            }
    });

    server_->on("/download", HTTP_GET, [this](AsyncWebServerRequest *request){
        if (!request->hasParam("file")) {
            request->send(400, "text/plain", "Missing file parameter");
            return;
        }
        String fileName = request->getParam("file")->value();
        String fullPath = "/" + fileName;

        // 指定されたファイルが存在しなければ404
        if (!SD.exists(fullPath)) {
            request->send(404, "text/plain", "File Not Found");
            return;
        }

        // Wi-Fiモード中はSDへの新規書き込みが停止しているため、直接ファイルを送信して問題ない
        request->send(SD, fullPath, "text/csv", true);
    });

    server_->on("/api/delete", HTTP_DELETE, [this](AsyncWebServerRequest *request){
        if (!request->hasParam("file")) {
            request->send(400, "text/plain", "Missing file parameter");
            return;
        }
        String fileName = request->getParam("file")->value();
        String fullPath = "/" + fileName;

        if (fullPath == storageManager_.getCurrentFilename() || fileName == storageManager_.getCurrentFilename()) {
            request->send(403, "text/plain", "Cannot delete currently recording file");
            return;
        }

        storageManager_.lock();
        if (SD.exists(fullPath)) {
            SD.remove(fullPath);
            storageManager_.unlock();
            Logger::info("WebServer", "File deleted via HTTP: %s", fullPath.c_str());
            request->send(200, "application/json", "{\"status\":\"deleted\"}");
        } else {
            storageManager_.unlock();
            request->send(404, "text/plain", "File Not Found");
        }
    });
}

} // namespace services
