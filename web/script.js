// ESP32-C6 Zigbee Gateway - Frontend JavaScript

// ============================================================================
// Global State
// ============================================================================

let devices = [];
let permitJoinTimer = null;
let clockInterval = null;
let currentEditDevice = null;
let espTimeOffset = 0;  // Offset between ESP RTC and local time
let espTimeInitialized = false;
let zigbeeActive = false;

// ============================================================================
// Initialization
// ============================================================================

document.addEventListener('DOMContentLoaded', function() {
    loadStatus();
    loadDevices();
    loadGlobalConfig();
    startClock();

    // Refresh data periodically
    setInterval(loadStatus, 5000);
    setInterval(loadDevices, 10000);
});

// ============================================================================
// Clock Functions
// ============================================================================

function startClock() {
    updateClock();
    clockInterval = setInterval(updateClock, 1000);
}

function updateClock() {
    const now = new Date();

    // Host time
    document.getElementById('current-time').textContent =
        now.toLocaleTimeString('hu-HU', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    document.getElementById('current-date').textContent =
        now.toLocaleDateString('hu-HU', { year: 'numeric', month: '2-digit', day: '2-digit' });

    // ESP time (calculated from offset)
    if (espTimeInitialized) {
        const espTime = new Date(now.getTime() + espTimeOffset);
        document.getElementById('esp-time').textContent =
            espTime.toLocaleTimeString('hu-HU', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
        document.getElementById('esp-date').textContent =
            espTime.toLocaleDateString('hu-HU', { year: 'numeric', month: '2-digit', day: '2-digit' });
    }
}

// ============================================================================
// API Functions
// ============================================================================

async function loadStatus() {
    try {
        const response = await fetch('/api/status');
        const data = await response.json();

        const statusDot = document.getElementById('status-indicator');
        const statusText = document.getElementById('status-text');
        const rtcStatus = document.getElementById('rtc-status');

        statusDot.className = 'status-dot connected';
        statusText.textContent = 'Csatlakozva';

        // Update ESP time offset
        if (data.current_time) {
            const espDate = new Date(data.current_time.replace(' ', 'T'));
            const now = new Date();
            espTimeOffset = espDate.getTime() - now.getTime();
            espTimeInitialized = true;
        }

        // Update Zigbee status
        zigbeeActive = data.zigbee_active || false;

        if (data.rtc_initialized) {
            rtcStatus.className = 'rtc-status ok';
            rtcStatus.textContent = 'RTC beallitva';
        } else {
            rtcStatus.className = 'rtc-status warning';
            rtcStatus.textContent = 'RTC nincs beallitva - kerem allitsa be az idot!';
        }
    } catch (error) {
        console.error('Status load error:', error);
        document.getElementById('status-indicator').className = 'status-dot error';
        document.getElementById('status-text').textContent = 'Kapcsolat hiba';
    }
}

async function setRtc() {
    const input = document.getElementById('datetime-input');
    if (!input.value) {
        showToast('Kerem valasszon idopontot!', true);
        return;
    }

    const datetime = input.value.replace('T', ' ') + ':00';

    try {
        const response = await fetch('/api/rtc/set', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ datetime: datetime })
        });

        const data = await response.json();
        if (data.success) {
            showToast('Ora sikeresen beallitva!');
            // Reset ESP time to show immediately
            espTimeOffset = 0;
            espTimeInitialized = false;
            loadStatus();
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('RTC set error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function syncRtc() {
    const now = new Date();
    const year = now.getFullYear();
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const day = String(now.getDate()).padStart(2, '0');
    const hour = String(now.getHours()).padStart(2, '0');
    const minute = String(now.getMinutes()).padStart(2, '0');
    const second = String(now.getSeconds()).padStart(2, '0');

    const datetime = `${year}-${month}-${day} ${hour}:${minute}:${second}`;

    try {
        const response = await fetch('/api/rtc/set', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ datetime: datetime })
        });

        const data = await response.json();
        if (data.success) {
            showToast('Ora szinkronizalva!');
            espTimeOffset = 0;  // After sync, offset should be ~0
            espTimeInitialized = true;
            loadStatus();
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('RTC sync error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function loadDevices() {
    try {
        const response = await fetch('/api/devices');
        const data = await response.json();
        devices = data.devices || [];
        renderDevices();
        updateAutomationButton();
    } catch (error) {
        console.error('Device load error:', error);
    }
}

function updateAutomationButton() {
    const btn = document.getElementById('automation-btn');
    const hint = document.getElementById('automation-hint');

    if (devices.length === 0) {
        btn.disabled = true;
        btn.classList.add('btn-disabled');
        hint.textContent = 'Nincs csatlakozott eszkoz - nem indithato az automatizacio';
        hint.classList.add('warning');
    } else {
        btn.disabled = false;
        btn.classList.remove('btn-disabled');
        hint.textContent = 'A Wi-Fi AP leall es az automatizacio aktivalodik';
        hint.classList.remove('warning');
    }
}

async function permitJoin() {
    // Warn user if Zigbee is not active
    if (!zigbeeActive) {
        showToast('Figyelmeztetes: Zigbee mod nem aktiv! Elobb valtson Zigbee modba a gomb megnyomasaval.', true);
        return;
    }

    try {
        const response = await fetch('/api/zigbee/permit-join', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ duration: 60 })
        });

        const data = await response.json();
        if (data.success) {
            showToast('Eszkoz hozzaadas engedelyezve 60 masodpercig');
            startPermitJoinTimer(60);
        }
    } catch (error) {
        console.error('Permit join error:', error);
        showToast('Hiba tortent', true);
    }
}

