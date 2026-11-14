import { t, tf } from '../i18n';

// Keep this in sync with HTTP_SERVER_UPLOAD_MAX_SIZE in http_server_priv.h.
export const UPLOAD_MAX_BYTES = 512 * 1024;
const MAX_IMAGE_EDGE = 2560;
const JPEG_QUALITIES = [0.9, 0.76, 0.6];
const IMAGE_EXTENSION = /\.(avif|bmp|gif|heic|heif|jpe?g|png|svg|tiff?|webp)$/i;

export function jpegUploadName(name: string): string {
  return name.replace(/\.[^.\/]+$/, '') + '.jpg';
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

async function encodeJpeg(canvas: HTMLCanvasElement, quality: number): Promise<Blob | null> {
  return new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', quality));
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
    const context = canvas.getContext('2d', { alpha: false });
    if (!context) throw new Error(t('uploadImageCompressionFailed'));

    const longest = Math.max(image.naturalWidth, image.naturalHeight);
    let edge = Math.min(longest, MAX_IMAGE_EDGE);
    while (edge >= 1) {
      canvas.width = Math.max(1, Math.round(image.naturalWidth * edge / longest));
      canvas.height = Math.max(1, Math.round(image.naturalHeight * edge / longest));
      context.fillStyle = '#fff';
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.drawImage(image, 0, 0, canvas.width, canvas.height);

      for (const quality of JPEG_QUALITIES) {
        const blob = await encodeJpeg(canvas, quality);
        if (blob?.type === 'image/jpeg' && blob.size > 0 && blob.size <= UPLOAD_MAX_BYTES) {
          return new File([blob], jpegUploadName(file.name), { type: blob.type, lastModified: file.lastModified });
        }
      }
      edge = Math.floor(edge * 0.75);
    }
    throw new Error(t('uploadImageCompressionFailed'));
  } finally {
    URL.revokeObjectURL(url);
    canvas.width = canvas.height = 1;
  }
}
