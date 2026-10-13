/**
 * file_handler.js — Drag-and-drop & Base64 encoder for invoice ingestion.
 */

class InvoiceFileHandler {
    constructor(options = {}) {
        this.dropZoneElement = options.dropZoneElement;
        this.fileInputElement = options.fileInputElement;
        this.onFileSelected = options.onFileSelected || (() => {});
        this.onError = options.onError || (() => {});
        this.maxSizeBytes = options.maxSizeBytes || 10 * 1024 * 1024; // 10MB default
        this.allowedTypes = options.allowedTypes || [
            'image/jpeg',
            'image/png',
            'image/webp',
            'application/pdf'
        ];

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

    processFile(file) {
        if (!file) return;

        // Size check
        if (file.size > this.maxSizeBytes) {
            this.onError(`File too large (${(file.size / (1024 * 1024)).toFixed(2)} MB). Maximum allowed size is 10 MB.`);
            return;
        }

        // Type check
        let mimeType = file.type || '';
        if (!mimeType) {
            const ext = file.name.split('.').pop().toLowerCase();
            if (ext === 'pdf') mimeType = 'application/pdf';
            else if (ext === 'jpg' || ext === 'jpeg') mimeType = 'image/jpeg';
            else if (ext === 'png') mimeType = 'image/png';
            else if (ext === 'webp') mimeType = 'image/webp';
            else mimeType = 'application/octet-stream';
        }

        const reader = new FileReader();

        reader.onload = () => {
            const dataUrl = reader.result;
            // Extract base64 part after "data:mime/type;base64,"
            const base64Index = dataUrl.indexOf(';base64,');
            let base64Payload = '';
            if (base64Index !== -1) {
                base64Payload = dataUrl.substring(base64Index + 8);
            } else {
                base64Payload = dataUrl;
            }

            this.onFileSelected({
                file: file,
                fileName: file.name,
                fileSize: file.size,
                mimeType: mimeType,
                base64: base64Payload,
                dataUrl: dataUrl
            });
        };

        reader.onerror = (err) => {
            this.onError('Failed to read file: ' + (err.message || 'Unknown error'));
        };

        reader.readAsDataURL(file);
    }
}

// Export for module/global usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = InvoiceFileHandler;
} else {
    window.InvoiceFileHandler = InvoiceFileHandler;
}