function startPermitJoinTimer(seconds) {
    const timerEl = document.getElementById('permit-join-timer');
    const btn = document.getElementById('permit-join-btn');

    timerEl.classList.remove('hidden');
    btn.disabled = true;

    let remaining = seconds;
    timerEl.textContent = remaining + 's';

    if (permitJoinTimer) {
        clearInterval(permitJoinTimer);
    }

    permitJoinTimer = setInterval(() => {
        remaining--;
        timerEl.textContent = remaining + 's';

        if (remaining <= 0) {
            clearInterval(permitJoinTimer);
            timerEl.classList.add('hidden');
            btn.disabled = false;
            loadDevices();
        }
    }, 1000);
}

async function deleteDevice(ieeeAddr) {
    if (!confirm('Biztosan torolni szeretne ezt az eszkozt?')) {
        return;
    }

    try {
        const response = await fetch('/api/devices/' + ieeeAddr, {
            method: 'DELETE'
        });

        const data = await response.json();
        if (data.success) {
            showToast('Eszkoz torolve');
            loadDevices();
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('Delete error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function saveDeviceConfig() {
    const ieeeAddr = document.getElementById('edit-ieee-addr').value;
    const mode = document.getElementById('edit-mode').value;

    const config = {
        custom_name: document.getElementById('edit-name').value,
        enabled: document.getElementById('edit-enabled').checked,
        mode: mode
    };

    if (mode === 'fixed_time') {
        config.time_pairs = getTimePairs();
    } else {
        config.delay_on_minutes = parseInt(document.getElementById('edit-delay-on').value);
        config.delay_duration_minutes = parseInt(document.getElementById('edit-delay-duration').value);
    }

    try {
        const response = await fetch('/api/devices/' + ieeeAddr + '/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(config)
        });

        const data = await response.json();
        if (data.success) {
            showToast('Beallitasok mentve');
            closeModal();
            loadDevices();
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('Save error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function loadGlobalConfig() {
    try {
        const response = await fetch('/api/config');
        const data = await response.json();

        if (data.wifi_on_behavior) {
            document.getElementById('wifi-maintain').checked = true;
        } else {
            document.getElementById('wifi-poweroff').checked = true;
        }
    } catch (error) {
        console.error('Config load error:', error);
    }
}

async function saveGlobalConfig() {
    const maintain = document.getElementById('wifi-maintain').checked;

    try {
        const response = await fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ wifi_on_behavior: maintain })
        });

        const data = await response.json();
        if (data.success) {
            showToast('Beallitasok mentve');
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('Save config error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function startAutomation() {
    // Check if there are devices
    if (devices.length === 0) {
        showToast('Nincs csatlakozott eszkoz - nem indithato az automatizacio!', true);
        return;
    }

    if (!confirm('A Wi-Fi AP leall es az automatizacio aktivalodik. Folytatja?')) {
        return;
    }

    const maintain = document.getElementById('wifi-maintain').checked;

    try {
        const response = await fetch('/api/wifi/shutdown', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                wifi_on_behavior: maintain ? 'maintain_state' : 'power_off'
            })
        });

        const data = await response.json();
        if (data.success) {
            showToast('Automatizacio aktivalva - a kapcsolat hamarosan megszakad');
        }
    } catch (error) {
        console.error('Start automation error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

// ============================================================================
// Device Rendering
// ============================================================================

function renderDevices() {
    const container = document.getElementById('device-list');

    if (devices.length === 0) {
        container.innerHTML = '<p class="no-devices">Nincs csatlakozott eszkoz</p>';
        return;
    }

    container.innerHTML = devices.map(device => `
        <div class="device-item">
            <div class="device-info">
                <div class="device-name">${escapeHtml(device.custom_name)}</div>
                <div class="device-manufacturer">${escapeHtml(device.manufacturer || 'Ismeretlen gyarto')}</div>
                <div class="device-model">${escapeHtml(device.model || 'Ismeretlen model')}</div>
                <div class="device-addr">${device.ieee_addr} | EP: ${device.endpoint}</div>
                <div class="device-status">
                    <span class="state-badge ${device.current_state ? 'state-on' : 'state-off'}">
                        ${device.current_state ? 'ON' : 'OFF'}
                    </span>
                    ${device.error ? `
                        <span class="error-badge">
                            &#9888; ${escapeHtml(device.error.message)} (${device.error.timestamp})
                        </span>
                    ` : ''}
                </div>
            </div>
            <div class="device-actions">
                <button onclick="editDevice('${device.ieee_addr}')" class="btn btn-primary btn-small">
                    Szerkesztes
                </button>
                <button onclick="deleteDevice('${device.ieee_addr}')" class="btn btn-danger btn-small">
                    Torles
                </button>
            </div>
        </div>
    `).join('');
}

// ============================================================================
// Modal Functions
// ============================================================================

function editDevice(ieeeAddr) {
    const device = devices.find(d => d.ieee_addr === ieeeAddr);
    if (!device) return;

    currentEditDevice = device;
    document.getElementById('edit-ieee-addr').value = ieeeAddr;
    document.getElementById('edit-name').value = device.custom_name || '';
    document.getElementById('edit-enabled').checked = device.enabled;
    document.getElementById('edit-mode').value = device.mode;

    // Set delay values
    document.getElementById('edit-delay-on').value = device.delay_on_minutes || 30;
    document.getElementById('edit-delay-duration').value = device.delay_duration_minutes || 120;

    // Set time pairs
    renderTimePairs(device.time_pairs || [{ on: '06:00', off: '18:00' }]);

    onModeChange();
    document.getElementById('device-modal').classList.remove('hidden');
}

function closeModal() {
    document.getElementById('device-modal').classList.add('hidden');
    currentEditDevice = null;
}

function onModeChange() {
    const mode = document.getElementById('edit-mode').value;
    const fixedSettings = document.getElementById('fixed-time-settings');
    const delaySettings = document.getElementById('delay-settings');

    if (mode === 'fixed_time') {
        fixedSettings.classList.remove('hidden');
        delaySettings.classList.add('hidden');
    } else {
        fixedSettings.classList.add('hidden');
        delaySettings.classList.remove('hidden');
    }
}

// ============================================================================
// Time Pair Functions
// ============================================================================

function renderTimePairs(pairs) {
    const container = document.getElementById('time-pairs-container');
    container.innerHTML = pairs.map((pair, index) => `
        <div class="time-pair" data-index="${index}">
            <label>ON:</label>
            <input type="time" class="time-on" value="${pair.on}">
            <label>OFF:</label>
            <input type="time" class="time-off" value="${pair.off}">
            ${index > 0 ? `<button type="button" class="remove-btn" onclick="removeTimePair(${index})">&times;</button>` : ''}
        </div>
    `).join('');
}

function addTimePair() {
    const container = document.getElementById('time-pairs-container');
    const currentCount = container.children.length;

    if (currentCount >= 5) {
        showToast('Maximum 5 idopont par adhato meg', true);
        return;
    }

    const div = document.createElement('div');
    div.className = 'time-pair';
    div.dataset.index = currentCount;
    div.innerHTML = `
        <label>ON:</label>
        <input type="time" class="time-on" value="08:00">
        <label>OFF:</label>
        <input type="time" class="time-off" value="20:00">
        <button type="button" class="remove-btn" onclick="removeTimePair(${currentCount})">&times;</button>
    `;
    container.appendChild(div);
}

function removeTimePair(index) {
    const container = document.getElementById('time-pairs-container');
    const pairs = Array.from(container.children);

    if (pairs.length <= 1) return;

    pairs[index].remove();

    // Re-index remaining pairs
    Array.from(container.children).forEach((pair, i) => {
        pair.dataset.index = i;
        const removeBtn = pair.querySelector('.remove-btn');
        if (removeBtn) {
            removeBtn.onclick = () => removeTimePair(i);
        }
    });
}

function getTimePairs() {
    const container = document.getElementById('time-pairs-container');
    const pairs = [];

    container.querySelectorAll('.time-pair').forEach(pair => {
        const on = pair.querySelector('.time-on').value;
        const off = pair.querySelector('.time-off').value;
        if (on && off) {
            pairs.push({ on, off });
        }
    });

    return pairs;
}

// ============================================================================
// Utility Functions
// ============================================================================

function showToast(message, isError = false) {
    const toast = document.getElementById('toast');
    toast.textContent = message;
    toast.className = 'toast' + (isError ? ' error' : '');

    setTimeout(() => {
        toast.classList.add('hidden');
    }, 3000);
}

function escapeHtml(text) {
    if (!text) return '';
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Close modal on escape key
document.addEventListener('keydown', function(e) {
    if (e.key === 'Escape') {
        closeModal();
    }
});

// Close modal on outside click
document.getElementById('device-modal').addEventListener('click', function(e) {
    if (e.target === this) {
        closeModal();
    }
});
