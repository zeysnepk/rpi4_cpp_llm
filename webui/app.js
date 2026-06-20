// ============================================================
// CHAT
// ============================================================
const chat = document.getElementById('chat');
const form = document.getElementById('form');
const input = document.getElementById('input');
const sendBtn = document.getElementById('send');
const statusEl = document.getElementById('status');

// NOT: Backend kendi system prompt'unu enjekte ediyor (IntentRouter + LLM-Interpreter).
// Burada sadece user mesajlarini yolluyoruz. History opsiyonel olarak gonderilebilir,
// ama backend simdilik sadece son user mesajini kullaniyor.

const history = [];
let latestSensors = null;

let scrollPending = false;
function scheduleScroll() {
    if (scrollPending) return;
    scrollPending = true;
    requestAnimationFrame(() => {
        scrollPending = false;
        chat.scrollTop = chat.scrollHeight;
    });
}

function setStatus(state, label) {
    statusEl.classList.remove('ok', 'err');
    if (state) statusEl.classList.add(state);
    statusEl.querySelector('.label').textContent = label;
}

function addMessage(role, content = '') {
    const wrap = document.createElement('div');
    wrap.className = `message ${role}`;
    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    bubble.textContent = content;
    wrap.appendChild(bubble);
    chat.appendChild(wrap);
    chat.scrollTop = chat.scrollHeight;
    return bubble;
}

async function checkHealth() {
    try {
        const r = await fetch('/api/health');
        if (r.ok) setStatus('ok', 'bağlı');
        else setStatus('err', 'sunucu hata');
    } catch {
        setStatus('err', 'bağlantı yok');
    }
}

async function sendMessage(text) {
    addMessage('user', text);
    const bubble = addMessage('assistant', '');
    bubble.classList.add('thinking');
    bubble.innerHTML = '<span class="dot">.</span><span class="dot">.</span><span class="dot">.</span>';

    history.push({ role: 'user', content: text });
    // Backend sadece son user mesajini kullaniyor; yine de history yollayalim (gelecekte multi-turn icin)
    const messages = [...history];

    let toolBlock = null;
    let finalText = '';
    let isStreaming = false;

    try {
        const response = await fetch('/api/chat', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ messages })
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            buffer += decoder.decode(value, { stream: true });
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.startsWith('data: ')) continue;
                const data = line.slice(6).trim();
                if (!data || data === '[DONE]') continue;

                let ev;
                try { ev = JSON.parse(data); } catch { continue; }

                if (ev.type === 'content_delta') {
                    if (!isStreaming) {
                        bubble.classList.remove('thinking');
                        bubble.textContent = '';
                        isStreaming = true;
                    }
                    finalText += ev.text;
                    bubble.textContent = finalText;
                    scheduleScroll();
                }
                else if (ev.type === 'tool_call') {
                    if (!toolBlock) {
                        toolBlock = document.createElement('div');
                        toolBlock.className = 'tool-block';
                        bubble.parentElement.insertBefore(toolBlock, bubble);
                    }
                    const item = document.createElement('div');
                    item.className = 'tool-item pending';
                    item.innerHTML = `🔧 <code>${ev.name}</code>(${JSON.stringify(ev.args)})`;
                    toolBlock.appendChild(item);

                    if (!isStreaming) {
                        bubble.classList.add('thinking');
                        bubble.innerHTML = '<span class="dot">.</span><span class="dot">.</span><span class="dot">.</span>';
                    }
                    scheduleScroll();
                }
                else if (ev.type === 'tool_result') {
                    if (toolBlock) {
                        const pending = toolBlock.querySelector('.tool-item.pending');
                        if (pending) {
                            pending.classList.remove('pending');
                            pending.classList.add('done');
                            const res = document.createElement('pre');
                            res.className = 'tool-result';
                            res.textContent = JSON.stringify(ev.result, null, 2);
                            pending.appendChild(res);
                        }
                    }
                }
                else if (ev.type === 'done') {
                    bubble.classList.remove('thinking');
                    if (!isStreaming && !finalText) {
                        bubble.textContent = '(boş yanıt)';
                    }
                }
                else if (ev.type === 'error') {
                    bubble.classList.remove('thinking');
                    bubble.parentElement.classList.remove('assistant');
                    bubble.parentElement.classList.add('error');
                    bubble.textContent = `Hata: ${ev.message}`;
                }
            }
        }

        if (finalText) history.push({ role: 'assistant', content: finalText });
    } catch (err) {
        bubble.classList.remove('thinking');
        bubble.parentElement.classList.remove('assistant');
        bubble.parentElement.classList.add('error');
        bubble.textContent = `Hata: ${err.message}`;
    }
}

