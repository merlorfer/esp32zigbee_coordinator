/**
 * proxy-tools.js - Config export/import panel injected by the USB proxy only.
 *
 * This file is served by proxy.py and appended to the page after the normal
 * web/ frontend loads. It never ships on the device itself (the feature only
 * works over the serial link the proxy owns), so it deliberately lives here
 * instead of in web/index.html.
 *
 * Talks to two small proxy-only endpoints (not part of the ESP32's own HTTP
 * API): GET/POST /api/proxy/rules.txt and /api/proxy/config.json. The proxy
 * translates those into the device's export_config/import_config/upload_*
 * serial commands (see main/ble_handlers.c).
 */
(function () {
    'use strict';

    const style = document.createElement('style');
    style.textContent = `
        #proxy-tools-panel {
            position: fixed;
            right: 16px;
            bottom: 16px;
            z-index: 99999;
            font-family: -apple-system, Segoe UI, Roboto, sans-serif;
            font-size: 13px;
        }
        #proxy-tools-toggle {
            background: #2c3e50;
            color: #fff;
            border: none;
            border-radius: 20px;
            padding: 8px 14px;
            cursor: pointer;
            box-shadow: 0 2px 8px rgba(0,0,0,0.3);
        }
        #proxy-tools-body {
            display: none;
            margin-top: 8px;
            background: #fff;
            border: 1px solid #ccc;
            border-radius: 8px;
            padding: 12px;
            width: 240px;
            box-shadow: 0 4px 16px rgba(0,0,0,0.25);
        }
        #proxy-tools-body.open { display: block; }
        #proxy-tools-body h4 { margin: 0 0 8px 0; color: #2c3e50; }
        #proxy-tools-body button {
            display: block;
            width: 100%;
            margin-bottom: 6px;
            padding: 6px 8px;
            border: 1px solid #ccc;
            border-radius: 4px;
            background: #f5f5f5;
            cursor: pointer;
            text-align: left;
        }
        #proxy-tools-body button:hover { background: #e8e8e8; }
        #proxy-tools-status {
            margin-top: 6px;
            font-size: 12px;
            color: #555;
            white-space: pre-wrap;
            word-break: break-word;
        }
        #proxy-tools-status.error { color: #c0392b; }
    `;
    document.head.appendChild(style);

    const panel = document.createElement('div');
    panel.id = 'proxy-tools-panel';
    panel.innerHTML = `
        <button id="proxy-tools-toggle">&#9881; Config tools</button>
        <div id="proxy-tools-body">
            <h4>Config export / import</h4>
            <button id="pt-export-rules">Export rules -&gt; rules.txt</button>
            <button id="pt-import-rules">Import rules from file...</button>
            <button id="pt-export-config">Export config -&gt; config.json</button>
            <button id="pt-import-config">Import config from file...</button>
            <input type="file" id="pt-file-input" style="display:none">
            <div id="proxy-tools-status"></div>
        </div>
    `;
    document.body.appendChild(panel);

    const toggleBtn = document.getElementById('proxy-tools-toggle');
    const body = document.getElementById('proxy-tools-body');
    const statusEl = document.getElementById('proxy-tools-status');
    const fileInput = document.getElementById('pt-file-input');

    toggleBtn.addEventListener('click', () => body.classList.toggle('open'));

    function setStatus(msg, isError) {
        statusEl.textContent = msg;
        statusEl.classList.toggle('error', !!isError);
    }

    function downloadBlob(filename, mime, text) {
        const blob = new Blob([text], { type: mime });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
    }

    async function exportFile(endpoint, filename, mime) {
        setStatus('Exporting...');
        try {
            const res = await fetch(endpoint);
            if (!res.ok) throw new Error('HTTP ' + res.status);
            const text = await res.text();
            downloadBlob(filename, mime, text);
            setStatus('Saved ' + filename);
        } catch (e) {
            setStatus('Export failed: ' + e.message, true);
        }
    }

    function pickFileThen(onText) {
        fileInput.value = '';
        fileInput.onchange = () => {
            const file = fileInput.files[0];
            if (!file) return;
            const reader = new FileReader();
            reader.onload = () => onText(reader.result);
            reader.onerror = () => setStatus('Could not read file', true);
            reader.readAsText(file);
        };
        fileInput.click();
    }

    async function importFile(endpoint, contentType, text, label) {
        setStatus('Importing ' + label + ' (this can take a few seconds for large files)...');
        try {
            const res = await fetch(endpoint, {
                method: 'POST',
                headers: { 'Content-Type': contentType },
                body: text
            });
            const data = await res.json();
            if (data.success) {
                setStatus(label + ' imported successfully.' +
                    (data.devices_imported !== undefined ? ' Devices: ' + data.devices_imported : '') +
                    (data.rule_count !== undefined ? ' Rules: ' + data.rule_count : ''));
            } else {
                setStatus('Import failed: ' + (data.message || 'unknown error'), true);
            }
        } catch (e) {
            setStatus('Import failed: ' + e.message, true);
        }
    }

    document.getElementById('pt-export-rules').addEventListener('click', () => {
        exportFile('/api/proxy/rules.txt', 'rules.txt', 'text/plain');
    });

    document.getElementById('pt-export-config').addEventListener('click', () => {
        exportFile('/api/proxy/config.json', 'config.json', 'application/json');
    });

    document.getElementById('pt-import-rules').addEventListener('click', () => {
        pickFileThen((text) => importFile('/api/proxy/rules.txt', 'text/plain', text, 'Rules'));
    });

    document.getElementById('pt-import-config').addEventListener('click', () => {
        pickFileThen((text) => importFile('/api/proxy/config.json', 'application/json', text, 'Config'));
    });
})();
