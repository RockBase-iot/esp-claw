import { createMemo, createSignal, For, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import type { AppConfig, LuaModuleItem } from '../api/client';
import { appCapabilities, appConfig, appLuaModules } from '../state/config';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { SavePanel } from '../components/ui/SavePanel';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';

const SENTINEL_NONE = '__none__';

type SkillForm = { enabled: string[] };

function parseModules(serialized: string, items: LuaModuleItem[], fallbackAll: boolean): string[] {
  const raw = (serialized || '').trim();
  if (!raw) {
    return fallbackAll ? items.map((item) => item.module_id) : [];
  }
  if (raw === SENTINEL_NONE || raw === 'none') return [];
  return raw
    .split(',')
    .map((token) => token.trim())
    .filter((token) => token && token !== SENTINEL_NONE && token !== 'none');
}

function serializeModules(selected: string[], items: LuaModuleItem[]): string {
  if (items.length === 0) return '';
  const set = new Set(selected);
  if (set.size === items.length) return '';
  if (set.size === 0) return SENTINEL_NONE;
  return Array.from(set).join(',');
}

function isCapLuaEnabled(): boolean {
  const raw = (appConfig().enabled_cap_groups || '').trim();
  if (!raw) {
    return appCapabilities().some((item) => item.group_id === 'cap_lua');
  }
  if (raw === SENTINEL_NONE || raw === 'none') return false;
  return raw
    .split(',')
    .map((token) => token.trim())
    .includes('cap_lua');
}

export const SkillsPage: Component = () => {
  const tab = createConfigTab<SkillForm>({
    tab: 'skills',
    groups: ['skills', 'capabilities'],
    toForm: (config: Partial<AppConfig>) => ({
      enabled: parseModules(config.enabled_lua_modules ?? '', appLuaModules(), true),
    }),
    fromForm: (form) => ({
      enabled_lua_modules: serializeModules(form.enabled, appLuaModules()),
    }),
    isDirty: (form, baseline) => {
      const a = new Set(form.enabled);
      const b = new Set(baseline.enabled);
      if (a.size !== b.size) return true;
      for (const v of a) if (!b.has(v)) return true;
      return false;
    },
  });

  const [search, setSearch] = createSignal('');
  const enabledSet = createMemo(() => new Set(tab.form.enabled));

  const filtered = createMemo(() => {
    const keyword = search().trim().toLowerCase();
    return appLuaModules().filter((item) => {
      if (!keyword) return true;
      return (
        item.module_id.toLowerCase().includes(keyword) ||
        (item.display_name ?? '').toLowerCase().includes(keyword)
      );
    });
  });

  const toggle = (id: string, checked: boolean) => {
    const set = new Set(tab.form.enabled);
    if (checked) set.add(id);
    else set.delete(id);
    tab.setForm('enabled', Array.from(set));
  };

  const selectAll = () => {
    tab.setForm('enabled', appLuaModules().map((item) => item.module_id));
  };
  const clearAll = () => {
    tab.setForm('enabled', []);
  };

  const summary = () =>
    tf('skillsSummary', {
      enabled: tab.form.enabled.length,
      total: appLuaModules().length,
    });

  return (
    <TabShell>
      <PageHeader
        title={t('sectionSkills') as string}
        description={t('skillsDescription') as string}
      />
      <div class="px-5 py-4 flex flex-col gap-3 border-b border-[var(--color-border-subtle)]">
        <Show when={!isCapLuaEnabled()}>
          <Banner kind="info" message={t('skillsCapabilityRequired') as string} />
        </Show>
        <div class="flex flex-wrap gap-2 items-center">
          <input
            type="search"
            placeholder={t('skillsSearchPlaceholder') as string}
            class="flex-1 min-w-[160px] max-w-xs"
            value={search()}
            onInput={(event) => setSearch(event.currentTarget.value)}
          />
          <Button size="sm" variant="secondary" onClick={selectAll} disabled={!isCapLuaEnabled()}>
            {t('skillsSelectAll')}
          </Button>
          <Button size="sm" variant="secondary" onClick={clearAll} disabled={!isCapLuaEnabled()}>
            {t('skillsClearAll')}
          </Button>
        </div>
        <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">{summary()}</p>
      </div>

      <div class="p-5">
        <Show when={appLuaModules().length === 0}>
          <Banner kind="info" message={t('skillsLoading') as string} />
        </Show>
        <Show when={appLuaModules().length > 0 && filtered().length === 0}>
          <Banner kind="info" message={t('skillsNoResult') as string} />
        </Show>
        <div class="grid gap-2 sm:grid-cols-2">
          <For each={filtered()}>
            {(item) => (
              <div class="flex items-start gap-3 p-3 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-white/[0.02] hover:bg-white/[0.04] transition">
                <div class="flex-1 min-w-0">
                  <div class="font-semibold text-[0.9rem] text-[var(--color-text-primary)]">
                    {item.display_name || item.module_id}
                  </div>
                  <div class="text-[0.76rem] text-[var(--color-text-muted)] font-mono break-all">
                    {item.module_id}
                  </div>
                </div>
                <Switch
                  checked={enabledSet().has(item.module_id)}
                  disabled={!isCapLuaEnabled()}
                  onChange={(checked) => toggle(item.module_id, checked)}
                />
              </div>
            )}
          </For>
        </div>
      </div>

      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => tab.save().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
