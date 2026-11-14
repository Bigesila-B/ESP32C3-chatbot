import { createEffect, createSignal, Show, type Component } from 'solid-js';
import { currentLocale, t, tf } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { appConfig, appStatus } from '../state/config';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput, SelectInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { Switch } from '../components/ui/Switch';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';
import { pushToast } from '../state/toast';

type BasicForm = {
  wifi_ssid: string;
  wifi_password: string;
  ap_ssid: string;
  ap_password: string;
  ap_behavior: string;
  time_timezone: string;
  show_tool_progress: boolean;
};

function networkSettingsChanged(form: BasicForm, config: Partial<AppConfig>) {
  return (
    form.wifi_ssid !== (config.wifi_ssid ?? '') ||
    form.wifi_password !== (config.wifi_password ?? '') ||
    form.ap_ssid !== (config.ap_ssid ?? '') ||
    form.ap_password !== (config.ap_password ?? '') ||
    form.ap_behavior !== (config.ap_behavior ?? 'keep')
  );
}

export const BasicPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<BasicForm>({
    tab: 'basic',
    groups: ['wifi', 'time', 'chat'],
    toForm: (config: Partial<AppConfig>) => ({
      wifi_ssid: config.wifi_ssid ?? '',
      wifi_password: config.wifi_password ?? '',
      ap_ssid: config.ap_ssid ?? '',
      ap_password: config.ap_password ?? '',
      ap_behavior: config.ap_behavior ?? 'keep',
      time_timezone: config.time_timezone ?? '',
      show_tool_progress: config.show_tool_progress === 'true' || config.show_tool_progress === '1',
    }),
    fromForm: (form) => {
      const config = appConfig();
      const patch: Partial<AppConfig> = { show_tool_progress: String(form.show_tool_progress) };
      if (networkSettingsChanged(form, config)) {
        patch.wifi_ssid = form.wifi_ssid.trim();
        patch.wifi_password = form.wifi_password;
        patch.ap_ssid = form.ap_ssid.trim();
        patch.ap_password = form.ap_password;
        patch.ap_behavior = form.ap_behavior;
      }
      if (form.time_timezone !== (config.time_timezone ?? '')) {
        patch.time_timezone = form.time_timezone.trim();
      }
      return patch;
    },
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  createEffect(() => {
    void tab.form.wifi_ssid;
    void tab.form.wifi_password;
    void tab.form.ap_ssid;
    void tab.form.ap_password;
    setValidationError(null);
  });

  const handleSave = async () => {
    if (tab.loading() || tab.saving()) return;
    const wifiSsid = tab.form.wifi_ssid.trim();
    const wifiPassword = tab.form.wifi_password;
    const apPassword = tab.form.ap_password;

    const networkChanged = networkSettingsChanged(tab.form, appConfig());
    const needsRestart = networkChanged || tab.form.time_timezone !== (appConfig().time_timezone ?? '');

    if (networkChanged && !wifiSsid) {
      const message = t('wifiValidationSsidRequired') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    if (networkChanged && wifiPassword.length > 0 && wifiPassword.length < 8) {
      const message = t('wifiValidationPasswordLength') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    if (networkChanged && apPassword.length > 0 && apPassword.length < 8) {
      const message = t('apValidationPasswordLength') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await tab.save();
    if (needsRestart) setConfirmOpen(true);
  };

  const currentApSsid = () => appStatus()?.ap_ssid ?? '';

  const apNameHint = () => {
    const ssid = currentApSsid();
    return ssid ? tf('apNameHint', { ssid }) : '';
  };

  const timezoneHint = () =>
    currentLocale() === 'zh-cn' ? (
      <>
        仅接受 POSIX TZ 字符串，符号与日常 UTC 表示相反。北京时间（UTC+8）应写作 "CST-8"
        ，纽约（UTC-5）写作 "EST5"。可在
        <a
          href="https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv"
          target="_blank"
          rel="noopener noreferrer"
          class="underline underline-offset-2 hover:text-[var(--color-text-primary)]"
        >
          此表格
        </a>
        查阅 IANA 时区与 POSIX 表达转换关系。
      </>
    ) : (
      (t('timezoneHelp') as string)
    );

  return (
    <TabShell>
      <PageHeader title={t('navBasic') as string} />
      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionChat') as string}>
          <Switch
            checked={tab.form.show_tool_progress}
            disabled={tab.loading() || tab.saving()}
            label={t('showToolProgress')}
            hint={t('showToolProgressHint')}
            onChange={(checked) => tab.setForm('show_tool_progress', checked)}
          />
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionWifi') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              label={t('wifiSsid')}
              autocomplete="off"
              value={tab.form.wifi_ssid}
              onInput={(event) => tab.setForm('wifi_ssid', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('wifiPassword')}
              autocomplete="new-password"
              value={tab.form.wifi_password}
              onInput={(event) => tab.setForm('wifi_password', event.currentTarget.value)}
            />
            <TextInput
              label={t('apName')}
              autocomplete="off"
              hint={apNameHint()}
              value={tab.form.ap_ssid}
              onInput={(event) => tab.setForm('ap_ssid', event.currentTarget.value)}
            />
            <TextInput
              type="password"
              label={t('apPassword')}
              autocomplete="new-password"
              hint={t('apPasswordHint') as string}
              value={tab.form.ap_password}
              onInput={(event) => tab.setForm('ap_password', event.currentTarget.value)}
            />
            <SelectInput
              label={t('apBehavior')}
              value={tab.form.ap_behavior}
              onChange={(event) => tab.setForm('ap_behavior', event.currentTarget.value)}
            >
              <option value="keep">{t('apBehaviorKeep') as string}</option>
              <option value="close_on_sta">{t('apBehaviorCloseOnSta') as string}</option>
            </SelectInput>
          </div>
        </StaticConfigBlock>
        <CollapsibleConfigBlock title={t('sectionAdvanced') as string} defaultOpen={false}>
          <div class="pt-2">
            <TextInput
              full
              label={t('timezone')}
              placeholder={t('timezonePlaceholder') as string}
              hint={timezoneHint()}
              value={tab.form.time_timezone}
              onInput={(event) => tab.setForm('time_timezone', event.currentTarget.value)}
            />
          </div>
        </CollapsibleConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};