form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const text = input.value.trim();
    if (!text || sendBtn.disabled) return;
    input.value = '';
    input.style.height = 'auto';
    sendBtn.disabled = true;
    try { await sendMessage(text); }
    finally { sendBtn.disabled = false; input.focus(); }
});

input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        form.requestSubmit();
    }
});
input.addEventListener('input', () => {
    input.style.height = 'auto';
    input.style.height = Math.min(input.scrollHeight, 160) + 'px';
});

// ============================================================
// SENSOR KARTLARI
// ============================================================
function fmt(n, digits=2) {
    if (typeof n !== 'number') return String(n);
    return n.toFixed(digits);
}

function renderReadings(card, data) {
    const lines = [];
    const walk = (obj, prefix='') => {
        for (const k in obj) {
            const v = obj[k];
            if (v !== null && typeof v === 'object') walk(v, prefix + k + '.');
            else lines.push(`<span class="k">${prefix}${k}:</span> <span class="v">${fmt(v)}</span>`);
        }
    };
    walk(data);
    card.querySelector('.readings').innerHTML = lines.join('<br>');
}

// ============================================================
// GRAFIKLER (Web Worker)
// ============================================================
const chartWorker = new Worker('chart-worker.js');

function initCharts() {
    requestAnimationFrame(() => {
        for (const name of ['bme280', 'mpu6050', 'qmc5883l']) {
            const canvas = document.getElementById('chart-' + name);
            const wrap = canvas.parentElement;
            const w = wrap.clientWidth  || 600;
            const h = wrap.clientHeight || 200;
            canvas.width = w;
            canvas.height = h;
            const off = canvas.transferControlToOffscreen();
            chartWorker.postMessage({ type: 'init', name, canvas: off }, [off]);
        }
    });
}

function pushPoint(name, label, values) {
    chartWorker.postMessage({ type: 'update', name, label, values });
}

let resizeTimer = null;
window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
        for (const name of ['bme280', 'mpu6050', 'qmc5883l']) {
            const wrap = document.getElementById('chart-' + name).parentElement;
            chartWorker.postMessage({
                type: 'resize', name,
                width:  wrap.clientWidth,
                height: wrap.clientHeight
            });
        }
    }, 150);
});

function timeLabel() {
    const t = new Date();
    return `${String(t.getMinutes()).padStart(2,'0')}:${String(t.getSeconds()).padStart(2,'0')}`;
}
function gyroMag(g) { return Math.sqrt(g.x*g.x + g.y*g.y + g.z*g.z); }

// ============================================================
// SENSOR STREAM
// ============================================================
let sse = null;

function updateCards(all) {
    document.querySelectorAll('.sensor-card').forEach(card => {
        const name = card.dataset.name;
        const info = all[name];
        if (!info) {
            card.classList.remove('online'); card.classList.add('offline');
            card.querySelector('.readings').textContent = 'Yapılandırılmamış';
            return;
        }
        card.querySelector('.rate').textContent = info.rate_hz ? `${info.rate_hz} Hz` : '';
        if (info.online && info.data) {
            card.classList.add('online'); card.classList.remove('offline');
            renderReadings(card, info.data);
        } else {
            card.classList.remove('online'); card.classList.add('offline');
            card.querySelector('.readings').textContent = 'Offline';
        }
    });
}

let chartUpdatePending = false;
function scheduleChartUpdate() {
    if (!latestSensors) return;
    const label = timeLabel();
    for (const [name, info] of Object.entries(latestSensors)) {
        if (!info?.online || !info.data) continue;
        const d = info.data;
        if (name === 'bme280') {
            pushPoint('bme280', label, [d.temperature_c, d.humidity_pct, d.pressure_hpa]);
        } else if (name === 'mpu6050') {
            pushPoint('mpu6050', label,
                [d.accel_g.x, d.accel_g.y, d.accel_g.z, gyroMag(d.gyro_dps)]);
        } else if (name === 'qmc5883l') {
            pushPoint('qmc5883l', label, [d.mag_g.x, d.mag_g.y, d.mag_g.z, d.heading_deg]);
        }
    }
}

function startSensorStream() {
    if (sse) sse.close();
    sse = new EventSource('/api/sensors/stream');

    sse.onmessage = (event) => {
        try {
            latestSensors = JSON.parse(event.data);
            updateCards(latestSensors);
            scheduleChartUpdate();
        } catch (e) { /* parse errors */ }
    };

    sse.onerror = () => {
        setStatus('err', 'sensör bağlantısı koptu, deniyor...');
        setTimeout(() => checkHealth(), 1500);
    };
}

