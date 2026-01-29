// ESP32-C6 Zigbee Gateway - Frontend JavaScript

// ============================================================================
// Global State
// ============================================================================

let devices = [];
// permitJoinTimer removed - pairing is done via physical button
let clockInterval = null;
let currentEditDevice = null;
let espTimeOffset = 0;  // Offset between ESP RTC and local time
let espTimeInitialized = false;
let zigbeeActive = false;

// BLE State
let bleGateway = null;
let bleConnected = false;

// Polling intervals
let statusInterval = null;
let devicesInterval = null;

// Helper function to check if we should poll via WiFi
function shouldPollViaWiFi() {
    const shouldPoll = !bleConnected;
    if (!shouldPoll) {
        console.log('Skipping WiFi poll: BLE mode active');
    }
    return shouldPoll;
}

// ============================================================================
// Initialization
// ============================================================================

document.addEventListener('DOMContentLoaded', function() {
    // Initial load - these will use WiFi by default
    loadStatus();
    loadDevices();
    loadGlobalConfig();
    startClock();

    // Refresh data periodically (only for WiFi mode)
    statusInterval = setInterval(() => {
        if (shouldPollViaWiFi()) {
            loadStatus();
        }
    }, 5000);

    devicesInterval = setInterval(() => {
        if (shouldPollViaWiFi()) {
            loadDevices();
        }
    }, 10000);
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
// BLE Connection Management
// ============================================================================

function updateBLEStatus(connected, statusMessage) {
    // Update main BLE status
    const indicator = document.getElementById('ble-indicator');
    const statusText = document.getElementById('ble-text');
    const connectBtn = document.getElementById('ble-connect-btn');
    const disconnectBtn = document.getElementById('ble-disconnect-btn');

    if (indicator) {
        indicator.className = connected ? 'status-dot connected' : 'status-dot disconnected';
    }
    if (statusText) {
        statusText.textContent = statusMessage;
    }
    if (connectBtn) {
        if (connected) {
            connectBtn.classList.add('hidden');
        } else {
            connectBtn.classList.remove('hidden');
        }
    }
    if (disconnectBtn) {
        if (connected) {
            disconnectBtn.classList.remove('hidden');
        } else {
            disconnectBtn.classList.add('hidden');
        }
    }

    // Update modal BLE status
    const modalIndicator = document.getElementById('modal-ble-indicator');
    const modalStatusText = document.getElementById('modal-ble-text');
    const modalConnectBtn = document.getElementById('modal-ble-connect-btn');
    const modalDisconnectBtn = document.getElementById('modal-ble-disconnect-btn');

    if (modalIndicator) {
        modalIndicator.className = connected ? 'connected' : 'disconnected';
    }
    if (modalStatusText) {
        modalStatusText.textContent = 'BLE: ' + statusMessage;
    }
    if (modalConnectBtn) {
        if (connected) {
            modalConnectBtn.classList.add('hidden');
        } else {
            modalConnectBtn.classList.remove('hidden');
        }
    }
    if (modalDisconnectBtn) {
        if (connected) {
            modalDisconnectBtn.classList.remove('hidden');
        } else {
            modalDisconnectBtn.classList.add('hidden');
        }
    }
}

async function connectBLE() {
    const connectBtn = document.getElementById('ble-connect-btn');
    const modalConnectBtn = document.getElementById('modal-ble-connect-btn');

    try {
        // Disable connect buttons
        if (connectBtn) connectBtn.disabled = true;
        if (modalConnectBtn) modalConnectBtn.disabled = true;

        updateBLEStatus(false, 'Csatlakozas...');

        // Check browser support
        if (!navigator.bluetooth) {
            showToast('Web Bluetooth nem tamogatott ebben a bongeszoben. Hasznaljon Chrome vagy Edge bongeszo!', true);
            if (connectBtn) connectBtn.disabled = false;
            if (modalConnectBtn) modalConnectBtn.disabled = false;
            updateBLEStatus(false, 'Nincs csatlakozva');
            return;
        }

        // Create BLE gateway if not exists
        if (!bleGateway) {
            bleGateway = new BLEGateway();
        }

        // Connect
        await bleGateway.connect();

        // IMPORTANT: Set flags IMMEDIATELY after successful connection
        bleConnected = true;

        // STOP WiFi polling intervals when switching to BLE mode
        if (statusInterval) {
            clearInterval(statusInterval);
            statusInterval = null;
            console.log('WiFi status polling stopped');
        }
        if (devicesInterval) {
            clearInterval(devicesInterval);
            devicesInterval = null;
            console.log('WiFi devices polling stopped');
        }

        console.log('BLE flags set: bleConnected=', bleConnected);

        updateBLEStatus(true, 'Csatlakozva');
        showToast('Bluetooth kapcsolat letrejott');

        // Load initial data sequentially with delay to avoid GATT conflicts
        // Don't use setTimeout to avoid closure/scope issues
        try {
            await new Promise(resolve => setTimeout(resolve, 500));
            await loadStatus();
            await new Promise(resolve => setTimeout(resolve, 300));
            await loadDevices();
        } catch (err) {
            console.error('Error loading initial data:', err);
            // Don't show error toast - connection is still established
        }

    } catch (error) {
        console.error('BLE connection error:', error);
        showToast('Bluetooth kapcsolat sikertelen: ' + error.message, true);

        // Re-enable connect buttons
        if (connectBtn) connectBtn.disabled = false;
        if (modalConnectBtn) modalConnectBtn.disabled = false;

        updateBLEStatus(false, 'Kapcsolat sikertelen');
        bleConnected = false;
    }
}

function disconnectBLE() {
    if (bleGateway) {
        bleGateway.disconnect();
    }

    bleConnected = false;

    updateBLEStatus(false, 'Nincs csatlakozva');

    // Re-enable connect buttons
    const connectBtn = document.getElementById('ble-connect-btn');
    const modalConnectBtn = document.getElementById('modal-ble-connect-btn');
    if (connectBtn) connectBtn.disabled = false;
    if (modalConnectBtn) modalConnectBtn.disabled = false;

    // RESTART WiFi polling intervals ONLY if they don't exist
    // Clear first to prevent duplicates
    if (statusInterval) {
        clearInterval(statusInterval);
        statusInterval = null;
    }
    statusInterval = setInterval(() => {
        if (shouldPollViaWiFi()) {
            loadStatus();
        }
    }, 5000);
    console.log('WiFi status polling restarted');

    if (devicesInterval) {
        clearInterval(devicesInterval);
        devicesInterval = null;
    }
    devicesInterval = setInterval(() => {
        if (shouldPollViaWiFi()) {
            loadDevices();
        }
    }, 10000);
    console.log('WiFi devices polling restarted');

    showToast('Bluetooth kapcsolat bontva');
}

// Handle BLE disconnect event
window.addEventListener('ble-disconnected', () => {
    showToast('Bluetooth kapcsolat megszakadt', true);
    disconnectBLE();
});

// ============================================================================
// API Helper Functions
// ============================================================================

async function apiRequest(endpoint, method = 'GET', body = null) {
    if (bleConnected) {
        // Use BLE
        return await bleRequest(endpoint, body);
    } else {
        // Use WiFi/HTTP
        return await httpRequest(endpoint, method, body);
    }
}

async function httpRequest(endpoint, method = 'GET', body = null) {

    const options = {
        method: method,
        headers: {
            'Content-Type': 'application/json'
        }
    };

    if (body) {
        options.body = JSON.stringify(body);
    }

    const response = await fetch(endpoint, options);
    return await response.json();
}

async function bleRequest(endpoint, body = null) {
    // Map HTTP endpoints to BLE commands
    const command = endpointToCommand(endpoint, body);

    if (!command) {
        throw new Error('Unknown endpoint: ' + endpoint);
    }

    const response = await bleGateway.sendCommand(command.cmd, command.params);

    // Convert BLE response to HTTP-like format
    if (response.status === 'ok') {
        return { success: true, ...response };
    } else if (response.status === 'error') {
        return { success: false, message: response.message };
    }

    return response;
}

function endpointToCommand(endpoint, body) {
    // Map HTTP REST endpoints to BLE JSON commands
    if (endpoint === '/api/status') {
        return { cmd: 'get_status', params: {} };
    }

    if (endpoint === '/api/devices') {
        return { cmd: 'get_devices', params: {} };
    }

    if (endpoint === '/api/rtc/set') {
        // body contains {datetime: "YYYY-MM-DD HH:MM:SS"}
        const dt = body.datetime.split(/[- :]/);
        return {
            cmd: 'set_rtc',
            params: {
                year: parseInt(dt[0]),
                month: parseInt(dt[1]),
                day: parseInt(dt[2]),
                hour: parseInt(dt[3]),
                minute: parseInt(dt[4]),
                second: parseInt(dt[5])
            }
        };
    }

    if (endpoint.startsWith('/api/devices/') && endpoint.endsWith('/config')) {
        // Device config update
        const ieeeAddr = extractIeeeFromEndpoint(endpoint);
        return {
            cmd: 'set_device_config',
            params: {
                ieee_addr: ieeeAddr,
                ...body
            }
        };
    }

    if (endpoint.startsWith('/api/devices/') && body && body.cmd) {
        // Device control
        return {
            cmd: 'control_device',
            params: body
        };
    }

    if (endpoint.startsWith('/api/devices/') && !endpoint.includes('config')) {
        // Device delete
        return {
            cmd: 'delete_device',
            params: {
                ieee_addr: extractIeeeFromEndpoint(endpoint)
            }
        };
    }

    if (endpoint === '/api/zigbee/permit-join') {
        return {
            cmd: 'permit_join',
            params: body || { duration: 60 }
        };
    }

    if (endpoint === '/api/config') {
        if (body) {
            return {
                cmd: 'set_global_settings',
                params: body
            };
        } else {
            return {
                cmd: 'get_global_settings',
                params: {}
            };
        }
    }

    if (endpoint === '/api/factory-reset') {
        return {
            cmd: 'factory_reset',
            params: {}
        };
    }

    return null;
}

function extractIeeeFromEndpoint(endpoint) {
    const match = endpoint.match(/\/api\/devices\/(0x[0-9A-Fa-f]+)/);
    return match ? match[1] : null;
}

// ============================================================================
// API Functions
// ============================================================================

async function loadStatus() {
    try {
        const data = await apiRequest('/api/status');

        const statusDot = document.getElementById('status-indicator');
        const statusText = document.getElementById('status-text');
        const rtcStatus = document.getElementById('rtc-status');

        statusDot.className = 'status-dot connected';
        statusText.textContent = bleConnected ? 'Csatlakozva (BLE)' : 'Csatlakozva (WiFi)';

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

        // If BLE fails, try to reconnect
        if (bleConnected) {
            disconnectBLE();
        }
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
        const data = await apiRequest('/api/rtc/set', 'POST', { datetime: datetime });

        if (data.success || data.status === 'ok') {
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
        const data = await apiRequest('/api/rtc/set', 'POST', { datetime: datetime });

        if (data.success || data.status === 'ok') {
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
        console.log('loadDevices: Requesting device list...');
        const data = await apiRequest('/api/devices');
        console.log('loadDevices: Received data:', data);
        console.log('loadDevices: Device count:', data.devices ? data.devices.length : 0);
        devices = data.devices || [];
        renderDevices();
    } catch (error) {
        console.error('Device load error:', error);
    }
}

async function refreshDevices() {
    const refreshBtn = document.getElementById('refresh-devices-btn');

    // Disable button during refresh
    if (refreshBtn) {
        refreshBtn.disabled = true;
        refreshBtn.textContent = 'Frissites...';
    }

    try {
        await loadDevices();
        showToast('Eszkoklista frissitve');
    } catch (error) {
        console.error('Refresh error:', error);
        showToast('Frissites sikertelen', true);
    } finally {
        // Re-enable button
        if (refreshBtn) {
            refreshBtn.disabled = false;
            refreshBtn.textContent = 'Frissites';
        }
    }
}

// permitJoin() and startPermitJoinTimer() removed - pairing is done via physical button on ESP

async function deleteDevice(ieeeAddr) {
    if (!confirm('Biztosan torolni szeretne ezt az eszkozt?')) {
        return;
    }

    try {
        const data = await apiRequest('/api/devices/' + ieeeAddr, 'DELETE');

        if (data.success || data.status === 'ok') {
            showToast('Eszkoz torolve');

            // Remove device from local array immediately for instant UI update
            devices = devices.filter(d => d.ieee_addr !== ieeeAddr);
            renderDevices();

            // Also reload from server after a short delay (especially for BLE)
            setTimeout(() => {
                loadDevices();
            }, 500);
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
        // 3-phase delay: OFF1 → ON → OFF2
        config.delay_off1_minutes = parseInt(document.getElementById('edit-delay-off1').value);
        config.delay_duration_minutes = parseInt(document.getElementById('edit-delay-duration').value);
        config.delay_off2_minutes = parseInt(document.getElementById('edit-delay-off2').value);
    }

    try {
        const data = await apiRequest('/api/devices/' + ieeeAddr + '/config', 'POST', config);

        if (data.success || data.status === 'ok') {
            showToast('Beallitasok mentve');
            closeModal();

            // Wait a bit for NVS write to complete before reloading
            await new Promise(resolve => setTimeout(resolve, 500));
            await loadDevices();
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
        const data = await apiRequest('/api/config');

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
        const data = await apiRequest('/api/config', 'POST', { wifi_on_behavior: maintain });

        if (data.success || data.status === 'ok') {
            showToast('Beallitasok mentve');
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('Save config error:', error);
        showToast('Kapcsolati hiba', true);
    }
}

async function factoryReset() {
    if (!confirm('Biztosan torolni szeretne az osszes eszkozt es beallitast? Ez a muvelet nem vonhato vissza!')) {
        return;
    }

    if (!confirm('Utolso figyelmeztetes: Az osszes adat torlodik. Folytatja?')) {
        return;
    }

    try {
        const data = await apiRequest('/api/factory-reset', 'POST');

        if (data.success || data.status === 'ok') {
            showToast('Gyari alaphelyzetbe allitas sikeres');
            setTimeout(() => {
                if (bleConnected) {
                    // If using BLE, just reload the page
                    location.reload();
                } else {
                    // If using WiFi, device might restart
                    location.reload();
                }
            }, 3000);
        } else {
            showToast(data.message || 'Hiba tortent', true);
        }
    } catch (error) {
        console.error('Factory reset error:', error);
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
                    <span class="state-badge ${device.enabled ? 'state-on' : 'state-off'}">
                        ${device.enabled ? 'Automatizacio BE' : 'Automatizacio KI'}
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

    // Set delay values (3-phase)
    // Use explicit undefined check to allow 0 values
    document.getElementById('edit-delay-off1').value = (device.delay_off1_minutes !== undefined) ? device.delay_off1_minutes : 0;
    document.getElementById('edit-delay-duration').value = (device.delay_duration_minutes !== undefined) ? device.delay_duration_minutes : 120;
    document.getElementById('edit-delay-off2').value = (device.delay_off2_minutes !== undefined) ? device.delay_off2_minutes : 30;

    // Set time pairs
    renderTimePairs(device.time_pairs || [{ on: '06:00', off: '18:00' }]);

    onModeChange();
    document.getElementById('device-modal').classList.remove('hidden');

    // Update modal BLE status when opening
    updateBLEStatus(bleConnected, bleConnected ? 'Csatlakozva' : 'Nincs csatlakozva');
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
