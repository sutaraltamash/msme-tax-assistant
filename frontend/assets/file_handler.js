/**
 * file_handler.js — Drag-and-drop & Base64 encoder for invoice ingestion.
 * Security-hardened: client-side validation + magic byte verification.
 */

class InvoiceFileHandler {
    constructor(options = {}) {
        this.dropZoneElement = options.dropZoneElement;
        this.fileInputElement = options.fileInputElement;
        this.onFileSelected = options.onFileSelected || (() => {});
        this.onError = options.onError || (() => {});
        this.maxSizeBytes = options.maxSizeBytes || 5 * 1024 * 1024; // 5MB (matches backend)
        this.maxFilesPerBatch = options.maxFilesPerBatch || 5; // Max 5 files per batch
        this.allowedTypes = options.allowedTypes || [
            'image/jpeg',
            'image/png',
            'application/pdf'
        ];

        // Magic byte signatures for client-side verification
        this.magicBytes = {
            'application/pdf': new Uint8Array([0x25, 0x50, 0x44, 0x46]), // %PDF
            'image/png': new Uint8Array([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
            'image/jpeg': new Uint8Array([0xFF, 0xD8, 0xFF])
        };

        // PDF.js module cache (lazy-loaded)
        this._pdfjsLib = null;

        this.initEventListeners();
    }

    initEventListeners() {
        if (this.dropZoneElement) {
            ['dragenter', 'dragover'].forEach(eventName => {
                this.dropZoneElement.addEventListener(eventName, (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    this.dropZoneElement.classList.add('drag-active');
                }, false);
            });

            ['dragleave', 'drop'].forEach(eventName => {
                this.dropZoneElement.addEventListener(eventName, (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    this.dropZoneElement.classList.remove('drag-active');
                }, false);
            });

            this.dropZoneElement.addEventListener('drop', (e) => {
                const dt = e.dataTransfer;
                const files = dt.files;
                if (files && files.length > 0) {
                    this.processFile(files[0]);
                }
            }, false);

            this.dropZoneElement.addEventListener('click', (e) => {
                // If clicked directly or on interactive zone (not on a button that triggers its own click)
                if (this.fileInputElement && e.target !== this.fileInputElement) {
                    this.fileInputElement.click();
                }
            });
        }

        if (this.fileInputElement) {
            this.fileInputElement.addEventListener('change', (e) => {
                if (this.fileInputElement.files && this.fileInputElement.files.length > 0) {
                    this.processFile(this.fileInputElement.files[0]);
                }
            });
        }
    }

    // Process a batch of files (for multi-file upload support)
    processFiles(files) {
        if (!files || files.length === 0) return;

        // Batch size limit
        if (files.length > this.maxFilesPerBatch) {
            this.onError(`Too many files. Maximum ${this.maxFilesPerBatch} files allowed per upload.`);
            return;
        }

        // Process each file independently
        Array.from(files).forEach(file => this.processFile(file));
    }

    async processFile(file) {
        if (!file) return;

        // Size check
        if (file.size > this.maxSizeBytes) {
            this.onError(`File too large (${(file.size / (1024 * 1024)).toFixed(2)} MB). Maximum allowed size is 5 MB.`);
            return;
        }

        // Type check (extension-based fallback)
        let mimeType = file.type || '';
        if (!mimeType) {
            const ext = file.name.split('.').pop().toLowerCase();
            if (ext === 'pdf') mimeType = 'application/pdf';
            else if (ext === 'jpg' || ext === 'jpeg') mimeType = 'image/jpeg';
            else if (ext === 'png') mimeType = 'image/png';
            else mimeType = 'application/octet-stream';
        }

        // Check if type is allowed
        if (!this.allowedTypes.includes(mimeType)) {
            this.onError(`File type "${mimeType}" not allowed. Allowed: PDF, PNG, JPEG.`);
            return;
        }

        // Read file as arrayBuffer first for magic byte verification
        let arrayBuffer;
        try {
            arrayBuffer = await file.arrayBuffer();
        } catch (err) {
            this.onError('Failed to read file: ' + (err.message || 'Unknown error'));
            return;
        }

        // Security: Client-side magic byte verification BEFORE any rendering
        const fileHeader = new Uint8Array(arrayBuffer.slice(0, 8));
        if (!this.verifyMagicBytes(fileHeader, mimeType)) {
            this.onError(`File content does not match declared type "${mimeType}". Upload rejected for security.`);
            return;
        }

        // Handle PDF files - render to image
        if (mimeType === 'application/pdf') {
            await this._processPdfFile(file, arrayBuffer);
            return;
        }

        // For PNG/JPEG files - convert arrayBuffer to base64 dataUrl
        const base64Payload = this._arrayBufferToBase64(arrayBuffer);
        const dataUrl = `data:${mimeType};base64,${base64Payload}`;

        this.onFileSelected({
            file: file,
            fileName: file.name,
            fileSize: file.size,
            mimeType: mimeType,
            base64: base64Payload,
            dataUrl: dataUrl
        });
    }

    // Convert arrayBuffer to base64 string
    _arrayBufferToBase64(arrayBuffer) {
        const bytes = new Uint8Array(arrayBuffer);
        let binary = '';
        const len = bytes.byteLength;
        for (let i = 0; i < len; i++) {
            binary += String.fromCharCode(bytes[i]);
        }
        return btoa(binary);
    }

    // Process PDF file: render pages compactly (auto-cropped JPEG to stay well within Groq ITPM)
    async _processPdfFile(file, arrayBuffer) {
        let pdf = null;
        let finalCanvas = null;
        const pageCanvases = [];
        try {
            if (!this._pdfjsLib) {
                const pdfjsModule = await import('/assets/pdf.min.js');
                this._pdfjsLib = pdfjsModule.default || pdfjsModule;
                this._pdfjsLib.GlobalWorkerOptions.workerSrc = '/assets/pdf.worker.min.js';
            }

            const pdfjsLib = this._pdfjsLib;
            pdf = await pdfjsLib.getDocument({
                data: new Uint8Array(arrayBuffer),
                isEvalSupported: false
            }).promise;

            const maxPages = Math.min(pdf.numPages, 3);
            let totalHeight = 0;
            let maxWidth = 0;

            for (let pageNum = 1; pageNum <= maxPages; pageNum++) {
                const page = await pdf.getPage(pageNum);
                const baseVp = page.getViewport({ scale: 1.0 });
                const scale = Math.min(1.5, 900 / Math.max(baseVp.width, 1));
                const viewport = page.getViewport({ scale });

                const rawCanvas = document.createElement('canvas');
                const rawCtx = rawCanvas.getContext('2d');
                rawCanvas.width = Math.round(viewport.width);
                rawCanvas.height = Math.round(viewport.height);
                rawCtx.fillStyle = '#ffffff';
                rawCtx.fillRect(0, 0, rawCanvas.width, rawCanvas.height);

                await page.render({ canvasContext: rawCtx, viewport }).promise;

                // Auto-crop blank top/bottom whitespace so Groq Vision does not waste tokens on empty margins
                const imgData = rawCtx.getImageData(0, 0, rawCanvas.width, rawCanvas.height).data;
                const w = rawCanvas.width, h = rawCanvas.height;
                let top = h, bottom = 0;
                for (let y = 0; y < h; y += 2) {
                    let rowHasInk = false;
                    const rowOff = y * w * 4;
                    for (let x = 0; x < w; x += 4) {
                        const i = rowOff + x * 4;
                        if (imgData[i] < 245 || imgData[i + 1] < 245 || imgData[i + 2] < 245) {
                            rowHasInk = true;
                            break;
                        }
                    }
                    if (rowHasInk) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                    }
                }
                const pad = 16;
                const cropTop = top < bottom ? Math.max(0, top - pad) : 0;
                const cropBottom = top < bottom ? Math.min(h, bottom + pad) : h;
                const cropH = Math.max(1, cropBottom - cropTop);

                const cropped = document.createElement('canvas');
                cropped.width = w;
                cropped.height = cropH;
                cropped.getContext('2d').drawImage(rawCanvas, 0, cropTop, w, cropH, 0, 0, w, cropH);
                rawCanvas.width = 0; rawCanvas.height = 0;

                maxWidth = Math.max(maxWidth, w);
                totalHeight += cropH;
                pageCanvases.push(cropped);
            }

            finalCanvas = document.createElement('canvas');
            const finalContext = finalCanvas.getContext('2d');
            finalCanvas.width = maxWidth;
            finalCanvas.height = totalHeight;
            finalContext.fillStyle = '#ffffff';
            finalContext.fillRect(0, 0, finalCanvas.width, finalCanvas.height);

            let yOffset = 0;
            for (const pc of pageCanvases) {
                finalContext.drawImage(pc, 0, yOffset);
                yOffset += pc.height;
                pc.width = 0; pc.height = 0;
            }

            const mimeType = 'image/jpeg';
            const dataUrl = finalCanvas.toDataURL('image/jpeg', 0.82);
            const base64Payload = dataUrl.substring(dataUrl.indexOf(';base64,') + 8);

            this.onFileSelected({
                file: file,
                fileName: file.name,
                fileSize: file.size,
                mimeType: mimeType,
                base64: base64Payload,
                dataUrl: dataUrl
            });
        } catch (err) {
            this.onError('Failed to process PDF file: ' + (err.message || 'Unknown error'));
        } finally {
            for (const pc of pageCanvases) { pc.width = 0; pc.height = 0; }
            if (finalCanvas) { finalCanvas.width = 0; finalCanvas.height = 0; }
            if (pdf) { try { await pdf.destroy(); } catch (_) {} }
        }
    }

    // Verify magic bytes match expected file type
    verifyMagicBytes(header, expectedMimeType) {
        const magic = this.magicBytes[expectedMimeType];
        if (!magic) return false;

        if (header.length < magic.length) return false;

        for (let i = 0; i < magic.length; i++) {
            if (header[i] !== magic[i]) return false;
        }
        return true;
    }
}

// Export for module/global usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = InvoiceFileHandler;
} else {
    window.InvoiceFileHandler = InvoiceFileHandler;
}
