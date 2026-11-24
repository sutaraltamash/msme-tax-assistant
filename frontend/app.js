/**
 * app.js — Core frontend controller for MSME GST Compliance Assistant.
 */

document.addEventListener('DOMContentLoaded', () => {
    // --- Security: HTML escaping helper ---
    function escapeHtml(text) {
        if (text === null || text === undefined) return '';
        const div = document.createElement('div');
        div.textContent = String(text);
        return div.innerHTML;
    }

    // --- UI Elements ---
    const profileButton = document.getElementById('profileButton');
    const profileMenu = document.getElementById('profileMenu');
    const profileEmail = document.getElementById('profileEmail');
    const dropzone = document.getElementById('dropzone');
    const fileInput = document.getElementById('fileInput');
    const uploadStatus = document.getElementById('uploadStatus');
    const uploadModal = document.getElementById('uploadModal');
    const btnAttach = document.getElementById('btnAttach');
    const browseInvoices = document.getElementById('browseInvoices');
    const uploadModalClose = document.getElementById('uploadModalClose');
    const logoutButton = document.getElementById('logoutButton');

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

        // actionRequired is kept for functional behavior (e.g., focusing chat input)
        // but the visual "Action:" badge is no longer rendered

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
            div.innerHTML = `<span class="audit-icon">${escapeHtml(icon)}</span><div><strong>${escapeHtml(item.title)}:</strong> ${escapeHtml(item.detail)}</div>`;
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
                    <td class="mono">${escapeHtml(idx + 1)}</td>
                    <td class="mono">${escapeHtml(item.hsn_code || 'N/A')}</td>
                    <td>${escapeHtml(item.description || 'N/A')}</td>
                    <td class="text-right">${escapeHtml(item.quantity || 1)}</td>
                    <td class="text-right mono">${escapeHtml(formatINR(item.unit_price_cents))}</td>
                    <td class="text-right mono">${escapeHtml(formatINR(item.taxable_amount_cents))}</td>
                    <td class="text-right mono">${escapeHtml(item.cgst_rate_bps ? ((item.cgst_rate_bps/100)+'%') : '-')}</td>
                    <td class="text-right mono">${escapeHtml(item.sgst_rate_bps ? ((item.sgst_rate_bps/100)+'%') : '-')}</td>
                    <td class="text-right mono">${escapeHtml(item.igst_rate_bps ? ((item.igst_rate_bps/100)+'%') : '-')}</td>
                    <td class="text-right mono"><strong>${escapeHtml(formatINR(item.total_cents))}</strong></td>
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
        onStatusChange: () => {},
        onSessionInit: (sessionId) => {
            appendChatMessage('assistant', 'Welcome to your GST compliance workspace. Upload an invoice (PDF, PNG, or JPG) to begin statutory rule validation.');
        },
        onChatMessage: (msg) => {
            appendChatMessage(msg.role, msg.content, msg.actionRequired);
            // If chat message mentions gaps, focus input
            if (msg.actionRequired) {
                chatInput.focus();
            }
            // If we were generating CSVs and got an error message instead of FILING_READY,
            // re-enable the button so user can retry
            if (msg.role === 'assistant' &&
                (msg.content.includes('error') || msg.content.includes('Error') ||
                 msg.content.includes('failed') || msg.content.includes('Failed')) &&
                btnRequestFiling.disabled && btnRequestFiling.innerText.includes('Generating')) {
                btnRequestFiling.disabled = false;
                btnRequestFiling.innerText = '⚡ Generate GST Offline Utility CSVs';
            }
        },
        onLedgerUpdate: (data) => {
            renderLedger(data);
        },
        onFilingReady: (files) => {
            generatedFiles = files;
            deliverablesContainer.classList.remove('hidden');
            // Re-enable the filing button so users can upload more bills and regenerate
            btnRequestFiling.disabled = false;
            btnRequestFiling.innerText = '⚡ Generate GST Offline Utility CSVs';
            appendChatMessage('assistant', 'Filing deliverables successfully generated and serialized to GST Offline Utility CSV formats. You can download or preview GSTR-1 and GSTR-3B below.');
        },
        onError: (err) => {
            console.error('[ws client error]', err);
        }
    });

    wsClient.connect();

    // --- Initialize File Handler ---
    const fileHandler = new InvoiceFileHandler({
        dropZoneElement: null,
        fileInputElement: null,
        onFileSelected: (info) => {
            uploadModal.classList.add('hidden');
            uploadStatus.innerText = `Uploading ${info.fileName} (${(info.fileSize / 1024).toFixed(1)} KB)...`;
            appendChatMessage('user', `Uploaded invoice file: ${info.fileName} (${info.mimeType})`);

            try {
                wsClient.uploadInvoice(info.base64, info.mimeType);
                uploadStatus.innerText = `Uploaded ${info.fileName}. Groq multimodal parsing & rule checks in progress...`;
            } catch (e) {
                uploadStatus.innerText = `Upload failed: ${e.message}`;
                appendChatMessage('assistant', `Error sending invoice over WebSocket: ${e.message}`);
            }
        },
        onError: (msg) => {
            uploadStatus.innerText = msg;
            // Show user-friendly error for common backend error codes
            if (msg.includes('413') || msg.includes('too large')) {
                alert('File too large. Maximum allowed size is 5 MB.');
            } else if (msg.includes('415') || msg.includes('Unsupported') || msg.includes('magic') || msg.includes('content does not match')) {
                alert('Invalid file type. Only PDF, PNG, and JPEG files are allowed.');
            } else if (msg.includes('429') || msg.includes('Rate limit')) {
                alert('Rate limit exceeded. Please wait a minute and try again.');
            } else {
                alert(msg);
            }
        }
    });

    // The processor remains responsible for validation and base64 conversion;
    // each selected/dropped file is passed through it independently.
    function processFiles(files) {
        fileHandler.processFiles(files);
    }

    browseInvoices.addEventListener('click', (event) => {
        event.stopPropagation();
        fileInput.click();
    });
    fileInput.addEventListener('change', () => {
        const files = Array.from(fileInput.files || []);
        if (files.length) {
            uploadModal.classList.add('hidden');
            processFiles(files);
        }
        // Reset so re-uploading the same file works
        fileInput.value = '';
    });
    ['dragenter', 'dragover'].forEach(type => dropzone.addEventListener(type, event => {
        event.preventDefault();
        event.stopPropagation();
        dropzone.classList.add('drag-active');
    }));
    ['dragleave', 'drop'].forEach(type => dropzone.addEventListener(type, event => {
        event.preventDefault();
        event.stopPropagation();
        dropzone.classList.remove('drag-active');
    }));
    dropzone.addEventListener('drop', event => {
        const files = Array.from(event.dataTransfer?.files || []);
        if (files.length) {
            uploadModal.classList.add('hidden');
            processFiles(files);
        }
    });

    btnAttach.addEventListener('click', () => {
        uploadModal.classList.remove('hidden');
        browseInvoices.focus();
    });
    uploadModalClose.addEventListener('click', (event) => {
        event.stopPropagation();
        uploadModal.classList.add('hidden');
    });
    uploadModal.addEventListener('click', (event) => {
        if (event.target === uploadModal) uploadModal.classList.add('hidden');
    });
    document.addEventListener('keydown', (event) => {
        if (event.key === 'Escape') uploadModal.classList.add('hidden');
    });

    // --- Auth: Check session on page load ---
    async function checkAuth() {
        try {
            const response = await fetch('/api/auth/me', {
                credentials: 'include'
            });
            if (response.ok) {
                const user = await response.json();
                profileEmail.innerText = user.email || user.username;
                return true;
            }
        } catch (e) {
            // Not authenticated
        }
        // Not authenticated - redirect to login
        window.location.href = '/login.html';
        return false;
    }

    // --- Logout ---
    if (logoutButton) {
        logoutButton.addEventListener('click', async () => {
            try {
                await fetch('/api/auth/logout', {
                    method: 'POST',
                    credentials: 'include'
                });
            } catch (e) {
                console.error('Logout error:', e);
            }
            window.location.href = '/login.html';
        });
    }

    profileButton.addEventListener('click', () => {
        const isOpen = !profileMenu.classList.contains('hidden');
        profileMenu.classList.toggle('hidden', isOpen);
        profileButton.setAttribute('aria-expanded', String(!isOpen));
    });
    document.addEventListener('click', event => {
        if (!event.target.closest('.profile-wrap')) {
            profileMenu.classList.add('hidden');
            profileButton.setAttribute('aria-expanded', 'false');
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

    // Run auth check on page load
    checkAuth();
});