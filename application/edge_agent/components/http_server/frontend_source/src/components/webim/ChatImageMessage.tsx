import { createMemo, createSignal, For, Show, type Component, type JSX } from 'solid-js';
import { t } from '../../i18n';

type ImagePart = { alt: string; url: string };

function safeImageUrl(url: string): boolean {
  if (/^\/api\/mcp\/image\?id=[0-9a-f]{8}$/i.test(url)) return true;
  if (!/^https?:\/\//i.test(url) || /[\\\s<>]/.test(url)) return false;
  try {
    const parsed = new URL(url);
    return Boolean(parsed.hostname) && !parsed.username && !parsed.password;
  } catch {
    return false;
  }
}

function imageParts(text: string): (string | ImagePart)[] {
  // Preserve fenced code, where Markdown examples should stay literal.
  const syntax = /(^[ \t]{0,3}(?:`{3,}|~{3,})[^\n]*\n[\s\S]*?(?:^[ \t]{0,3}(?:`{3,}|~{3,})[^\n]*(?:\n|$)|$))|(?<![\\`])!\[([^\]\r\n]*)\]\((?:<([^>\r\n]+)>|([^\s)]+))\)/gm;
  const parts: (string | ImagePart)[] = [];
  let cursor = 0;
  let images = 0;
  for (const match of text.matchAll(syntax)) {
    if (match[1]) continue;
    const url = match[3] ?? match[4];
    if (!url || !safeImageUrl(url)) continue;
    if (match.index > cursor) parts.push(text.slice(cursor, match.index));
    parts.push({ url, alt: match[2] ?? '' });
    cursor = match.index + match[0].length;
    if (++images === 4) break;
  }
  if (cursor < text.length) parts.push(text.slice(cursor));
  return parts;
}

const ChatImage: Component<{ image: ImagePart; onLoad?: () => void }> = (props) => {
  const [failed, setFailed] = createSignal(false);
  const alt = () => props.image.alt || t('webimOpenImage');
  return (
    <figure class="m-0 my-2">
      <a href={props.image.url} target="_blank" rel="noopener noreferrer">
        <Show when={!failed()} fallback={t('webimOpenImage')}>
          <img
            src={props.image.url}
            alt={alt()}
            class="block h-auto max-h-[min(26rem,50vh)] w-auto max-w-full rounded-[var(--radius-sm)] object-contain"
            loading="lazy"
            decoding="async"
            referrerPolicy="no-referrer"
            onLoad={props.onLoad}
            onError={() => setFailed(true)}
          />
        </Show>
      </a>
      <Show when={failed()}>
        <figcaption class="text-[0.78rem] text-[var(--color-text-muted)]">{t('webimImageUnavailable')}</figcaption>
      </Show>
    </figure>
  );
};

export const ChatImageMessage: Component<{
  text: string;
  renderText: (text: string) => JSX.Element;
  onImageLoad?: () => void;
}> = (props) => {
  const parts = createMemo(() => imageParts(props.text));
  return (
    <For each={parts()}>
      {(part) => (typeof part === 'string'
        ? props.renderText(part)
        : <ChatImage image={part} onLoad={props.onImageLoad} />)}
    </For>
  );
};
