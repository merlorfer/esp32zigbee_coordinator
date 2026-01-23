/**
 * @file ble-integration.js
 * @brief BLE integration helpers for script.js
 *
 * Add this to the top of script.js or merge these functions into the existing code
 */

// ============================================================================
// BLE Global State
// ============================================================================

let bleGateway = null;
let bleConnected = false;
let useBluetoothMode = false;  // Toggle between WiFi and BLE

// ============================================================================
// BLE Connection Management
// ============================================================================

async function connectBLE() {
    const connectBtn = document.getElementById('ble-connect-btn');
    const disconnectBtn = document.getElementById('ble-disconnect-btn');
    const indicator = document.getElementById('ble-indicator');
    const statusText = document.getElementById('ble-text');

    try {
        connectBtn.disabled = true;
        statusText.textContent = 'Csatlakozas...';

        // Create BLE gateway if not exists
        if (!bleGateway) {
            bleGateway = new BLEGateway();
        }

        // Connect
        await bleGateway.connect();

        // Update UI
        bleConnected = true;
        useBluetoothMode = true;
        indicator.className = 'status-dot connected';
        statusText.textContent = 'Csatlakozva (BLE)';
        connectBtn.classList.add('hidden');
        disconnectBtn.classList.remove('hidden');

        showMessage('Bluetooth kapcsolat letrejott', 'success');

        // Load initial data
        await loadStatus();
        await loadDevices();

    } catch (error) {
        console.error('BLE connection error:', error);
        showMessage('Bluetooth kapcsolat sikertelen: ' + error.message, 'error');
        indicator.className = 'status-dot disconnected';
        statusText.textContent = 'Kapcsolat sikertelen';
        connectBtn.disabled = false;
    }
}

function disconnectBLE() {
    if (bleGateway) {
        bleGateway.disconnect();
    }

    bleConnected = false;
    useBluetoothMode = false;

    const connectBtn = document.getElementById('ble-connect-btn');
    const disconnectBtn = document.getElementById('ble-disconnect-btn');
    const indicator = document.getElementById('ble-indicator');
    const statusText = document.getElementById('ble-text');

    indicator.className = 'status-dot disconnected';
    statusText.textContent = 'Nincs csatlakozva';
    connectBtn.classList.remove('hidden');
    disconnectBtn.classList.add('hidden');
    connectBtn.disabled = false;

    showMessage('Bluetooth kapcsolat bontva', 'info');
}

// Handle BLE disconnect event
window.addEventListener('ble-disconnected', () => {
    showMessage('Bluetooth kapcsolat megszakadt', 'warning');
    disconnectBLE();
});

// ============================================================================
// API Helper Functions (replaces fetch with BLE when needed)
// ============================================================================

async function apiRequest(endpoint, method = 'GET', body = null) {
    if (useBluetoothMode && bleConnected) {
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

    return await bleGateway.sendCommand(command.cmd, command.params);
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
        return {
            cmd: 'set_device_config',
            params: {
                ...body,
                ieee_addr: body.ieee_addr || extractIeeeFromEndpoint(endpoint)
            }
        };
    }

    if (endpoint.startsWith('/api/devices/') && endpoint.includes('control')) {
        // Device control
        return {
            cmd: 'control_device',
            params: body
        };
    }

    if (endpoint.startsWith('/api/devices/') && !endpoint.includes('/')) {
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
        return {
            cmd: 'set_global_settings',
            params: body
        };
    }

    return null;
}

function extractIeeeFromEndpoint(endpoint) {
    const match = endpoint.match(/\/api\/devices\/(0x[0-9A-Fa-f]+)/);
    return match ? match[1] : null;
}

// ============================================================================
// Replace existing loadStatus function
// ============================================================================

async function loadStatusBLE() {
    try {
        const data = await apiRequest('/api/status');

        const statusDot = document.getElementById('status-indicator');
        const statusText = document.getElementById('status-text');
        const rtcStatus = document.getElementById('rtc-status');

        if (data.status === 'ok') {
            statusDot.className = 'status-dot connected';
            statusText.textContent = useBluetoothMode ? 'Csatlakozva (BLE)' : 'Csatlakozva (WiFi)';

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
        } else {
            throw new Error(data.message || 'Unknown error');
        }
    } catch (error) {
        console.error('Status load error:', error);
        document.getElementById('status-indicator').className = 'status-dot error';
        document.getElementById('status-text').textContent = 'Kapcsolat hiba';

        // If BLE fails, try to reconnect
        if (useBluetoothMode && bleConnected) {
            disconnectBLE();
        }
    }
}

// ============================================================================
// Usage Instructions
// ============================================================================

/*
To integrate this into script.js:

1. Add these functions at the top of script.js after the global state section

2. Replace the existing loadStatus function with loadStatusBLE

3. Update all fetch() calls to use apiRequest() instead:
   OLD: const response = await fetch('/api/status');
        const data = await response.json();
   NEW: const data = await apiRequest('/api/status');

4. Update loadDevices similarly:
   OLD: const response = await fetch('/api/devices');
        const data = await response.json();
   NEW: const data = await apiRequest('/api/devices');

5. For POST requests:
   OLD: await fetch('/api/rtc/set', {
           method: 'POST',
           headers: { 'Content-Type': 'application/json' },
           body: JSON.stringify({ datetime: datetimeString })
        });
   NEW: await apiRequest('/api/rtc/set', 'POST', { datetime: datetimeString });

6. The apiRequest function will automatically route through BLE or WiFi
   based on the useBluetoothMode flag.
*/
