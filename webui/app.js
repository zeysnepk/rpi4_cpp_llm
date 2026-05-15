// ============================================================
// CHAT
// ============================================================
const chat = document.getElementById('chat');
const form = document.getElementById('form');
const input = document.getElementById('input');
const sendBtn = document.getElementById('send');
const statusEl = document.getElementById('status');

const SYSTEM_PROMPT = "Sen yardımcı, net ve doğru bilgi veren bir Türkçe asistansın. " +
                      "Sensör verilerini yorumlayabilir, kısa ve öz cevap verirsin.";

const history = [];

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
    bubble.textContent = '...';

    history.push({ role: 'user', content: text });
    const messages = [{ role: 'system', content: SYSTEM_PROMPT }, ...history];

    try {
        const response = await fetch('/api/chat', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ messages, temperature: 0.3, max_tokens: 512 })
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';
        let text = '';
        let first = true;

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
                try {
                    const obj = JSON.parse(data);
                    const delta = obj.choices?.[0]?.delta?.content || '';
                    if (delta) {
                        if (first) { bubble.textContent = ''; first = false; }
                        text += delta;
                        bubble.textContent = text;
                        scheduleScroll();
                    }
                } catch { /* ignore */ }
            }
        }
        history.push({ role: 'assistant', content: text });
    } catch (err) {
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
// SENSOR KARTLARI (sayisal okumalar)
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
// GRAFIKLER - Web Worker'a delege edildi (ayri thread)
// ============================================================
const chartWorker = new Worker('chart-worker.js');

function initCharts() {
    // Kanvasin gercek pixel boyutunu ayarla, sonra worker'a transfer et
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

// Window resize -> worker'a bildir
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
// SENSOR STREAM (SSE - server push)
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

// Chart guncellemesi rAF ile throttle - sunucu 5 Hz pushluyor,
// rAF browser'in refresh rate'ine (60 Hz) hizalar, akici gorunur
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
        console.log('[SSE]', new Date().toISOString().slice(11,23));
        try {
            latestSensors = JSON.parse(event.data);
            updateCards(latestSensors);
            scheduleChartUpdate();
        } catch (e) { /* parse errors */ }
    };

    sse.onerror = () => {
        // EventSource otomatik yeniden baglanir (retry: 2000)
        setStatus('err', 'sensör bağlantısı koptu, deniyor...');
        setTimeout(() => checkHealth(), 1500);
    };

    sse.onopen = () => {
        // healthy state'i checkHealth ayarlar
    };
}

// ============================================================
// BASLATMA
// ============================================================
window.addEventListener('DOMContentLoaded', () => {
    initCharts();
    checkHealth();
    setInterval(checkHealth, 10000);
    startSensorStream();   

    // Guvenlik agi: SSE gecikirse bile son veriden chart'i besle (1 Hz)
    setInterval(() => {
        if (latestSensors) scheduleChartUpdate();
    }, 1000);


    input.focus();
});