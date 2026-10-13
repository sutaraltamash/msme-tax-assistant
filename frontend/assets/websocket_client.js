/**
 * websocket_client.js — Full-duplex WebSocket client wrapper for Tax Assistant.
 *
 * Implements architecture.md §10 message contract:
 *  - Client -> Server: UPLOAD_INVOICE, USER_REPLY, REQUEST_FILING
 *  - Server -> Client: SESSION_INIT, CHAT_MESSAGE, FILING_READY
 */

class TaxWebSocketClient {
    constructor(options = {}) {
        this.url = options.url || this.getDefaultUrl();
        this.socket = null;
        this.sessionId = null;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = options.maxReconnectAttempts || 10;
        this.reconnectDelay = options.reconnectDelay || 2000;
        this.autoReconnect = options.autoReconnect !== false;

        // Callback hooks
        this.onStatusChange = options.onStatusChange || (() => {});
        this.onSessionInit = options.onSessionInit || (() => {});
        this.onChatMessage = options.onChatMessage || (() => {});
        this.onFilingReady = options.onFilingReady || (() => {});
        this.onError = options.onError || (() => {});
    }

    getDefaultUrl() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        // If served from backend server, use current host; otherwise default to localhost:18080 or 8080
        if (window.location.host && window.location.port !== '3000' && window.location.port !== '5500') {
            return `${protocol}//${window.location.host}/ws`;
        }
        return `${protocol}//localhost:18080/ws`;
    }

    connect(customUrl = null) {
        if (customUrl) {
            this.url = customUrl;
        }

        if (this.socket && (this.socket.readyState === WebSocket.OPEN || this.socket.readyState === WebSocket.CONNECTING)) {
            return;
        }

        this.onStatusChange('CONNECTING', 'Connecting to compliance server...');

        try {
            this.socket = new WebSocket(this.url);

            this.socket.onopen = (event) => {
                this.reconnectAttempts = 0;
                this.onStatusChange('OPEN', 'Connected to GST Compliance Server');
            };

            this.socket.onmessage = (event) => {
                this.handleMessage(event.data);
            };

            this.socket.onerror = (error) => {
                this.onError(error);
                this.onStatusChange('ERROR', 'WebSocket encountered an error');
            };

            this.socket.onclose = (event) => {
                this.onStatusChange('CLOSED', `Disconnected (${event.code})`);
                if (this.autoReconnect && this.reconnectAttempts < this.maxReconnectAttempts) {
                    this.reconnectAttempts++;
                    const delay = Math.min(this.reconnectDelay * Math.pow(1.5, this.reconnectAttempts - 1), 10000);
                    setTimeout(() => {
                        if (!this.socket || this.socket.readyState === WebSocket.CLOSED) {
                            this.connect();
                        }
                    }, delay);
                }
            };
        } catch (err) {
            this.onError(err);
            this.onStatusChange('ERROR', 'Failed to establish WebSocket connection');
        }
    }

    handleMessage(raw) {
        try {
            const data = JSON.parse(raw);
            const type = data.type || '';

            switch (type) {
                case 'SESSION_INIT':
                    this.sessionId = data.session_id;
                    this.onSessionInit(data.session_id);
                    break;

                case 'CHAT_MESSAGE':
                    this.onChatMessage({
                        role: data.role || 'assistant',
                        content: data.content || '',
                        actionRequired: data.action_required || null,
                        timestamp: new Date()
                    });
                    break;

                case 'FILING_READY':
                    this.onFilingReady(data.files || {});
                    break;

                default:
                    console.warn('[ws] Unrecognized frame:', data);
                    break;
            }
        } catch (e) {
            console.error('[ws] Failed to parse message JSON:', raw, e);
        }
    }

    send(data) {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
            throw new Error('WebSocket is not open. ReadyState: ' + (this.socket ? this.socket.readyState : 'null'));
        }
        this.socket.send(JSON.stringify(data));
    }

    uploadInvoice(base64Payload, mimeType = 'image/jpeg') {
        this.send({
            type: 'UPLOAD_INVOICE',
            payload: base64Payload,
            mime_type: mimeType
        });
    }

    sendUserReply(text) {
        this.send({
            type: 'USER_REPLY',
            text: text
        });
    }

    requestFiling() {
        this.send({
            type: 'REQUEST_FILING'
        });
    }

    disconnect() {
        this.autoReconnect = false;
        if (this.socket) {
            this.socket.close();
        }
    }
}

// Export for module/global usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = TaxWebSocketClient;
} else {
    window.TaxWebSocketClient = TaxWebSocketClient;
}
