import { createResource, createSignal, For, Show, type Component } from 'solid-js';
import { createFolder, deletePath, fetchSkillCatalog, finishSkillUpload, uploadFile } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { Modal } from '../components/ui/Modal';
import { PageHeader } from '../components/ui/PageHeader';
import { t, tf } from '../i18n';

const MANIFEST = 'SKILL.md';
const MAX_UPLOAD_FILE_BYTES = 512 * 1024;
const MAX_MANIFEST_BYTES = 20 * 1024;
const MAX_ENCODED_PATH = 255;

function validFileName(name: string) {
  return name.length > 0 && name === name.trim() && !name.endsWith('.') && !name.includes('..') &&
    !/[\x00-\x1f/\\:"<>|?*]/.test(name);
}

async function readSkillFiles(files: File[], staging: string) {
  const items = files.map((file) => {
    const relative = file.webkitRelativePath;
    const path = relative ? relative.slice(relative.indexOf('/') + 1) : file.name;
    return { file, path: path.toUpperCase() === MANIFEST.toUpperCase() ? MANIFEST : path };
  });
  const manifest = items.find(({ path }) => path === MANIFEST);
  if (!manifest) throw new Error(t('skillUploadMissing'));
  if (manifest.file.size > MAX_MANIFEST_BYTES) throw new Error(t('skillUploadManifestSize'));

  const header = (await manifest.file.text()).match(/^\uFEFF?---\r?\n([\s\S]*?)\r?\n---(?:\r?\n|$)/);
  let metadata;
  try {
    metadata = header && JSON.parse(header[1] ?? '');
  } catch {
    throw new Error(t('skillUploadInvalid'));
  }
  const id = metadata?.name;
  if (typeof id !== 'string' || !validFileName(id) || typeof metadata.description !== 'string' || !metadata.description.trim() ||
      !['readonly', 'web', 'runtime'].includes(metadata.metadata?.manage_mode)) {
    throw new Error(t('skillUploadInvalid'));
  }

  const seen = new Set<string>();
  for (const { file, path } of items) {
    if (file.size > MAX_UPLOAD_FILE_BYTES) throw new Error(tf('skillUploadFileSize', { name: path }));
    const key = path.toLowerCase();
    if (seen.has(key) || !path.split('/').every(validFileName) ||
        encodeURIComponent(`${staging}/${id}/${path}`).length > MAX_ENCODED_PATH) {
      throw new Error(tf('skillUploadPath', { name: path }));
    }
    seen.add(key);
  }
  return { id, items };
}

export const SkillCatalogPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const [catalog, { refetch }] = createResource(fetchSkillCatalog);
  const [showUpload, setShowUpload] = createSignal(false);
  const [files, setFiles] = createSignal<File[]>([]);
  const [busy, setBusy] = createSignal(false);
  const [progress, setProgress] = createSignal(0);
  const [error, setError] = createSignal('');
  const [installed, setInstalled] = createSignal('');
  let fileInput!: HTMLInputElement;
  let folderInput!: HTMLInputElement;

  function selectFiles(input: HTMLInputElement) {
    if (input.files?.length) {
      setFiles(Array.from(input.files));
      setError('');
    }
    input.value = '';
  }

  async function install() {
    if (busy() || !files().length) return;
    setBusy(true);
    setProgress(0);
    setError('');
    const uploadId = Array.from(crypto.getRandomValues(new Uint8Array(8)), (value) => value.toString(16).padStart(2, '0')).join('');
    const staging = `/.skill-import-${uploadId}`;
    const stagedPaths = new Set([staging]);
    try {
      const { id, items } = await readSkillFiles(files(), staging);
      if (!catalog.error && catalog()?.skills.some((skill) => skill.id.toLowerCase() === id.toLowerCase())) {
        throw new Error(t('skillUploadExists'));
      }
      for (const { file, path } of items) {
        const target = `${staging}/${id}/${path}`;
        const parent = target.slice(0, target.lastIndexOf('/'));
        if (!stagedPaths.has(parent)) {
          const parents: string[] = [];
          for (let dir = parent; !stagedPaths.has(dir); dir = dir.slice(0, dir.lastIndexOf('/'))) {
            parents.push(dir);
          }
          parents.reverse().forEach((dir) => stagedPaths.add(dir));
          await createFolder(parent, { recursive: true });
        }
        stagedPaths.add(target);
        await uploadFile(target, file);
        setProgress((count) => count + 1);
      }
      await finishSkillUpload(uploadId, id);
      setInstalled(id);
      setShowUpload(false);
    } catch (err) {
      const message = err instanceof Error ? err.message : t('skillUploadFailed');
      if (stagedPaths.size === 1) setError(message);
      else if (message === 'skill_exists') setError(t('skillUploadExists'));
      else if (message === 'skill_invalid') setError(t('skillUploadInvalid'));
      else if (message === 'Device storage is full') setError(t('skillUploadStorageFull'));
      else setError(t('skillUploadFailed'));
      if (stagedPaths.size > 1) {
        for (const path of [...stagedPaths].reverse()) await deletePath(path).catch(() => {});
      }
    } finally {
      setBusy(false);
    }
  }

  return (
    <TabShell>
      <PageHeader
        title={t('navSkillCatalog')}
        description={t('skillCatalogDescription')}
        actions={
          <>
            <Show when={!catalog.error && catalog()?.can_upload}>
              <Button size="sm" onClick={() => { setFiles([]); setError(''); setShowUpload(true); }}>
                {t('skillUploadTitle')}
              </Button>
            </Show>
            <Button variant="secondary" size="sm" disabled={catalog.loading} onClick={() => void refetch()}>
              {t('commandRefresh')}
            </Button>
          </>
        }
      />
      <div class="space-y-4 px-5 py-4">
        <Show when={installed()}>
          <Banner kind="success">
            <div class="flex flex-wrap items-center justify-between gap-2">
              <span>{tf('skillUploadDone', { id: installed() })}</span>
              <Button size="sm" onClick={props.onRestartRequest}>{t('skillUploadRestart')}</Button>
            </div>
          </Banner>
        </Show>
        <Show when={catalog.loading}>
          <p class="m-0 text-sm text-[var(--color-text-muted)]">{t('statusLoading')}</p>
        </Show>
        <Show when={!catalog.loading}>
          <Show when={!catalog.error} fallback={<Banner kind="error" message={t('skillCatalogFailed')} />}>
            <Show when={catalog()?.skills.length} fallback={<Banner message={t('skillCatalogEmpty')} />}>
              <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] p-0">
                <For each={catalog()?.skills}>
                  {(skill) => (
                    <li class="space-y-1.5 py-3">
                      <code class="break-all text-sm font-semibold">{skill.id}</code>
                      <p class="m-0 break-words text-sm text-[var(--color-text-secondary)]">
                        {skill.id === 'cap_micropython' ? t('skillCatalogPython') : skill.summary}
                      </p>
                      <Show when={skill.unavailable_groups.length}>
                        <p class="m-0 break-words text-xs text-[var(--color-text-muted)]">
                          {tf('skillCatalogUnavailable', { groups: skill.unavailable_groups.join(', ') })}
                        </p>
                      </Show>
                    </li>
                  )}
                </For>
              </ul>
            </Show>
          </Show>
        </Show>
        <p class="m-0 text-sm text-[var(--color-text-secondary)]">
          {t('skillCatalogUseChat')}{' '}
          <a class="underline" href="#webim">{t('navWebIm')}</a>
        </p>
      </div>
      <Modal
        open={showUpload()}
        onClose={() => { if (!busy()) setShowUpload(false); }}
        title={t('skillUploadTitle')}
        subtitle={t('skillUploadHint')}
        widthClass="w-full max-w-lg max-h-[calc(100vh-3rem)]"
        actions={
          <Button disabled={busy() || !files().length} onClick={() => void install()}>
            {t('fileUpload')}
          </Button>
        }
      >
        <div class="space-y-3 px-5 pb-4">
          <input type="file" accept=".md" hidden ref={fileInput} onChange={({ currentTarget }) => selectFiles(currentTarget)} />
          <input type="file" hidden ref={(input) => {
            folderInput = input;
            input.setAttribute('webkitdirectory', '');
          }} onChange={({ currentTarget }) => selectFiles(currentTarget)} />
          <div class="flex flex-wrap gap-2">
            <Button variant="secondary" disabled={busy()} onClick={() => folderInput.click()}>{t('skillUploadFolder')}</Button>
            <Button variant="secondary" disabled={busy()} onClick={() => fileInput.click()}>{t('skillUploadFile')}</Button>
          </div>
          <Show when={files()[0]} keyed>{(firstFile) => (
            <p class="m-0 break-all text-sm" role="status">
              {busy()
                ? tf('skillUploadProgress', { count: progress(), total: files().length })
                : tf('skillUploadSelected', {
                  name: firstFile.webkitRelativePath.split('/')[0] || firstFile.name,
                  count: files().length,
                })}
            </p>
          )}</Show>
          <Banner kind="error" message={error()} />
        </div>
      </Modal>
    </TabShell>
  );
};
