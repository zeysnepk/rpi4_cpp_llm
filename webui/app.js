const chat = document.getElementById('chat');
const form = document.getElementById('form');
const input = document.getElementById('input');
const sendBtn = document.getElementById('send');
const statusEl = document.getElementById('status');

const SYSTEM_PROMPT = "Sen yardımcı, net ve doğru bilgi veren bir Türkçe asistansın. " +
                      "Bilmediğin şeyleri uydurma, kısa ve öz cevap ver.";

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
    const assistantBubble = addMessage('assistant', '');
    assistantBubble.textContent = '...';

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
        let assistantText = '';
        let firstToken = true;

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
                        if (firstToken) { assistantBubble.textContent = ''; firstToken = false; }
                        assistantText += delta;
                        assistantBubble.textContent = assistantText;
                        chat.scrollTop = chat.scrollHeight;
                    }
                } catch { /* parse error - normalde SSE arası boşluklar */ }
            }
        }
        history.push({ role: 'assistant', content: assistantText });
    } catch (err) {
        assistantBubble.parentElement.classList.remove('assistant');
        assistantBubble.parentElement.classList.add('error');
        assistantBubble.textContent = `Hata: ${err.message}`;
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
    input.style.height = Math.min(input.scrollHeight, 180) + 'px';
});

checkHealth();
setInterval(checkHealth, 10000);
input.focus();