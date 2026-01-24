/**
 * @file ble-service.js
 * @brief Web Bluetooth API abstraction for ESP32-C6 Gateway
 */

class BLEGateway {
    constructor() {
        this.device = null;
        this.server = null;
        this.service = null;
        this.characteristics = {};
        this.responseCallbacks = [];
        this.commandQueue = [];
        this.commandInProgress = false;
    }

    /**
     * Connect to ESP32-C6 Gateway via Bluetooth
     */
    async connect() {
        try {
            console.log('Requesting Bluetooth device...');

            this.device = await navigator.bluetooth.requestDevice({
                filters: [{ name: 'ESP32C6_Gateway' }],
                optionalServices: ['0000fff0-0000-1000-8000-00805f9b34fb']
            });

            console.log('Connecting to GATT server...');
            this.server = await this.device.gatt.connect();

            console.log('Getting primary service...');
            this.service = await this.server.getPrimaryService('0000fff0-0000-1000-8000-00805f9b34fb');

            // Get all characteristics
            console.log('Getting characteristics...');
            this.characteristics.cmdReq = await this.service.getCharacteristic('0000fff1-0000-1000-8000-00805f9b34fb');
            this.characteristics.cmdRes = await this.service.getCharacteristic('0000fff2-0000-1000-8000-00805f9b34fb');
            this.characteristics.deviceList = await this.service.getCharacteristic('0000fff3-0000-1000-8000-00805f9b34fb');
            this.characteristics.status = await this.service.getCharacteristic('0000fff4-0000-1000-8000-00805f9b34fb');

            // Start notifications
            console.log('Starting notifications...');
            await this.characteristics.cmdRes.startNotifications();
            await this.characteristics.deviceList.startNotifications();
            await this.characteristics.status.startNotifications();

            // Setup notification handlers
            this.characteristics.cmdRes.addEventListener('characteristicvaluechanged',
                this._handleResponseNotification.bind(this));
            this.characteristics.deviceList.addEventListener('characteristicvaluechanged',
                this._handleDeviceListNotification.bind(this));
            this.characteristics.status.addEventListener('characteristicvaluechanged',
                this._handleStatusNotification.bind(this));

            // Setup disconnect handler
            this.device.addEventListener('gattserverdisconnected', this._handleDisconnect.bind(this));

            console.log('BLE connection established');
            return true;
        } catch (error) {
            console.error('BLE connection failed:', error);
            throw error;
        }
    }

    /**
     * Disconnect from device
     */
    disconnect() {
        if (this.device && this.device.gatt.connected) {
            this.device.gatt.disconnect();
        }
    }

    /**
     * Check if connected
     */
    isConnected() {
        return this.device && this.server && this.server.connected;
    }

    /**
     * Send command to ESP32 (with queuing to prevent GATT conflicts)
     */
    async sendCommand(cmd, params = {}) {
        if (!this.isConnected()) {
            throw new Error('Not connected to device');
        }

        // Add command to queue
        return new Promise((resolve, reject) => {
            this.commandQueue.push({ cmd, params, resolve, reject });
            this._processCommandQueue();
        });
    }

    /**
     * Process command queue sequentially
     */
    async _processCommandQueue() {
        // If already processing, just return
        if (this.commandInProgress || this.commandQueue.length === 0) {
            return;
        }

        this.commandInProgress = true;

        while (this.commandQueue.length > 0) {
            const { cmd, params, resolve, reject } = this.commandQueue.shift();

            try {
                const payload = JSON.stringify({ cmd, params });
                console.log('Sending command:', payload);

                const encoder = new TextEncoder();
                const data = encoder.encode(payload);

                // Write to characteristic
                await this.characteristics.cmdReq.writeValue(data);

                // Wait for response
                const response = await new Promise((resolveResp, rejectResp) => {
                    const timeout = setTimeout(() => {
                        rejectResp(new Error('Command timeout'));
                    }, 10000);

                    this.responseCallbacks.push({ resolve: resolveResp, reject: rejectResp, timeout });
                });

                resolve(response);
            } catch (error) {
                console.error('Command error:', error);
                reject(error);
            }

            // Small delay between commands to prevent GATT conflicts
            await new Promise(r => setTimeout(r, 100));
        }

        this.commandInProgress = false;
    }

    /**
     * Handle response notification
     */
    _handleResponseNotification(event) {
        const decoder = new TextDecoder();
        const value = decoder.decode(event.target.value);
        console.log('Received response:', value);

        try {
            const response = JSON.parse(value);

            // Resolve waiting promise
            if (this.responseCallbacks.length > 0) {
                const callback = this.responseCallbacks.shift();
                clearTimeout(callback.timeout);
                callback.resolve(response);
            }
        } catch (error) {
            console.error('Failed to parse response:', error);
            if (this.responseCallbacks.length > 0) {
                const callback = this.responseCallbacks.shift();
                clearTimeout(callback.timeout);
                callback.reject(error);
            }
        }
    }

    /**
     * Handle device list notification
     */
    _handleDeviceListNotification(event) {
        const decoder = new TextDecoder();
        const value = decoder.decode(event.target.value);
        console.log('Device list update:', value);

        // Trigger custom event for UI update
        window.dispatchEvent(new CustomEvent('ble-devices-update', { detail: value }));
    }

    /**
     * Handle status notification
     */
    _handleStatusNotification(event) {
        const decoder = new TextDecoder();
        const value = decoder.decode(event.target.value);
        console.log('Status update:', value);

        // Trigger custom event for UI update
        window.dispatchEvent(new CustomEvent('ble-status-update', { detail: value }));
    }

    /**
     * Handle disconnection
     */
    _handleDisconnect() {
        console.log('BLE device disconnected');
        window.dispatchEvent(new CustomEvent('ble-disconnected'));
    }
}

// Export for use in other scripts
window.BLEGateway = BLEGateway;