// ============================================================
// MODE SWITCH
// ============================================================
async function loadModeStatus() {
    try {
        const r = await fetch('/api/mode');
        if (!r.ok) return;
        const info = await r.json();
        const btn = document.getElementById('modeToggle');
        btn.classList.remove('real', 'sim');
        btn.classList.add(info.current_mode);
        btn.querySelector('.mode-text').textContent = info.current_mode;
        btn.querySelector('.mode-icon').textContent =
            info.current_mode === 'real' ? '📡' : '🎲';
        btn.title = `Platform: ${info.platform} | Mode: ${info.current_mode}\nTıkla → ${info.current_mode === 'real' ? 'sim' : 'real'}'e geç`;
    } catch { /* ignore */ }
}

async function waitForServerBack() {
    setStatus('err', 'yeniden başlatılıyor...');
    for (let i = 0; i < 60; i++) {
        await new Promise(r => setTimeout(r, 1000));
        try {
            const r = await fetch('/api/health');
            if (r.ok) { window.location.reload(); return; }
        } catch { /* still down */ }
    }
    setStatus('err', 'sunucu dönmedi');
}

async function toggleMode() {
    const btn = document.getElementById('modeToggle');
    const current = btn.querySelector('.mode-text').textContent;
    const next = current === 'real' ? 'sim' : 'real';

    if (!confirm(`Mode: ${current.toUpperCase()} → ${next.toUpperCase()}\n\nDashboard yeniden başlatılacak (~3 sn). Devam edilsin mi?`)) {
        return;
    }

    btn.disabled = true;
    btn.querySelector('.mode-text').textContent = '...';

    try {
        const r = await fetch('/api/mode', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mode: next })
        });
        if (!r.ok) throw new Error('mode change failed');
        waitForServerBack();
    } catch (err) {
        btn.disabled = false;
        alert(`Hata: ${err.message}`);
        loadModeStatus();
    }
}

document.getElementById('modeToggle').addEventListener('click', toggleMode);

// ============================================================
// TRANSLATOR STATUS
// ============================================================
async function loadTranslatorStatus() {
    try {
        const r = await fetch('/api/translator');
        if (!r.ok) return;
        const info = await r.json();
        const badge = document.getElementById('translatorBadge');
        const text = badge.querySelector('.tr-text');

        badge.classList.remove('on', 'off', 'err');
        if (!info.enabled) {
            badge.classList.add('off');
            text.textContent = 'TR direkt';
            badge.title = 'Translator kapalı — LLM TR direkt cevap veriyor';
        } else if (info.available) {
            badge.classList.add('on');
            text.textContent = 'TR↔EN';
            badge.title = 'LibreTranslate çalışıyor';
        } else {
            badge.classList.add('err');
            text.textContent = 'TR↔EN ✗';
            badge.title = 'LibreTranslate erişilemez';
        }
    } catch { /* ignore */ }
}

// ============================================================
// EXAMPLE CHIPS
// ============================================================
const EXAMPLE_PROMPTS = [
    { icon: '🌡', label: 'Temperature',    text: 'What is the current temperature?' },
    { icon: '💧', label: 'Humidity',       text: 'What is the current humidity?' },
    { icon: '📊', label: '30s trend',      text: 'Show me the last 30 seconds trend for bme280' },
    { icon: '🔢', label: 'Last 10 reads',  text: 'Show last 10 readings of bme280' },
    { icon: '⚠️', label: 'Anomalies',      text: 'Are there any anomalies right now?' },
    { icon: '📡', label: 'All sensors',    text: 'Show all sensor readings' },
    { icon: '🧭', label: 'Heading',        text: 'What is the compass heading?' },
    { icon: '⚙',  label: 'What can I set', text: 'What can I change?' },
];

function initExampleChips() {
    const wrap = document.getElementById('exampleChips');
    if (!wrap) return;
    EXAMPLE_PROMPTS.forEach(p => {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'chip';
        btn.innerHTML = `<span class="chip-icon">${p.icon}</span>${p.label}`;
        btn.title = p.text;
        btn.addEventListener('click', () => {
            input.value = p.text;
            input.style.height = 'auto';
            input.style.height = Math.min(input.scrollHeight, 160) + 'px';
            input.focus();
            form.requestSubmit();
        });
        wrap.appendChild(btn);
    });
}

