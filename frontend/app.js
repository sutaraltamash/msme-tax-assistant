/**
 * app.js — Core frontend controller for MSME GST Compliance Assistant.
 */

document.addEventListener('DOMContentLoaded', () => {
    // --- UI Elements ---
    const connectionBadge = document.getElementById('connectionBadge');
    const connectionText = document.getElementById('connectionText');
    const sessionIdDisplay = document.getElementById('sessionIdDisplay');
    const dropzone = document.getElementById('dropzone');
    const fileInput = document.getElementById('fileInput');
    const uploadStatus = document.getElementById('uploadStatus');

    // Ledger elements
    const ledgerEmptyState = document.getElementById('ledgerEmptyState');
    const ledgerContent = document.getElementById('ledgerContent');
    const supplierName = document.getElementById('supplierName');
    const supplierGstin = document.getElementById('supplierGstin');
    const supplierState = document.getElementById('supplierState');
    const buyerName = document.getElementById('buyerName');
    const buyerGstin = document.getElementById('buyerGstin');
    const buyerState = document.getElementById('buyerState');
    const invoiceNumber = document.getElementById('invoiceNumber');
    const invoiceDate = document.getElementById('invoiceDate');
    const irnStatus = document.getElementById('irnStatus');
    const supplyTypeBadge = document.getElementById('supplyTypeBadge');
    const lineItemsBody = document.getElementById('lineItemsBody');
    const totalTaxable = document.getElementById('totalTaxable');
    const totalIgst = document.getElementById('totalIgst');
    const totalCgst = document.getElementById('totalCgst');
    const totalSgst = document.getElementById('totalSgst');
    const totalTax = document.getElementById('totalTax');
    const totalInvoice = document.getElementById('totalInvoice');

    // Audit list
    const auditList = document.getElementById('auditList');

    // Chat elements
    const chatMessages = document.getElementById('chatMessages');
    const chatForm = document.getElementById('chatForm');
    const chatInput = document.getElementById('chatInput');
    const btnSend = document.getElementById('btnSend');

    // Filing elements
    const btnRequestFiling = document.getElementById('btnRequestFiling');
    const deliverablesContainer = document.getElementById('deliverablesContainer');
    const gstr1Download = document.getElementById('gstr1Download');
    const gstr3bDownload = document.getElementById('gstr3bDownload');
    const gstr1PreviewBtn = document.getElementById('gstr1PreviewBtn');
    const gstr3bPreviewBtn = document.getElementById('gstr3bPreviewBtn');

    // Modal
    const previewModal = document.getElementById('previewModal');
    const modalTitle = document.getElementById('modalTitle');
    const modalBody = document.getElementById('modalBody');
    const modalCloseBtn = document.getElementById('modalCloseBtn');
    const modalDownloadBtn = document.getElementById('modalDownloadBtn');

    // Store generated CSV files
    let generatedFiles = {};
    let currentPreviewKey = '';

    // --- Currency Formatter ---
    function formatINR(amountCents) {
        if (amountCents === null || amountCents === undefined || isNaN(amountCents)) {
            return '₹ 0.00';
        }
        const rupees = Number(amountCents) / 100;
        return '₹ ' + rupees.toLocaleString('en-IN', {
            minimumFractionDigits: 2,
            maximumFractionDigits: 2
        });
    }

    // --- Format Date YYYY-MM-DD to DD/MM/YYYY ---
    function formatDate(dateStr) {
        if (!dateStr) return 'N/A';
        const parts = dateStr.split('-');
        if (parts.length === 3) {
            return `${parts[2]}/${parts[1]}/${parts[0]}`;
        }
        return dateStr;
    }

    // --- Add Chat Message ---
    function appendChatMessage(role, content, actionRequired = null) {
        const bubble = document.createElement('div');
        bubble.className = `chat-bubble chat-bubble-${role}`;

        const roleLabel = document.createElement('div');
        roleLabel.className = 'chat-role-label';
        roleLabel.innerHTML = role === 'assistant'
            ? '<span>🤖 GST Compliance Assistant</span>'
            : '<span>👤 You</span>';
        bubble.appendChild(roleLabel);

        const textDiv = document.createElement('div');
        textDiv.className = 'chat-content';
        textDiv.innerText = content;
        bubble.appendChild(textDiv);

        if (actionRequired) {
            const tag = document.createElement('span');
            tag.className = 'chat-action-tag';
            tag.innerText = `Action: ${actionRequired}`;
            bubble.appendChild(tag);
        }

        chatMessages.appendChild(bubble);
        chatMessages.scrollTop = chatMessages.scrollHeight;
    }

    // --- Update Compliance Audit Panel ---
    function updateAuditList(auditItems) {
        auditList.innerHTML = '';
        if (!auditItems || auditItems.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'audit-item audit-pass';
            empty.innerHTML = '<span class="audit-icon">✓</span><span>Ready for validation upon invoice ingestion</span>';
            auditList.appendChild(empty);
            return;
        }

        auditItems.forEach(item => {
            const div = document.createElement('div');
            div.className = `audit-item audit-${item.status}`;
            const icon = item.status === 'pass' ? '✓' : (item.status === 'warn' ? '⚠' : '✕');
            div.innerHTML = `<span class="audit-icon">${icon}</span><div><strong>${item.title}:</strong> ${item.detail}</div>`;
            auditList.appendChild(div);
        });
    }

    // --- Populate Ledger Data ---
    function renderLedger(matrixData) {
        if (!matrixData) return;

        ledgerEmptyState.classList.add('hidden');
        ledgerContent.classList.remove('hidden');

        // Supplier
        if (matrixData.supplier) {
            supplierName.innerText = matrixData.supplier.legal_name || 'N/A';
            supplierGstin.innerText = matrixData.supplier.gstin || 'N/A';
            supplierState.innerText = `State: ${matrixData.supplier.state_code || 'N/A'}`;
        }

        // Buyer
        if (matrixData.buyer) {
            buyerName.innerText = matrixData.buyer.legal_name || 'N/A';
            buyerGstin.innerText = matrixData.buyer.gstin || 'N/A';
            buyerState.innerText = `State: ${matrixData.buyer.state_code || 'N/A'}`;
        }

        // Invoice Meta
        if (matrixData.invoice) {
            invoiceNumber.innerText = matrixData.invoice.invoice_number || 'N/A';
            invoiceDate.innerText = formatDate(matrixData.invoice.invoice_date);
            const supState = (matrixData.supplier && matrixData.supplier.state_code) || '';
            const posState = matrixData.invoice.place_of_supply || '';
            const isInter = supState && posState && (supState !== posState);
            supplyTypeBadge.innerText = isInter ? 'Inter-State (IGST)' : 'Intra-State (CGST + SGST)';
            supplyTypeBadge.className = isInter ? 'badge badge-rule' : 'badge';
        }

        // IRP Meta
        if (matrixData.irp) {
            if (matrixData.irp.irn) {
                irnStatus.innerText = `IRN: ${matrixData.irp.irn.substring(0, 16)}... (Valid)`;
                irnStatus.style.color = 'var(--accent-green)';
            } else {
                irnStatus.innerText = 'IRN: Not Detected / Not Applicable';
                irnStatus.style.color = 'var(--text-secondary)';
            }
        }

        // Line items
        lineItemsBody.innerHTML = '';
        if (matrixData.line_items && matrixData.line_items.length > 0) {
            matrixData.line_items.forEach((item, idx) => {
                const row = document.createElement('tr');
                row.innerHTML = `
                    <td class="mono">${idx + 1}</td>
                    <td class="mono">${item.hsn_code || 'N/A'}</td>
                    <td>${item.description || 'N/A'}</td>
                    <td class="text-right">${item.quantity || 1}</td>
                    <td class="text-right mono">${formatINR(item.unit_price_cents)}</td>
                    <td class="text-right mono">${formatINR(item.taxable_amount_cents)}</td>
                    <td class="text-right mono">${item.cgst_rate_bps ? ((item.cgst_rate_bps/100)+'%') : '-'}</td>
                    <td class="text-right mono">${item.sgst_rate_bps ? ((item.sgst_rate_bps/100)+'%') : '-'}</td>
                    <td class="text-right mono">${item.igst_rate_bps ? ((item.igst_rate_bps/100)+'%') : '-'}</td>
                    <td class="text-right mono"><strong>${formatINR(item.total_cents)}</strong></td>
                `;
                lineItemsBody.appendChild(row);
            });
        }

        // Totals
        if (matrixData.totals) {
            totalTaxable.innerText = formatINR(matrixData.totals.base_value_cents);
            totalIgst.innerText = formatINR(matrixData.totals.igst_cents);
            totalCgst.innerText = formatINR(matrixData.totals.cgst_cents);
            totalSgst.innerText = formatINR(matrixData.totals.sgst_cents);
            totalTax.innerText = formatINR(matrixData.totals.total_tax_cents);
            totalInvoice.innerText = formatINR(matrixData.totals.total_value_cents);
        }

        // Enable filing CTA
        btnRequestFiling.disabled = false;
    }

    // --- Initialize WebSocket Client ---
    const wsClient = new TaxWebSocketClient({
        onStatusChange: (status, message) => {
            if (status === 'OPEN') {
                connectionBadge.className = 'badge badge-connected';
                connectionText.innerText = 'Connected';
            } else if (status === 'CONNECTING') {
                connectionBadge.className = 'badge badge-connecting';
                connectionText.innerText = 'Connecting...';
            } else {
                connectionBadge.className = 'badge badge-disconnected';
                connectionText.innerText = 'Disconnected';
            }
        },
        onSessionInit: (sessionId) => {
            sessionIdDisplay.innerText = `Session: ${sessionId}`;
            appendChatMessage('assistant', `Welcome! GST Compliance Session initialized (${sessionId.substring(0, 8)}...). Please upload an invoice (PDF, PNG, JPG) to begin statutory 2026 rule validation.`);
        },
        onChatMessage: (msg) => {
            appendChatMessage(msg.role, msg.content, msg.actionRequired);
            // If chat message mentions gaps, focus input
            if (msg.actionRequired) {
                chatInput.focus();
            }
        },
        onFilingReady: (files) => {
            generatedFiles = files;
            deliverablesContainer.classList.remove('hidden');
            appendChatMessage('assistant', 'Filing deliverables successfully generated and serialized to GST Offline Utility CSV formats. You can download or preview GSTR-1 and GSTR-3B below.');
        },
        onError: (err) => {
            console.error('[ws client error]', err);
        }
    });

    wsClient.connect();

    // --- Initialize File Handler ---
    const fileHandler = new InvoiceFileHandler({
        dropZoneElement: dropzone,
        fileInputElement: fileInput,
        onFileSelected: (info) => {
            uploadStatus.innerText = `Uploading ${info.fileName} (${(info.fileSize / 1024).toFixed(1)} KB)...`;
            appendChatMessage('user', `Uploaded invoice file: ${info.fileName} (${info.mimeType})`);

            try {
                wsClient.uploadInvoice(info.base64, info.mimeType);
                uploadStatus.innerText = `Uploaded ${info.fileName}. Gemini multimodal parsing & rule checks in progress...`;
            } catch (e) {
                uploadStatus.innerText = `Upload failed: ${e.message}`;
                appendChatMessage('assistant', `Error sending invoice over WebSocket: ${e.message}`);
            }
        },
        onError: (msg) => {
            uploadStatus.innerText = msg;
            alert(msg);
        }
    });

    // --- Chat Form Handling ---
    chatForm.addEventListener('submit', (e) => {
        e.preventDefault();
        const text = chatInput.value.trim();
        if (!text) return;

        appendChatMessage('user', text);
        chatInput.value = '';

        try {
            wsClient.sendUserReply(text);
        } catch (err) {
            appendChatMessage('assistant', `Error sending response: ${err.message}`);
        }
    });

    // --- Filing Trigger ---
    btnRequestFiling.addEventListener('click', () => {
        btnRequestFiling.disabled = true;
        btnRequestFiling.innerText = 'Generating GSTR CSVs...';
        appendChatMessage('user', 'Triggering final GST Offline Utility CSV generation.');

        try {
            wsClient.requestFiling();
        } catch (err) {
            appendChatMessage('assistant', `Error requesting filing: ${err.message}`);
            btnRequestFiling.disabled = false;
            btnRequestFiling.innerText = '⚡ Generate GST Offline Utility CSVs';
        }
    });

    // --- Download Helper ---
    function triggerDownload(fileName, content) {
        const blob = new Blob([content], { type: 'text/csv;charset=utf-8;' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = fileName;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }

    gstr1Download.addEventListener('click', () => {
        if (generatedFiles['gstr1_b2b.csv']) {
            triggerDownload('gstr1_b2b.csv', generatedFiles['gstr1_b2b.csv']);
        }
    });

    gstr3bDownload.addEventListener('click', () => {
        if (generatedFiles['gstr3b_summary.csv']) {
            triggerDownload('gstr3b_summary.csv', generatedFiles['gstr3b_summary.csv']);
        }
    });

    // --- Modal Preview ---
    function openPreview(fileName, content) {
        currentPreviewKey = fileName;
        modalTitle.innerText = `Preview: ${fileName}`;
        modalBody.innerText = content || '(Empty CSV)';
        previewModal.classList.remove('hidden');
    }

    gstr1PreviewBtn.addEventListener('click', () => {
        if (generatedFiles['gstr1_b2b.csv']) {
            openPreview('gstr1_b2b.csv', generatedFiles['gstr1_b2b.csv']);
        }
    });

    gstr3bPreviewBtn.addEventListener('click', () => {
        if (generatedFiles['gstr3b_summary.csv']) {
            openPreview('gstr3b_summary.csv', generatedFiles['gstr3b_summary.csv']);
        }
    });

    modalCloseBtn.addEventListener('click', () => {
        previewModal.classList.add('hidden');
    });

    modalDownloadBtn.addEventListener('click', () => {
        if (currentPreviewKey && generatedFiles[currentPreviewKey]) {
            triggerDownload(currentPreviewKey, generatedFiles[currentPreviewKey]);
        }
    });

    previewModal.addEventListener('click', (e) => {
        if (e.target === previewModal) {
            previewModal.classList.add('hidden');
        }
    });
});
