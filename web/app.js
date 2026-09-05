'use strict';

const app = {
  csrf: '',
  page: 1,
  pageCount: 1,
  pageSize: 50,
  files: [],
  totalFiles: 0,
  listTruncated: false,
  selected: new Map(),
  archiveTimer: null,
  autoDownloadArchive: false,
  chart: null
};

const $ = (id) => document.getElementById(id);

async function api(path, options = {}) {
  const method = (options.method || 'GET').toUpperCase();
  const headers = new Headers(options.headers || {});
  if (!['GET', 'HEAD'].includes(method)) {
    headers.set('X-CSRF-Token', app.csrf);
  }
  if (options.body && !headers.has('Content-Type')) {
    headers.set('Content-Type', 'application/json');
  }
  const response = await fetch(path, { ...options, method, headers, credentials: 'same-origin' });
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`;
    const text = await response.text();
    try {
      const body = JSON.parse(text);
      message = body.error || body.message || message;
    } catch (_) { if (text) message = text; }
    throw new Error(message);
  }
  return response;
}

function showToast(message, timeout = 3500) {
  const toast = $('toast');
  toast.textContent = message;
  toast.classList.remove('hidden');
  window.clearTimeout(showToast.timer);
  showToast.timer = window.setTimeout(() => toast.classList.add('hidden'), timeout);
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB'];
  let amount = bytes / 1024;
  let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024;
    unit += 1;
  }
  return `${amount.toFixed(amount >= 10 ? 1 : 2)} ${units[unit]}`;
}

function setConnection(ok, text) {
  const badge = $('connectionBadge');
  badge.textContent = text;
  badge.className = `badge ${ok ? 'good' : 'bad'}`;
}

function setupTabs() {
  document.querySelectorAll('.tab').forEach((button) => {
    button.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach((item) => item.classList.remove('active'));
      document.querySelectorAll('.view').forEach((item) => item.classList.remove('active'));
      button.classList.add('active');
      $(`view-${button.dataset.view}`).classList.add('active');
      if (button.dataset.view === 'files') loadFiles();
    });
  });
}

async function syncTime(silent = false) {
  const result = $('routineResult');
  try {
    await api('/api/time', {
      method: 'POST',
      body: JSON.stringify({ epoch: Math.floor(Date.now() / 1000) })
    });
    if (!silent) result.textContent = '端末時刻を送信しました。';
  } catch (error) {
    if (!silent) result.textContent = `時刻同期失敗: ${error.message}`;
  }
}

async function loadStatus() {
  try {
    const responses = await Promise.all([
      api('/api/status'), api('/api/location'), api('/api/weather'),
      api('/api/altitude'), api('/api/ppg')
    ]);
    const [status, location, weather, altitude, ppg] = await Promise.all(
      responses.map((response) => response.json())
    );

    const percent = status.max ? Math.round(status.pending * 100 / status.max) : 0;
    $('storageValue').textContent = `${status.pending} / ${status.max}`;
    $('storageDetail').textContent = `使用率 ${percent}% · drop ${status.dropped_records} · event ${status.event_count}/${status.event_max}${status.fram_read_only ? ' · FRAM READ ONLY' : ''}`;
    $('i2cValue').textContent = `${status.i2c_lock_timeouts + status.i2c_communication_errors} errors`;
    $('i2cDetail').textContent = `lock timeout ${status.i2c_lock_timeouts} · communication ${status.i2c_communication_errors}`;

    $('locationValue').textContent = location.valid ? location.source : '利用不可';
    $('locationDetail').textContent = location.valid
      ? `${location.latitudeDeg.toFixed(5)}, ${location.longitudeDeg.toFixed(5)} · age ${location.ageMs == null ? '?' : Math.round(location.ageMs / 1000)} s`
      : 'GNSSと設定fallbackを確認してください。';

    $('weatherValue').textContent = weather.has_pressure_field
      ? `${weather.pressure_hpa.toFixed(1)} hPa` : '無効';
    $('weatherDetail').textContent = `${weather.state} · ${weather.used_station_count}/${weather.cached_station_count} stations · age ${weather.observation_age_ms == null ? '?' : Math.round(weather.observation_age_ms / 60000)} min`;

    $('altitudeValue').textContent = altitude.altitude_valid
      ? `${altitude.display_altitude_m.toFixed(1)} m` : '無効';
    $('altitudeDetail').textContent = `${altitude.pressure_field_state} · ${altitude.calculation_temperature_source || '温度なし'} · ${altitude.calibration_state}`;

    $('ppgValue').textContent = ppg.state;
    $('ppgDetail').textContent = ppg.session_id
      ? `${ppg.session_id} · ${ppg.stored_samples} samples · drop ${ppg.dropped_samples} · ${ppg.buffer_memory}`
      : `sessionなし · ${ppg.buffer_memory}`;
    setConnection(true, status.clock_valid ? `接続済 · ${status.clock_source}` : '接続済 · 時刻未確定');
  } catch (error) {
    setConnection(false, '通信エラー');
    showToast(`状態取得失敗: ${error.message}`);
  }
}

function queryString() {
  const sort = $('fileSort').value.split('_');
  const params = new URLSearchParams({
    page: String(app.page), limit: String(app.pageSize),
    q: $('fileSearch').value.trim(), type: $('fileType').value,
    sort: sort[0], order: sort[1]
  });
  return params.toString();
}

function typeLabel(type) {
  return { log: '環境ログ', ppg: 'PPG', archive: 'ZIP', diagnostic: '診断' }[type] || type;
}

function selectedFiles() {
  return Array.from(app.selected, ([path, size]) => ({ path, size }));
}

function updateSelection() {
  const selected = selectedFiles();
  const total = selected.reduce((sum, item) => sum + item.size, 0);
  $('selectionCount').textContent = `${selected.length}件選択`;
  $('selectionSize').textContent = formatBytes(total);
  $('downloadSelected').disabled = selected.length === 0 || selected.length > 64;
  $('deleteSelected').disabled = selected.length === 0 || selected.length > 64;
}

function cell(text, className = '') {
  const element = document.createElement('td');
  element.textContent = text;
  if (className) element.className = className;
  return element;
}

function downloadPath(path) {
  const anchor = document.createElement('a');
  anchor.href = `/download?file=${encodeURIComponent(path)}`;
  anchor.download = path.split('/').pop();
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
}

function renderFiles(data) {
  app.files = data.files;
  app.page = data.page;
  app.pageCount = Math.max(1, data.page_count);
  app.totalFiles = data.total;
  app.listTruncated = data.truncated;
  const body = $('fileRows');
  body.replaceChildren();
  if (data.files.length === 0) {
    const row = document.createElement('tr');
    const empty = cell('条件に一致するファイルはありません。', 'empty');
    empty.colSpan = 6;
    row.appendChild(empty);
    body.appendChild(row);
  }

  data.files.forEach((file) => {
    const row = document.createElement('tr');
    const checkCell = document.createElement('td');
    checkCell.className = 'check-cell';
    const check = document.createElement('input');
    check.type = 'checkbox';
    check.disabled = !file.selectable;
    check.checked = app.selected.has(file.path);
    check.setAttribute('aria-label', `${file.name}を選択`);
    check.addEventListener('change', () => {
      if (check.checked) app.selected.set(file.path, file.size);
      else app.selected.delete(file.path);
      updateSelection();
    });
    checkCell.appendChild(check);
    row.appendChild(checkCell);

    const nameCell = document.createElement('td');
    const name = document.createElement('div');
    name.className = 'file-name';
    name.textContent = file.name;
    const path = document.createElement('div');
    path.className = 'file-path';
    path.textContent = file.path;
    nameCell.append(name, path);
    row.appendChild(nameCell);
    row.appendChild(cell(typeLabel(file.type)));
    row.appendChild(cell(formatBytes(file.size)));

    const stateCell = document.createElement('td');
    const tag = document.createElement('span');
    tag.className = `state-tag ${file.locked ? 'locked' : ''}`;
    tag.textContent = file.locked ? '記録中・保護' : '操作可能';
    stateCell.appendChild(tag);
    row.appendChild(stateCell);

    const actionCell = document.createElement('td');
    const actions = document.createElement('div');
    actions.className = 'row-actions';
    const download = document.createElement('button');
    download.type = 'button';
    download.className = 'secondary';
    download.textContent = '取得';
    download.disabled = !file.downloadable;
    download.addEventListener('click', () => downloadPath(file.path));
    actions.appendChild(download);
    if (file.type === 'log' && file.path.endsWith('.csv')) {
      const graph = document.createElement('button');
      graph.type = 'button';
      graph.className = 'secondary';
      graph.textContent = 'グラフ';
      graph.disabled = !file.downloadable;
      graph.addEventListener('click', () => openChart(file));
      actions.appendChild(graph);
    }
    actionCell.appendChild(actions);
    row.appendChild(actionCell);
    body.appendChild(row);
  });

  $('pageStatus').textContent = `${data.page} / ${Math.max(1, data.page_count)}ページ · 全${data.total}件${data.truncated ? '（上限到達）' : ''}`;
  $('previousPage').disabled = data.page <= 1;
  $('nextPage').disabled = data.page >= data.page_count;
  updateSelection();
}

async function loadFiles(resetPage = false) {
  if (resetPage) app.page = 1;
  try {
    const response = await api(`/api/files?${queryString()}`);
    renderFiles(await response.json());
  } catch (error) {
    $('fileRows').replaceChildren();
    const row = document.createElement('tr');
    const empty = cell(`一覧取得失敗: ${error.message}`, 'empty');
    empty.colSpan = 6;
    row.appendChild(empty);
    $('fileRows').appendChild(row);
  }
}

async function startArchive(autoDownload) {
  const files = selectedFiles().map((item) => item.path);
  if (files.length === 0 || files.length > 64) return;
  try {
    await api('/api/archive/manual', {
      method: 'POST', body: JSON.stringify({ files })
    });
    app.autoDownloadArchive = autoDownload;
    $('archiveNotice').textContent = 'ZIPを作成しています。計測は継続します。';
    $('archiveNotice').classList.remove('hidden');
    pollArchive();
  } catch (error) {
    showToast(`ZIP作成を開始できません: ${error.message}`);
  }
}

function pollArchive() {
  window.clearInterval(app.archiveTimer);
  const tick = async () => {
    try {
      const response = await api('/api/archive/status');
      const status = await response.json();
      const notice = $('archiveNotice');
      notice.classList.remove('hidden');
      if (status.state === 4) {
        window.clearInterval(app.archiveTimer);
        app.archiveTimer = null;
        notice.textContent = `ZIP検証完了: ${status.outputFile}`;
        if (app.autoDownloadArchive && status.outputFile) downloadPath(status.outputFile);
        app.autoDownloadArchive = false;
        app.selected.clear();
        updateSelection();
        loadFiles();
      } else if (status.state === 5) {
        window.clearInterval(app.archiveTimer);
        app.archiveTimer = null;
        notice.textContent = `ZIP作成失敗: ${status.message}`;
        app.autoDownloadArchive = false;
      } else {
        notice.textContent = `${status.message} · ${status.processedFiles}/${status.totalFiles} · ${status.progressPercent}%`;
      }
    } catch (error) {
      window.clearInterval(app.archiveTimer);
      app.archiveTimer = null;
      showToast(`ZIP状態取得失敗: ${error.message}`);
    }
  };
  tick();
  app.archiveTimer = window.setInterval(tick, 1000);
}

async function deleteSelected() {
  const selected = selectedFiles();
  if (selected.length === 0 || selected.length > 64) return;
  const total = selected.reduce((sum, item) => sum + item.size, 0);
  if (!window.confirm(`${selected.length}件（${formatBytes(total)}）を削除します。元に戻せません。続行しますか？`)) return;
  try {
    const response = await api('/api/files/delete', {
      method: 'POST',
      body: JSON.stringify({ files: selected.map((item) => item.path), confirmation: 'DELETE_SELECTED' })
    });
    const result = await response.json();
    result.deleted.forEach((path) => app.selected.delete(path));
    showToast(`${result.deleted.length}件を削除、${result.rejected.length}件を保護しました。`);
    await loadFiles();
  } catch (error) {
    showToast(`削除失敗: ${error.message}`);
  }
}

const chartMetrics = [
  ['CO2_ppm', 'CO₂ (ppm)', '#d64045'], ['Temp_C', '温度 (°C)', '#e47d20'],
  ['RH_pct', '湿度 (%)', '#247ba0'], ['Pressure_hPa', '気圧 (hPa)', '#2d936c'],
  ['VOC_Index', 'VOC指数', '#815ac0'], ['NOx_Index', 'NOx指数', '#5c4d9b'],
  ['HR_bpm', '心拍数 (bpm)', '#c62e65'], ['SpO2_pct', 'SpO₂ (%)', '#00a7a5'],
  ['BMP_Altitude_m', '高度 (m)', '#596b75'], ['Altitude_m', '高度 (m)', '#596b75']
];

async function openChart(file) {
  const dialog = $('chartDialog');
  $('chartTitle').textContent = file.name;
  $('chartMessage').textContent = '読込中…';
  dialog.showModal();
  try {
    const response = await api(`/download?file=${encodeURIComponent(file.path)}`);
    const text = await response.text();
    const lines = text.trim().split(/\r?\n/);
    const headers = lines.shift().split(',').map((item) => item.trim());
    const available = chartMetrics.filter(([column]) => headers.includes(column));
    if (available.length === 0) throw new Error('描画できる列がありません');
    const records = lines.map((line) => line.split(','));
    app.chart = { headers, records, available };
    const select = $('chartMetric');
    select.replaceChildren();
    available.forEach(([column, label]) => {
      const option = document.createElement('option');
      option.value = column;
      option.textContent = label;
      select.appendChild(option);
    });
    $('chartMessage').textContent = `${records.length}行（描画は最大1000点へ間引き）`;
    drawChart();
  } catch (error) {
    $('chartMessage').textContent = `グラフ読込失敗: ${error.message}`;
  }
}

function drawChart() {
  if (!app.chart) return;
  const canvas = $('chartCanvas');
  const context = canvas.getContext('2d');
  const column = $('chartMetric').value;
  const index = app.chart.headers.indexOf(column);
  const config = chartMetrics.find(([name]) => name === column);
  const step = Math.max(1, Math.ceil(app.chart.records.length / 1000));
  const values = [];
  for (let i = 0; i < app.chart.records.length; i += step) {
    const value = Number.parseFloat(app.chart.records[i][index]);
    if (Number.isFinite(value)) values.push(value);
  }
  context.clearRect(0, 0, canvas.width, canvas.height);
  context.fillStyle = '#ffffff';
  context.fillRect(0, 0, canvas.width, canvas.height);
  if (values.length < 2) {
    $('chartMessage').textContent = '有効値が不足しています。';
    return;
  }
  let minimum = Math.min(...values);
  let maximum = Math.max(...values);
  if (minimum === maximum) { minimum -= 1; maximum += 1; }
  const margin = { left: 68, right: 18, top: 22, bottom: 42 };
  const width = canvas.width - margin.left - margin.right;
  const height = canvas.height - margin.top - margin.bottom;
  context.strokeStyle = '#d9e2db';
  context.fillStyle = '#607067';
  context.font = '15px sans-serif';
  for (let line = 0; line <= 4; line += 1) {
    const y = margin.top + height * line / 4;
    context.beginPath(); context.moveTo(margin.left, y); context.lineTo(margin.left + width, y); context.stroke();
    const label = maximum - (maximum - minimum) * line / 4;
    context.fillText(label.toFixed(2), 8, y + 5);
  }
  context.strokeStyle = config ? config[2] : '#176b46';
  context.lineWidth = 2;
  context.beginPath();
  values.forEach((value, point) => {
    const x = margin.left + width * point / (values.length - 1);
    const y = margin.top + height * (maximum - value) / (maximum - minimum);
    if (point === 0) context.moveTo(x, y); else context.lineTo(x, y);
  });
  context.stroke();
  context.fillStyle = '#607067';
  context.fillText('開始', margin.left, canvas.height - 12);
  context.fillText('終了', canvas.width - margin.right - 30, canvas.height - 12);
}

async function postMaintenance(path, body, resultId) {
  const result = $(resultId);
  result.textContent = '処理中…';
  try {
    const response = await api(path, { method: 'POST', body: JSON.stringify(body) });
    const data = await response.json();
    result.textContent = data.message || data.status || '完了しました。';
    loadStatus();
  } catch (error) {
    result.textContent = `失敗: ${error.message}`;
  }
}

function setupActions() {
  $('refreshStatus').addEventListener('click', loadStatus);
  $('refreshFiles').addEventListener('click', () => loadFiles());
  $('fileSearch').addEventListener('input', () => {
    window.clearTimeout(setupActions.searchTimer);
    setupActions.searchTimer = window.setTimeout(() => loadFiles(true), 300);
  });
  $('fileType').addEventListener('change', () => loadFiles(true));
  $('fileSort').addEventListener('change', () => loadFiles(true));
  $('previousPage').addEventListener('click', () => { app.page -= 1; loadFiles(); });
  $('nextPage').addEventListener('click', () => { app.page += 1; loadFiles(); });
  $('selectPage').addEventListener('click', () => {
    app.files.filter((file) => file.selectable).forEach((file) => app.selected.set(file.path, file.size));
    renderFiles({ files: app.files, page: app.page, page_count: app.pageCount, total: app.totalFiles, truncated: app.listTruncated });
  });
  $('clearSelection').addEventListener('click', () => { app.selected.clear(); loadFiles(); });
  $('downloadSelected').addEventListener('click', () => {
    const selected = selectedFiles();
    if (selected.length === 1) downloadPath(selected[0].path);
    else startArchive(true);
  });
  $('deleteSelected').addEventListener('click', deleteSelected);
  $('syncTime').addEventListener('click', () => syncTime(false));
  $('flushStorage').addEventListener('click', async () => {
    try {
      await api('/api/flush', { method: 'POST' });
      $('routineResult').textContent = 'Flushと読戻し確認を要求しました。';
      loadStatus(); loadFiles();
    } catch (error) { $('routineResult').textContent = `Flush失敗: ${error.message}`; }
  });
  $('calibrateBmp').addEventListener('click', () => postMaintenance('/api/bmp581/calibrate', { reference_altitude_m: 13.6 }, 'bmpResult'));
  $('calibrateScd').addEventListener('click', () => {
    const reference = Number.parseInt($('scdReference').value, 10);
    if (!$('confirmFrc').checked || !Number.isFinite(reference)) {
      $('scdResult').textContent = '外部基準値と確認チェックが必要です。'; return;
    }
    postMaintenance('/api/scd41/calibrate', { reference_ppm: reference, confirm_external_reference: true }, 'scdResult');
  });
  $('resetScd').addEventListener('click', () => {
    if (!$('confirmReset').checked) { $('resetResult').textContent = '確認チェックが必要です。'; return; }
    if (!window.confirm('SCD41のユーザー設定と校正履歴を消去します。続行しますか？')) return;
    postMaintenance('/api/scd41/factory_reset', { confirmation: 'RESET_SCD41' }, 'resetResult');
  });
  $('chartMetric').addEventListener('change', drawChart);
  $('closeChart').addEventListener('click', () => $('chartDialog').close());
}

async function boot() {
  setupTabs();
  setupActions();
  try {
    const response = await api('/api/session');
    const session = await response.json();
    app.csrf = session.csrf_token;
    setConnection(true, '接続済');
    if (!session.clock_valid) await syncTime(true);
    await Promise.all([loadStatus(), loadFiles()]);
    window.setInterval(loadStatus, 10000);
  } catch (error) {
    setConnection(false, '認証・接続エラー');
    showToast(`初期化失敗: ${error.message}`, 8000);
  }
}

window.addEventListener('DOMContentLoaded', boot);
