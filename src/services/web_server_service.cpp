#include "services/web_server_service.h"
#include "services/logger.h"
#include "hal/clock.h"
#include <ArduinoJson.h>
#include <SD.h>

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
  </style>
</head>
<body>
  <h1>Data Logs</h1>
  <div id="status" class="status">Connecting...</div>
  <ul id="file-list"></ul>

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
          nameSpan.innerText = f.name + ' (' + formatBytes(f.size) + ')';
          if (f.isCurrent) {
            nameSpan.innerText += ' [Recording...]';
            nameSpan.style.color = '#d32f2f';
            nameSpan.style.fontWeight = 'bold';
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

    window.onload = async () => {
      await syncTime();
      await loadFiles();
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
