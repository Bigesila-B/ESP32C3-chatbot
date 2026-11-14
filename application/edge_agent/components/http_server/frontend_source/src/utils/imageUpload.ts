import { t, tf } from '../i18n';

// Keep this in sync with HTTP_SERVER_UPLOAD_MAX_SIZE in http_server_priv.h.
export const UPLOAD_MAX_BYTES = 512 * 1024;
// Bound canvas memory on mobile while allowing a 4K image at full size.
const MAX_CANVAS_EDGE = 8192;
const MAX_CANVAS_PIXELS = 12 * 1024 * 1024;
const JPEG_QUALITIES = [0.95, 0.90, 0.85, 0.80];
const IMAGE_EXTENSION = /\.(avif|bmp|gif|heic|heif|jpe?g|png|svg|tiff?|webp)$/i;

export function compressedImagePath(path: string, type: string): string {
  const extension = type === 'image/png' ? '.png' : '.jpg';
  return path.replace(/\.[^./]+$/, '') + extension;
}

export function imageUploadSize(file: File): number {
  return Math.ceil(file.size / 1024);
}

function loadImage(file: File, url: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error(tf('uploadImageDecodeFailed', { name: file.name })));
    image.src = url;
  });
}

function encodeImage(canvas: HTMLCanvasElement, type: string, quality?: number): Promise<Blob | null> {
  return new Promise((resolve) => {
    canvas.toBlob((blob) => resolve(blob?.type === type && blob.size > 0 ? blob : null), type, quality);
  });
}

function asUploadFile(original: File, blob: Blob): File {
  return new File([blob], compressedImagePath(original.name, blob.type), {
    type: blob.type,
    lastModified: original.lastModified,
  });
}

export async function prepareImageUpload(file: File): Promise<File> {
  if (file.size <= UPLOAD_MAX_BYTES) return file;
  if (!file.type.startsWith('image/') && !IMAGE_EXTENSION.test(file.name)) {
    throw new Error(tf('uploadFileTooLarge', { limit: UPLOAD_MAX_BYTES / 1024 }));
  }

  const url = URL.createObjectURL(file);
  const canvas = document.createElement('canvas');
  try {
    const image = await loadImage(file, url);
    const context = canvas.getContext('2d');
    const { naturalWidth: width, naturalHeight: height } = image;
    if (!context || !width || !height) throw new Error(t('uploadImageCompressionFailed'));

    let tryPng = file.type === 'image/png' || /\.png$/i.test(file.name);
    let scale = Math.min(1, MAX_CANVAS_EDGE / Math.max(width, height), Math.sqrt(MAX_CANVAS_PIXELS / (width * height)));
    while (true) {
      canvas.width = Math.max(1, Math.floor(width * scale));
      canvas.height = Math.max(1, Math.floor(height * scale));
      context.imageSmoothingQuality = 'high';
      context.drawImage(image, 0, 0, canvas.width, canvas.height);

      if (tryPng) {
        const png = await encodeImage(canvas, 'image/png');
        if (png && png.size <= UPLOAD_MAX_BYTES) return asUploadFile(file, png);
        tryPng = false;
      }

      // JPEG needs an opaque background. Leave transparent pixels intact in PNG.
      context.globalCompositeOperation = 'destination-over';
      context.fillStyle = '#fff';
      context.fillRect(0, 0, canvas.width, canvas.height);
      let jpegSize = 0;
      for (const quality of JPEG_QUALITIES) {
        const jpeg = await encodeImage(canvas, 'image/jpeg', quality);
        if (!jpeg) break;
        if (jpeg.size <= UPLOAD_MAX_BYTES) return asUploadFile(file, jpeg);
        jpegSize = jpeg.size;
      }
      if (canvas.width === 1 && canvas.height === 1) break;

      // Estimate the resize from the bytes still needed and recheck the result.
      const fitScale = jpegSize ? Math.sqrt(UPLOAD_MAX_BYTES / jpegSize) : 0.78;
      scale *= Math.max(0.78, Math.min(0.94, fitScale));
    }
    throw new Error(t('uploadImageCompressionFailed'));
  } finally {
    URL.revokeObjectURL(url);
    canvas.width = canvas.height = 1;
  }
}
