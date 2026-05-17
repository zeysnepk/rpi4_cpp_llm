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

window.addEventListener('DOMContentLoaded', () => {
    initCharts();
    checkHealth();
    setInterval(checkHealth, 10000);
    startSensorStream();
    loadModeStatus();
    loadTranslatorStatus();
    setInterval(loadTranslatorStatus, 30000);
    setInterval(() => { if (latestSensors) scheduleChartUpdate(); }, 1000);
    input.focus();
});