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
                        chat.scrollTop = chat.scrollHeight;
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
// GRAFIKLER (Chart.js)
// ============================================================
const MAX_POINTS = 60;
const charts = {};

function commonOptions(yTitle, dualAxis = false, y1Title = '') {
    const scales = {
        x: {
            ticks: { color: '#6e7681', font: { size: 9 }, maxTicksLimit: 8 },
            grid: { color: 'rgba(48,54,61,0.4)' }
        },
        y: {
            position: 'left',
            ticks: { color: '#8b949e', font: { size: 10 } },
            title: { display: !!yTitle, text: yTitle, color: '#8b949e', font: { size: 10 } },
            grid: { color: 'rgba(48,54,61,0.4)' }
        }
    };
    if (dualAxis) {
        scales.y1 = {
            position: 'right',
            ticks: { color: '#8b949e', font: { size: 10 } },
            title: { display: !!y1Title, text: y1Title, color: '#8b949e', font: { size: 10 } },
            grid: { drawOnChartArea: false }
        };
    }
    return {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { intersect: false, mode: 'index' },
        scales,
        plugins: {
            legend: {
                labels: { color: '#c9d1d9', font: { size: 10 }, boxWidth: 12, padding: 8 },
                position: 'top',
                align: 'end'
            },
            tooltip: { backgroundColor: '#21262d', titleColor: '#c9d1d9', bodyColor: '#c9d1d9' }
        }
    };
}

function lineDataset(label, color, yAxisID = 'y') {
    return {
        label, data: [], borderColor: color, backgroundColor: color + '20',
        borderWidth: 1.5, tension: 0.25, pointRadius: 0, yAxisID
    };
}

function initCharts() {
    // BME280: temp/humidity ayri eksen, pressure ayri (3 eksen yerine 2 + gizli)
    charts.bme280 = new Chart(document.getElementById('chart-bme280'), {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                lineDataset('Sıcaklık (°C)',  '#f97316', 'y'),
                lineDataset('Nem (%)',         '#06b6d4', 'y1'),
                lineDataset('Basınç (hPa)',    '#a78bfa', 'y')
            ]
        },
        options: commonOptions('°C / hPa', true, '%')
    });

    // MPU6500: ivme 3 eksen (sol y, g birimi) + gyro magnitude (sag y1, dps)
    charts.mpu6050 = new Chart(document.getElementById('chart-mpu6050'), {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                lineDataset('aX (g)',  '#ef4444', 'y'),
                lineDataset('aY (g)',  '#22c55e', 'y'),
                lineDataset('aZ (g)',  '#3b82f6', 'y'),
                lineDataset('|gyro| (°/s)', '#f59e0b', 'y1')
            ]
        },
        options: commonOptions('g', true, '°/s')
    });

    // QMC5883L: mag 3 eksen (sol y, G) + heading (sag y1, derece)
    charts.qmc5883l = new Chart(document.getElementById('chart-qmc5883l'), {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                lineDataset('mX (G)',  '#ef4444', 'y'),
                lineDataset('mY (G)',  '#22c55e', 'y'),
                lineDataset('mZ (G)',  '#3b82f6', 'y'),
                lineDataset('Heading (°)', '#f59e0b', 'y1')
            ]
        },
        options: commonOptions('G', true, '°')
    });
}

function pushPoint(chartName, label, values) {
    const c = charts[chartName];
    if (!c) return;
    c.data.labels.push(label);
    c.data.datasets.forEach((ds, i) => ds.data.push(values[i]));
    while (c.data.labels.length > MAX_POINTS) {
        c.data.labels.shift();
        c.data.datasets.forEach(ds => ds.data.shift());
    }
    c.update('none');
}

function timeLabel(ms) {
    const t = new Date(ms || Date.now());
    return `${String(t.getMinutes()).padStart(2,'0')}:${String(t.getSeconds()).padStart(2,'0')}`;
}

function gyroMag(g) {
    return Math.sqrt(g.x*g.x + g.y*g.y + g.z*g.z);
}

// ============================================================
// SENSOR POLLING (kart + grafik birlikte)
// ============================================================
async function pollSensors() {
    try {
        const r = await fetch('/api/sensors');
        if (!r.ok) return;
        const all = await r.json();
        const label = timeLabel();

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

                // Grafige veri ekle
                const d = info.data;
                if (name === 'bme280') {
                    pushPoint('bme280', label,
                        [d.temperature_c, d.humidity_pct, d.pressure_hpa]);
                } else if (name === 'mpu6050') {
                    pushPoint('mpu6050', label,
                        [d.accel_g.x, d.accel_g.y, d.accel_g.z, gyroMag(d.gyro_dps)]);
                } else if (name === 'qmc5883l') {
                    pushPoint('qmc5883l', label,
                        [d.mag_g.x, d.mag_g.y, d.mag_g.z, d.heading_deg]);
                }
            } else {
                card.classList.remove('online'); card.classList.add('offline');
                card.querySelector('.readings').textContent = 'Offline';
            }
        });
    } catch { /* ignore */ }
}

// ============================================================
// BASLATMA
// ============================================================
window.addEventListener('DOMContentLoaded', () => {
    initCharts();
    checkHealth();
    setInterval(checkHealth, 10000);
    pollSensors();
    setInterval(pollSensors, 1000);
    input.focus();
});