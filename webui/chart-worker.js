// Web Worker - grafikleri main thread'den izole eder
self.importScripts('chart.umd.min.js');

const MAX_POINTS = 300;
const charts = {};

function commonOptions(yTitle, dualAxis = false, y1Title = '', extra = {}) {
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
    Object.assign(scales, extra);
    return {
        responsive: false,           // Worker'da DOM yok, kendi resize yapacagiz
        maintainAspectRatio: false,
        animation: false,
        interaction: { intersect: false, mode: 'index' },
        scales,
        plugins: {
            legend: {
                labels: { color: '#c9d1d9', font: { size: 10 }, boxWidth: 12, padding: 8 },
                position: 'top', align: 'end'
            },
            tooltip: { backgroundColor: '#21262d', titleColor: '#c9d1d9', bodyColor: '#c9d1d9' }
        }
    };
}

function ds(label, color, yAxisID = 'y', hidden = false) {
    return {
        label, data: [], borderColor: color, backgroundColor: color + '20',
        borderWidth: 1.5, tension: 0.25, pointRadius: 0, yAxisID, hidden
    };
}

function getConfig(name) {
    if (name === 'bme280') return {
        type: 'line',
        data: { labels: [], datasets: [
            ds('Temperature (°C)', '#f97316', 'y'),
            ds('Humidity (%)',     '#06b6d4', 'y1'),
            ds('Pressure (hPa)',   '#a78bfa', 'yP', true)
        ]},
        options: commonOptions('°C', true, '%', { yP: { display: false, position: 'right' } })
    };
    if (name === 'mpu6050') return {
        type: 'line',
        data: { labels: [], datasets: [
            ds('aX (g)', '#ef4444', 'y'),
            ds('aY (g)', '#22c55e', 'y'),
            ds('aZ (g)', '#3b82f6', 'y'),
            ds('|gyro| (°/s)', '#f59e0b', 'y1')
        ]},
        options: commonOptions('g', true, '°/s')
    };
    if (name === 'qmc5883l') return {
        type: 'line',
        data: { labels: [], datasets: [
            ds('mX (G)', '#ef4444', 'y'),
            ds('mY (G)', '#22c55e', 'y'),
            ds('mZ (G)', '#3b82f6', 'y'),
            ds('Heading (°)', '#f59e0b', 'y1')
        ]},
        options: commonOptions('G', true, '°')
    };
}

self.onmessage = (e) => {
    const m = e.data;
    if (m.type === 'init') {
        charts[m.name] = new Chart(m.canvas, getConfig(m.name));
    }
    else if (m.type === 'update') {
        const c = charts[m.name];
        if (!c) return;
        c.data.labels.push(m.label);
        c.data.datasets.forEach((d, i) => d.data.push(m.values[i]));
        while (c.data.labels.length > MAX_POINTS) {
            c.data.labels.shift();
            c.data.datasets.forEach(d => d.data.shift());
        }
        c.update('none');
    }
    else if (m.type === 'resize') {
        const c = charts[m.name];
        if (!c) return;
        c.canvas.width  = m.width;
        c.canvas.height = m.height;
        c.resize();
    }
};