// ============================================================
// SETTINGS PANEL
// ============================================================
const SENSOR_DEFS = [
    { key: 'bme280',   label: 'BME280',   maxHz: 50  },
    { key: 'mpu6050',  label: 'MPU6500',  maxHz: 200 },
    { key: 'qmc5883l', label: 'QMC5883L', maxHz: 20  },
];
const THRESHOLD_LABELS = {
    'bme280.temperature_c': 'Temperature (°C)',
    'bme280.humidity_pct':  'Humidity (%)',
    'bme280.pressure_hpa':  'Pressure (hPa)',
    'mpu6050.accel_g.x':    'Accel X (g)',
    'mpu6050.accel_g.y':    'Accel Y (g)',
    'mpu6050.accel_g.z':    'Accel Z (g)',
    'mpu6050.gyro_dps.x':   'Gyro X (°/s)',
    'mpu6050.gyro_dps.y':   'Gyro Y (°/s)',
    'mpu6050.gyro_dps.z':   'Gyro Z (°/s)',
    'mpu6050.temp_c':       'Internal Temp (°C)',
    'qmc5883l.heading_deg': 'Heading (°)',
};

async function callTool(name, args) {
    const r = await fetch('/api/tool', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, args })
    });
    if (!r.ok) throw new Error(`tool ${name} failed: ${r.status}`);
    return r.json();
}

function flashBtn(btn, ok) {
    const cls = ok ? 'ok-flash' : 'err-flash';
    btn.classList.add(cls);
    const prev = btn.textContent;
    btn.textContent = ok ? '✓' : '✗';
    setTimeout(() => {
        btn.classList.remove(cls);
        btn.textContent = prev;
    }, 1200);
}

function buildSensorPane(sensorKey, sensorCfg, allThresholds, maxHz) {
    const pane = document.createElement('div');
    pane.className = 'spane';
    pane.id = 'spane-' + sensorKey;

    // ── Enable toggle ──
    const enSection = document.createElement('div');
    enSection.className = 's-section';
    enSection.textContent = 'SENSOR';
    pane.appendChild(enSection);

    const enRow = document.createElement('div');
    enRow.className = 's-row';
    const enLabel = document.createElement('span');
    enLabel.className = 's-label';
    enLabel.textContent = 'Enable sensor';

    const togWrap = document.createElement('div');
    togWrap.className = 'tog-wrap';
    const togLabel = document.createElement('span');
    togLabel.className = 'tog-label';
    togLabel.textContent = sensorCfg?.enabled !== false ? 'ON' : 'OFF';

    const togEl = document.createElement('label');
    togEl.className = 'tog';
    const togInput = document.createElement('input');
    togInput.type = 'checkbox';
    togInput.checked = sensorCfg?.enabled !== false;
    const togTrack = document.createElement('span');
    togTrack.className = 'tog-track';
    togEl.appendChild(togInput);
    togEl.appendChild(togTrack);

    togInput.addEventListener('change', async () => {
        togLabel.textContent = togInput.checked ? 'ON' : 'OFF';
        try {
            await callTool('set_sensor_enabled', {
                sensor: sensorKey,
                enabled: togInput.checked
            });
        } catch { togInput.checked = !togInput.checked; togLabel.textContent = togInput.checked ? 'ON' : 'OFF'; }
    });

    togWrap.appendChild(togLabel);
    togWrap.appendChild(togEl);
    enRow.appendChild(enLabel);
    enRow.appendChild(togWrap);
    pane.appendChild(enRow);

    // ── Sample rate slider ──
    const rateSection = document.createElement('div');
    rateSection.className = 's-section';
    rateSection.textContent = 'SAMPLE RATE';
    pane.appendChild(rateSection);

    const rateRow = document.createElement('div');
    rateRow.className = 'rate-row';

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.className = 'rate-slider';
    slider.min = 1;
    slider.max = maxHz;
    slider.value = sensorCfg?.sample_rate_hz ?? 10;

    const rateVal = document.createElement('span');
    rateVal.className = 'rate-val';
    rateVal.textContent = slider.value + ' Hz';

    const rateApply = document.createElement('button');
    rateApply.className = 's-apply';
    rateApply.textContent = 'Apply';

    slider.addEventListener('input', () => {
        rateVal.textContent = slider.value + ' Hz';
    });
    rateApply.addEventListener('click', async () => {
        rateApply.disabled = true;
        try {
            await callTool('set_sample_rate', {
                sensor: sensorKey,
                hz: parseInt(slider.value)
            });
            flashBtn(rateApply, true);
        } catch { flashBtn(rateApply, false); }
        finally { rateApply.disabled = false; }
    });

    rateRow.appendChild(slider);
    rateRow.appendChild(rateVal);
    rateRow.appendChild(rateApply);
    pane.appendChild(rateRow);

    // ── Thresholds ──
    const thr = Object.entries(allThresholds || {})
        .filter(([k]) => k.startsWith(sensorKey));
    if (thr.length > 0) {
        const thrSection = document.createElement('div');
        thrSection.className = 's-section';
        thrSection.textContent = 'ANOMALY THRESHOLDS';
        pane.appendChild(thrSection);

        const table = document.createElement('table');
        table.className = 'thr-table';

        thr.forEach(([metric, vals]) => {
            const tr = document.createElement('tr');

            const tdLabel = document.createElement('td');
            const nameSpan = document.createElement('span');
            nameSpan.className = 'thr-metric';
            nameSpan.textContent = THRESHOLD_LABELS[metric] || metric;
            tdLabel.appendChild(nameSpan);

            const tdRange = document.createElement('td');
            const rangeDiv = document.createElement('div');
            rangeDiv.className = 'thr-range';

            const minInput = document.createElement('input');
            minInput.type = 'number';
            minInput.className = 'thr-input';
            minInput.value = vals.min ?? '';
            minInput.step = 'any';
            minInput.placeholder = 'min';

            const sep = document.createElement('span');
            sep.className = 'thr-sep';
            sep.textContent = '—';

            const maxInput = document.createElement('input');
            maxInput.type = 'number';
            maxInput.className = 'thr-input';
            maxInput.value = vals.max ?? '';
            maxInput.step = 'any';
            maxInput.placeholder = 'max';

            const applyBtn = document.createElement('button');
            applyBtn.className = 's-apply';
            applyBtn.textContent = 'Apply';
            applyBtn.addEventListener('click', async () => {
                applyBtn.disabled = true;
                const args = { metric };
                if (minInput.value !== '') args.min = parseFloat(minInput.value);
                if (maxInput.value !== '') args.max = parseFloat(maxInput.value);
                try {
                    await callTool('set_threshold', args);
                    flashBtn(applyBtn, true);
                } catch { flashBtn(applyBtn, false); }
                finally { applyBtn.disabled = false; }
            });

            rangeDiv.appendChild(minInput);
            rangeDiv.appendChild(sep);
            rangeDiv.appendChild(maxInput);
            rangeDiv.appendChild(applyBtn);
            tdRange.appendChild(rangeDiv);

            tr.appendChild(tdLabel);
            tr.appendChild(tdRange);
            table.appendChild(tr);
        });
        pane.appendChild(table);
    }

    return pane;
}

