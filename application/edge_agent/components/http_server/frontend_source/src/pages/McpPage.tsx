import { ChevronRight, Plus } from 'lucide-solid';
import { batch, createEffect, createSignal, For, on, onCleanup, onMount, Show, untrack, type Component } from 'solid-js';
import { createStore, reconcile } from 'solid-js/store';
import { fetchMcpServers, saveMcpServers, testMcpServer, type McpServerPatch, type McpServers, type McpTestResult } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { TextInput } from '../components/ui/FormField';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { t, tf } from '../i18n';
import { appCapabilities } from '../state/config';
import { useDirtyTracker } from '../state/dirty';
import { pushToast } from '../state/toast';

type McpDraft = McpServerPatch & {
  key: number;
  savedUrl: string;
  has_token: boolean;
  token: string;
  clearToken: boolean;
};

function bearerPatch(draft: McpDraft): string | undefined {
  if (draft.clearToken) return '';
  return draft.token.trim().replace(/^Bearer\s+/i, '') || undefined;
}

function validToken(token?: string) {
  return !token || (token.length <= 319 && /^[A-Za-z0-9._~+/-]+=*$/.test(token));
}

function validUrl(url: string) {
  if (!/^https?:\/\//.test(url) || url.length > 319 || /[#"\\\s]/.test(url)) return false;
  try {
    const parsed = new URL(url);
    return Boolean(parsed.hostname) && !parsed.username && !parsed.password;
  } catch {
    return false;
  }
}

function nameKey(name: string) {
  return name.trim().replace(/[A-Z]/g, (char) => char.toLowerCase());
}

function validName(name: string) {
  return name.length > 0 && !/[\u0000-\u001f\u007f]/.test(name) &&
    new TextEncoder().encode(name).length <= 48;
}

function validDraft(draft: McpDraft) {
  return validName(draft.name.trim()) && validUrl(draft.server_url.trim()) && validToken(bearerPatch(draft));
}

const McpServerCard: Component<{
  draft: McpDraft;
  primary: boolean;
  initiallyOpen: boolean;
  disabled: boolean;
  showErrors: boolean;
  duplicateName: boolean;
  onChange: (patch: Partial<McpDraft>) => void;
  onTestBusy: (busy: boolean) => void;
  onRemove: () => void;
  onMakeDefault: () => void;
}> = (props) => {
  const [testing, setTesting] = createSignal(false);
  const [result, setResult] = createSignal<McpTestResult>();
  const [testError, setTestError] = createSignal('');
  const url = () => props.draft.server_url.trim();
  const savedToken = () => props.draft.has_token && url() === props.draft.savedUrl;
  const token = () => bearerPatch(props.draft);
  let details!: HTMLDetailsElement;
  let testRequest: AbortController | undefined;
  onCleanup(() => testRequest?.abort());
  createEffect(() => {
    if (props.showErrors && (props.duplicateName || !validDraft(props.draft))) details.open = true;
  });

  const clearResult = () => {
    setResult(undefined);
    setTestError('');
  };
  createEffect(on(() => [url(), token(), savedToken(), props.draft.id], clearResult));

  const testConnection = async () => {
    const controller = new AbortController();
    testRequest = controller;
    clearResult();
    setTesting(true);
    props.onTestBusy(true);
    try {
      const testToken = token() ?? (savedToken() ? undefined : '');
      setResult(await testMcpServer(url(), testToken, controller.signal, props.draft.id));
    } catch (error) {
      if (!controller.signal.aborted) {
        const reason = (error as Error).message;
        setTestError(reason === 'ESP_ERR_NOT_ALLOWED' ? t('mcpAuthFailed') : reason);
      }
    } finally {
      setTesting(false);
      props.onTestBusy(false);
    }
  };

  return (
    <details ref={details} class="group rounded border border-[var(--color-border-subtle)]"
      open={untrack(() => props.initiallyOpen)}>
      <summary class="flex cursor-pointer list-none items-center gap-3 rounded px-4 py-3 hover:bg-[var(--color-bg-surface)] focus-visible:outline-2 [&::-webkit-details-marker]:hidden">
        <ChevronRight aria-hidden="true" class="h-4 w-4 shrink-0 transition-transform group-open:rotate-90" />
        <span class="min-w-0 flex-1">
          <span class="flex flex-wrap items-center gap-x-2">
            <span class="break-all font-semibold text-[var(--color-text-primary)]">
              {props.draft.name.trim() || t('mcpNewServer')}
            </span>
            <Show when={props.primary}>
              <span class="text-xs text-[var(--color-text-secondary)]">{t('mcpDefaultServer')}</span>
            </Show>
          </span>
          <Show when={url()}>
            <span class="block truncate text-xs text-[var(--color-text-secondary)]">{url()}</span>
          </Show>
        </span>
      </summary>
      <div class="space-y-4 border-t border-[var(--color-border-subtle)] p-4">
        <TextInput
          label={t('mcpServerName')}
          hint={t('mcpServerNameHint')}
          maxLength={48}
          value={props.draft.name}
          disabled={props.disabled}
          error={props.duplicateName ? t('mcpServerNameDuplicate') :
            (props.showErrors || props.draft.name) && !validName(props.draft.name.trim()) ? t('mcpServerNameInvalid') : undefined}
          onInput={(event) => props.onChange({ name: event.currentTarget.value })}
        />
        <TextInput
          type="url"
          label={t('mcpServerUrl')}
          hint={t('mcpServerUrlHint')}
          placeholder="http://192.168.1.10:8000/mcp"
          maxLength={319}
          value={props.draft.server_url}
          disabled={props.disabled}
          error={(props.showErrors || url()) && !validUrl(url()) ? t('mcpServerUrlInvalid') : undefined}
          onInput={(event) => props.onChange({ server_url: event.currentTarget.value })}
        />
        <TextInput
          type="password"
          autocomplete="new-password"
          label={t('mcpBearerToken')}
          hint={props.draft.clearToken ? t('mcpTokenPendingClear') : t('mcpBearerHint')}
          placeholder={savedToken() && !props.draft.clearToken ? t('mcpTokenSaved') : undefined}
          error={validToken(token()) ? undefined : t('mcpTokenInvalid')}
          maxLength={326}
          value={props.draft.token}
          disabled={props.disabled}
          onInput={(event) => props.onChange({ token: event.currentTarget.value, clearToken: false })}
        />
        <div class="flex flex-wrap gap-2">
          <Button variant="secondary" disabled={props.disabled || !validUrl(url()) || !validToken(token())}
            onClick={() => void testConnection()}>
            {testing() ? t('mcpTesting') : t('mcpTest')}
          </Button>
          <Show when={savedToken()}>
            <Button variant="secondary" disabled={props.disabled}
              onClick={() => props.onChange({ token: '', clearToken: !props.draft.clearToken })}>
              {props.draft.clearToken ? t('mcpKeepToken') : t('mcpClearToken')}
            </Button>
          </Show>
          <Show when={!props.primary}>
            <Button variant="secondary" disabled={props.disabled} onClick={props.onMakeDefault}>
              {t('mcpSetDefault')}
            </Button>
          </Show>
          <Button variant="secondary" disabled={props.disabled} onClick={props.onRemove}>
            {t('mcpRemoveServer')}
          </Button>
        </div>
        <Show when={testError()}>
          <Banner kind="error" message={tf('mcpTestError', { error: testError() })} />
        </Show>
        <Show when={result()}>
          {(data) => (
            <Banner kind="success">
              <div>{tf('mcpConnected', { count: data().count })}</div>
              <Show when={data().names.length}>
                <div class="mt-1 break-all font-mono">{data().names.join(', ')}</div>
              </Show>
              <Show when={data().has_more}>
                <div class="mt-1">{t('mcpMoreTools')}</div>
              </Show>
            </Banner>
          )}
        </Show>
      </div>
    </details>
  );
};

export const McpPage: Component = () => {
  const [saved, setSaved] = createSignal<McpServers>();
  const [drafts, setDrafts] = createStore<McpDraft[]>([]);
  const [loading, setLoading] = createSignal(true);
  const [saving, setSaving] = createSignal(false);
  const [testing, setTesting] = createSignal(false);
  const [error, setError] = createSignal('');
  const [showErrors, setShowErrors] = createSignal(false);
  const available = () => appCapabilities().some((cap) => cap.group_id === 'cap_mcp_client');
  const busy = () => loading() || saving() || testing();
  let nextKey = 0;

  const payload = (): McpServerPatch[] => drafts.map((draft) => ({
    id: draft.id, name: draft.name.trim(), server_url: draft.server_url.trim(), bearer_token: bearerPatch(draft),
  }));
  const dirty = () => JSON.stringify(payload()) !== JSON.stringify(
    saved()?.servers.map(({ id, name, server_url }) => ({ id, name, server_url })) ?? [],
  );
  useDirtyTracker('mcp', dirty);

  const apply = (data: McpServers, keepOrder = false) => {
    const rows = data.servers.map((entry, index) => {
      const draft = keepOrder ? drafts[index] : drafts.find((item) => item.id === entry.id);
      return { ...entry, key: draft?.key ?? ++nextKey, savedUrl: entry.server_url, token: '', clearToken: false };
    });
    batch(() => {
      setSaved(data);
      setDrafts(reconcile(rows, { key: 'key' }));
      setShowErrors(false);
    });
  };

  const loadRequest = new AbortController();
  onCleanup(() => loadRequest.abort());
  onMount(async () => {
    try {
      apply(await fetchMcpServers(loadRequest.signal));
    } catch (err) {
      if (!loadRequest.signal.aborted) setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  });

  const duplicateName = (draft: McpDraft) => {
    const key = nameKey(draft.name);
    return Boolean(key) && drafts.some((other) => draft.key !== other.key && nameKey(other.name) === key);
  };

  const save = async () => {
    const version = saved()?.version;
    if (version === undefined || busy()) return;
    setError('');
    if (drafts.some((draft) => duplicateName(draft) || !validDraft(draft))) {
      setShowErrors(true);
      return;
    }
    setSaving(true);
    try {
      apply(await saveMcpServers(payload(), version), true);
      pushToast(t('saveSuccess'), 'success');
    } catch (err) {
      const reason = (err as Error).message;
      setError(reason === 'ESP_ERR_INVALID_STATE' ? t('mcpListChanged') : reason);
    } finally {
      setSaving(false);
    }
  };

  return (
    <TabShell>
      <PageHeader title={t('navMcp')} description={saved() ? tf('mcpServerLimit', { limit: saved()!.limit }) : undefined}
        actions={
          <Button variant="primary" disabled={!available() || busy() || drafts.length >= (saved()?.limit ?? 0)}
            onClick={() => setDrafts(drafts.length, {
              key: ++nextKey, name: '', server_url: '', savedUrl: '', has_token: false, token: '', clearToken: false,
            })}>
            <Plus class="h-4 w-4" /> {t('mcpAddServer')}
          </Button>
        }
      />
      <div class="space-y-4 px-5 py-5">
        <Show when={error()}>
          <Banner kind="error" message={error()} />
        </Show>
        <Show when={!loading() && !available()}>
          <Banner message={t('mcpUnavailable')} />
        </Show>
        <Show when={available() && saved()}>
          <For each={drafts} fallback={<p class="text-sm text-[var(--color-text-secondary)]">{t('mcpServerEmpty')}</p>}>
            {(draft, index) => (
              <McpServerCard draft={draft} primary={index() === 0} initiallyOpen={draft.id === undefined || drafts.length === 1}
                disabled={busy()} showErrors={showErrors()} duplicateName={duplicateName(draft)}
                onChange={(patch) => setDrafts(index(), patch)}
                onTestBusy={setTesting}
                onRemove={() => setDrafts(drafts.filter((item) => item !== draft))}
                onMakeDefault={() => setDrafts([draft, ...drafts.filter((item) => item !== draft)])}
              />
            )}
          </For>
          <p class="m-0 text-[0.78rem] text-[var(--color-text-secondary)]">
            {t('mcpAgentNote')}{' '}
            <a class="text-[var(--color-text-primary)] underline underline-offset-2" href="#capabilities">
              {t('navCapabilities')}
            </a>
          </p>
        </Show>
      </div>
      <SavePanel
        dirty={available() && dirty()}
        saving={busy()}
        onSave={() => void save()}
        onDiscard={() => {
          const data = saved();
          if (data) apply(data);
          setError('');
        }}
      />
    </TabShell>
  );
};
