import { createSignal, onCleanup, Show, type Component } from 'solid-js';
import { testMcpServer, type McpTestResult } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { TextInput } from '../components/ui/FormField';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { t, tf } from '../i18n';
import { appCapabilities } from '../state/config';
import { createConfigTab } from '../state/configTab';

export const McpPage: Component = () => {
  const tab = createConfigTab({
    tab: 'mcp',
    groups: ['mcp'],
    toForm: (config) => ({ mcp_server_url: config.mcp_server_url ?? '' }),
    fromForm: (form) => ({ mcp_server_url: form.mcp_server_url.trim() }),
  });
  const [testing, setTesting] = createSignal(false);
  const [result, setResult] = createSignal<McpTestResult>();
  const [testError, setTestError] = createSignal('');
  const available = () => appCapabilities().some((cap) => cap.group_id === 'cap_mcp_client');
  const url = () => tab.form.mcp_server_url.trim();
  let testRequest: AbortController | undefined;
  onCleanup(() => testRequest?.abort());

  const clearResult = () => {
    setResult(undefined);
    setTestError('');
  };

  const testConnection = async () => {
    const controller = new AbortController();
    testRequest = controller;
    clearResult();
    setTesting(true);
    try {
      setResult(await testMcpServer(url(), controller.signal));
    } catch (error) {
      if (!controller.signal.aborted) setTestError((error as Error).message);
    } finally {
      setTesting(false);
    }
  };

  return (
    <TabShell>
      <PageHeader title={t('navMcp') as string} />
      <div class="space-y-4 px-5 py-5">
        <Show when={tab.error()}>
          <Banner kind="error" message={tab.error() ?? undefined} />
        </Show>
        <Show when={!tab.loading() && !available()}>
          <Banner message={t('mcpUnavailable')} />
        </Show>
        <TextInput
          type="url"
          label={t('mcpServerUrl')}
          hint={t('mcpServerUrlHint')}
          placeholder="http://192.168.1.10:8000/mcp"
          maxLength={255}
          value={tab.form.mcp_server_url}
          disabled={!available() || tab.loading() || tab.saving() || testing()}
          onInput={(event) => {
            tab.setForm('mcp_server_url', event.currentTarget.value);
            clearResult();
          }}
        />
        <Button
          variant="secondary"
          disabled={!available() || !url() || tab.loading() || tab.saving() || testing()}
          onClick={() => void testConnection()}
        >
          {testing() ? t('mcpTesting') : t('mcpTest')}
        </Button>
        <Show when={testError()}>
          <Banner kind="error" message={tf('mcpTestError', { error: testError() })} />
        </Show>
        <Show when={result()}>
          {(data) => (
            <Banner kind="success">
              <div>{tf('mcpConnected', { count: data().count })}</div>
              <Show when={data().names.length > 0}>
                <div class="mt-1 break-all font-mono">{data().names.join(', ')}</div>
              </Show>
              <Show when={data().has_more}>
                <div class="mt-1">{t('mcpMoreTools')}</div>
              </Show>
            </Banner>
          )}
        </Show>
        <Show when={available()}>
          <p class="m-0 text-[0.78rem] text-[var(--color-text-muted)]">{t('mcpAgentNote')}</p>
        </Show>
      </div>
      <SavePanel
        dirty={available() && tab.dirty()}
        saving={tab.saving() || testing()}
        onSave={() => void tab.save().catch(() => undefined)}
        onDiscard={() => {
          tab.discard();
          clearResult();
        }}
      />
    </TabShell>
  );
};