async function loadSettings() {
    try {
        const r = await fetch('/api/config');
        if (!r.ok) return;
        const cfg = await r.json();

        const tabsEl  = document.getElementById('settingsTabs');
        const panesEl = document.getElementById('settingsPanes');
        if (!tabsEl || !panesEl) return;
        tabsEl.innerHTML  = '';
        panesEl.innerHTML = '';

        let first = true;
        for (const { key, label, maxHz } of SENSOR_DEFS) {
            const sensorCfg = cfg.sensors?.[key] || {};
            const thresholds = cfg.thresholds || {};

            // Tab button
            const tab = document.createElement('button');
            tab.className = 'stab' + (first ? ' active' : '');
            tab.textContent = label;
            tab.dataset.target = key;
            tab.addEventListener('click', () => {
                tabsEl.querySelectorAll('.stab').forEach(t => t.classList.remove('active'));
                panesEl.querySelectorAll('.spane').forEach(p => p.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById('spane-' + key)?.classList.add('active');
            });
            tabsEl.appendChild(tab);

            // Pane
            const pane = buildSensorPane(key, sensorCfg, thresholds, maxHz);
            if (first) pane.classList.add('active');
            panesEl.appendChild(pane);

            first = false;
        }
    } catch (e) {
        const panesEl = document.getElementById('settingsPanes');
        if (panesEl) panesEl.innerHTML = '<div class="settings-loading">Failed to load config.</div>';
    }
}

function initSettingsPanel() {
    const hdr  = document.getElementById('settingsHdr');
    const body = document.getElementById('settingsBody');
    const chev = document.getElementById('settingsChevron');
    if (!hdr || !body) return;

    hdr.addEventListener('click', () => {
        const collapsed = body.style.display === 'none';
        body.style.display = collapsed ? '' : 'none';
        chev.classList.toggle('closed', !collapsed);
    });
}

window.addEventListener('DOMContentLoaded', () => {
    initCharts();
    checkHealth();
    setInterval(checkHealth, 10000);
    startSensorStream();
    loadModeStatus();
    loadTranslatorStatus();
    setInterval(loadTranslatorStatus, 30000);
    setInterval(() => { if (latestSensors) scheduleChartUpdate(); }, 1000);
    initExampleChips();
    initSettingsPanel();
    loadSettings();
    input.focus();
});