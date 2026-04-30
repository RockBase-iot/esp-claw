import { createEffect, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { CollapsibleConfigBlock, StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput } from '../components/ui/FormField';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { pushToast } from '../state/toast';

type BasicForm = {
  wifi_ssid: string;
  wifi_password: string;
  time_timezone: string;
};

export const BasicPage: Component = () => {
  const tab = createConfigTab<BasicForm>({
    tab: 'basic',
    groups: ['wifi', 'time'],
    toForm: (config: Partial<AppConfig>) => ({
      wifi_ssid: config.wifi_ssid ?? '',
      wifi_password: config.wifi_password ?? '',
      time_timezone: config.time_timezone ?? '',
    }),
    fromForm: (form) => ({
      wifi_ssid: form.wifi_ssid.trim(),
      wifi_password: form.wifi_password,
      time_timezone: form.time_timezone.trim(),
    }),
  });
  const [validationError, setValidationError] = createSignal<string | null>(null);

  createEffect(() => {
    void tab.form.wifi_ssid;
    void tab.form.wifi_password;
    setValidationError(null);
  });

  const handleSave = async () => {
    const wifiSsid = tab.form.wifi_ssid.trim();
    const wifiPassword = tab.form.wifi_password;

    if (!wifiSsid) {
      const message = t('wifiValidationSsidRequired') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    if (wifiPassword.length > 0 && wifiPassword.length < 8) {
      const message = t('wifiValidationPasswordLength') as string;
      setValidationError(message);
      pushToast(message, 'error', 5000);
      return;
    }

    await tab.save();
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navBasic') as string}
        description={t('restartHint') as string}
      />
      <Show when={validationError() ?? tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={validationError() ?? tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
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
          </div>
        </StaticConfigBlock>
        <CollapsibleConfigBlock title={t('sectionAdvanced') as string} defaultOpen>
          <div class="pt-2">
            <TextInput
              full
              label={t('timezone')}
              placeholder={t('timezonePlaceholder') as string}
              hint={t('timezoneHelp') as string}
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
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
