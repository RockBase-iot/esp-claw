import { createEffect, createSignal, lazy, onCleanup, onMount, Show, Suspense } from 'solid-js';
import type { Component } from 'solid-js';

import { Layout } from './components/layout/Layout';
import { ToastViewport } from './components/ui/ToastViewport';
import { Banner } from './components/ui/Banner';
import { t } from './i18n';
import {
  reloadCapabilities,
  reloadLuaModules,
  reloadStatus,
} from './state/config';
import { pushToast } from './state/toast';
import type { TabId } from './state/dirty';
import { anyDirty } from './state/dirty';
import { LEAF_IDS } from './components/layout/Sidebar';

const StatusPage = lazy(() => import('./pages/StatusPage').then((mod) => ({ default: mod.StatusPage })));
const BasicPage = lazy(() => import('./pages/BasicPage').then((mod) => ({ default: mod.BasicPage })));
const SearchPage = lazy(() => import('./pages/SearchPage').then((mod) => ({ default: mod.SearchPage })));
const MemoryPage = lazy(() => import('./pages/MemoryPage').then((mod) => ({ default: mod.MemoryPage })));
const LlmPage = lazy(() => import('./pages/LlmPage').then((mod) => ({ default: mod.LlmPage })));
const ImPage = lazy(() => import('./pages/ImPage').then((mod) => ({ default: mod.ImPage })));
const CapabilitiesPage = lazy(() => import('./pages/CapabilitiesPage').then((mod) => ({ default: mod.CapabilitiesPage })));
const SkillsPage = lazy(() => import('./pages/SkillsPage').then((mod) => ({ default: mod.SkillsPage })));
const FilesPage = lazy(() => import('./pages/FilesPage').then((mod) => ({ default: mod.FilesPage })));
const WebImPage = lazy(() => import('./pages/WebImPage').then((mod) => ({ default: mod.WebImPage })));

function readTabFromHash(): TabId {
  const hash = window.location.hash.replace(/^#\/?/, '') as TabId;
  return LEAF_IDS.includes(hash) ? hash : 'status';
}

const App: Component = () => {
  const [currentTab, setCurrentTab] = createSignal<TabId>(readTabFromHash());
  const [bootError, setBootError] = createSignal<string | null>(null);

  const onHashChange = () => {
    const next = readTabFromHash();
    if (next === currentTab()) return;
    if (anyDirty()) {
      const ok = window.confirm(t('unsavedConfirmLeave') as string);
      if (!ok) {
        window.location.hash = '#' + currentTab();
        return;
      }
    }
    setCurrentTab(next);
  };

  onMount(() => {
    window.addEventListener('hashchange', onHashChange);
    bootstrap();
  });

  onCleanup(() => {
    window.removeEventListener('hashchange', onHashChange);
  });

  createEffect(() => {
    window.location.hash = '#' + currentTab();
  });

  const handleSelectTab = (next: TabId) => {
    setCurrentTab(next);
  };

  const bootstrap = async () => {
    /* Only fetch the data used globally (status bar, capabilities & lua
     * module catalogues). Each tab lazily loads its own configuration
     * groups via createConfigTab. */
    const tasks: Array<[string, () => Promise<unknown>]> = [
      ['status', () => reloadStatus()],
      ['capabilities', () => reloadCapabilities()],
      ['luaModules', () => reloadLuaModules()],
    ];
    for (const [label, task] of tasks) {
      try {
        await task();
      } catch (err) {
        const message = (err as Error).message || 'Failed to initialise: ' + label;
        if (label === 'status') {
          setBootError(message);
        } else {
          pushToast(message, 'error', 4500);
        }
      }
    }
  };

  return (
    <>
      <Layout currentTab={currentTab()} onSelectTab={handleSelectTab}>
        <Show when={bootError()}>
          <div class="mb-4">
            <Banner kind="error" message={bootError() ?? undefined} />
          </div>
        </Show>
        <Suspense fallback={<div class="p-6 text-[var(--color-text-muted)]">{t('statusLoading')}</div>}>
          <Show when={currentTab() === 'status'}>
            <StatusPage />
          </Show>
          <Show when={currentTab() === 'basic'}>
            <BasicPage />
          </Show>
          <Show when={currentTab() === 'llm'}>
            <LlmPage />
          </Show>
          <Show when={currentTab() === 'im'}>
            <ImPage />
          </Show>
          <Show when={currentTab() === 'search'}>
            <SearchPage />
          </Show>
          <Show when={currentTab() === 'memory'}>
            <MemoryPage />
          </Show>
          <Show when={currentTab() === 'webim'}>
            <WebImPage />
          </Show>
          <Show when={currentTab() === 'capabilities'}>
            <CapabilitiesPage />
          </Show>
          <Show when={currentTab() === 'skills'}>
            <SkillsPage />
          </Show>
          <Show when={currentTab() === 'files'}>
            <FilesPage />
          </Show>
        </Suspense>
      </Layout>
      <ToastViewport />
    </>
  );
};

export default App;
