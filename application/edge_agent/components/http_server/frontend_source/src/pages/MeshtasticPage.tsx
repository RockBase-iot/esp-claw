import { LoaderCircle, RefreshCw, Trash2 } from 'lucide-solid';
import {
  createSignal,
  For,
  onCleanup,
  onMount,
  Show,
  type Component,
} from 'solid-js';
import {
  clearMeshMessages,
  fetchMeshMessages,
  fetchMeshStatus,
  fetchMeshImChannels,
  setMeshImChannel,
  type MeshImChannel,
  type MeshMessage,
  type MeshStatus,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { t } from '../i18n';
import { pushToast } from '../state/toast';

function formatTime(ts?: number): string {
  if (!ts) return '--';
  const ms = ts > 1e12 ? ts : ts * 1000;
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return '--';
  const p = (n: number) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(
    d.getMinutes(),
  )}:${p(d.getSeconds())}`;
}

function formatSize(bytes?: number): string {
  if (!bytes) return '0 B';
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(1) + ' MB';
}

export const MeshtasticPage: Component = () => {
  const [status, setStatus] = createSignal<MeshStatus | null>(null);
  const [messages, setMessages] = createSignal<MeshMessage[]>([]);
  const [loading, setLoading] = createSignal(false);
  const [autoRefresh, setAutoRefresh] = createSignal(true);
  const [limit, setLimit] = createSignal(500);
  const [expanded, setExpanded] = createSignal<Set<number>>(new Set());
  const [imChannels, setImChannels] = createSignal<MeshImChannel[]>([]);
  const [imBusy, setImBusy] = createSignal<string | null>(null);
  let timer: ReturnType<typeof setInterval> | null = null;

  const loadAll = async () => {
    setLoading(true);
    try {
      const [st, msgs, ims] = await Promise.all([
        fetchMeshStatus().catch(() => null),
        fetchMeshMessages(limit()).catch(() => [] as MeshMessage[]),
        fetchMeshImChannels().catch(() => [] as MeshImChannel[]),
      ]);
      if (st) setStatus(st);
      setMessages(msgs);
      setImChannels(ims);
    } finally {
      setLoading(false);
    }
  };

  const toggleImChannel = async (channel: string, enabled: boolean) => {
    setImBusy(channel);
    try {
      await setMeshImChannel(channel, enabled);
      setImChannels((prev) =>
        prev.map((c) => (c.channel === channel ? { ...c, enabled } : c)),
      );
    } catch (err) {
      pushToast((err as Error).message || (t('meshImUpdateFail') as string), 'error', 3500);
    } finally {
      setImBusy(null);
    }
  };

  const toggleDetail = (index: number) => {
    const next = new Set(expanded());
    if (next.has(index)) next.delete(index);
    else next.add(index);
    setExpanded(next);
  };

  const handleClear = async () => {
    if (!window.confirm(t('meshClearConfirm') as string)) return;
    try {
      await clearMeshMessages();
      pushToast(t('meshClearOk') as string, 'success', 2500);
      void loadAll();
    } catch (err) {
      pushToast((err as Error).message || (t('meshClearFail') as string), 'error', 3500);
    }
  };

  const startAuto = () => {
    stopAuto();
    if (autoRefresh()) {
      timer = setInterval(() => void loadAll(), 10000);
    }
  };
  const stopAuto = () => {
    if (timer) {
      clearInterval(timer);
      timer = null;
    }
  };

  const onToggleAuto = (next: boolean) => {
    setAutoRefresh(next);
    startAuto();
  };

  onMount(() => {
    void loadAll();
    startAuto();
  });
  onCleanup(stopAuto);

  return (
    <TabShell>
      <PageHeader
        title={t('meshTitle') as string}
        description={t('meshDesc') as string}
        actions={
          <>
            <label class="inline-flex items-center gap-1.5 text-[0.8rem] text-[var(--color-text-muted)]">
              {t('meshLimit')}
              <input
                type="number"
                min="1"
                max="2000"
                value={limit()}
                onInput={(e) => {
                  const v = parseInt(e.currentTarget.value, 10);
                  setLimit(Number.isFinite(v) && v > 0 ? Math.min(v, 2000) : 500);
                }}
                class="w-20 bg-[var(--color-bg-surface)] border border-[var(--color-border-subtle)] rounded-[var(--radius-sm)] px-2 py-1 text-[var(--color-text-primary)]"
              />
            </label>
            <label class="inline-flex items-center gap-1.5 text-[0.8rem] text-[var(--color-text-muted)] cursor-pointer">
              <input
                type="checkbox"
                checked={autoRefresh()}
                onChange={(e) => onToggleAuto(e.currentTarget.checked)}
              />
              {t('meshAutoRefresh')}
            </label>
            <Button variant="secondary" size="sm" onClick={() => void loadAll()} disabled={loading()}>
              <Show when={loading()} fallback={<RefreshCw class="w-4 h-4" />}>
                <LoaderCircle class="w-4 h-4 animate-spin" />
              </Show>
              {t('meshRefresh')}
            </Button>
            <Button variant="danger-ghost" size="sm" onClick={() => void handleClear()}>
              <Trash2 class="w-4 h-4" />
              {t('meshClear')}
            </Button>
          </>
        }
      />

      <div class="flex flex-wrap gap-x-6 gap-y-2 px-6 py-4 border-b border-[var(--color-border-subtle)] text-[0.82rem]">
        <span class="text-[var(--color-text-muted)]">
          {t('meshRadio')}:{' '}
          <span
            class="inline-block w-2 h-2 rounded-full align-middle mr-1"
            style={{
              background: status()?.connected ? 'var(--color-success, #3fb950)' : 'var(--color-danger, #f85149)',
            }}
          />
          <span class="text-[var(--color-text-primary)] font-semibold">
            {status()?.connected ? t('meshConnected') : t('meshDisconnected')}
          </span>
        </span>
        <span class="text-[var(--color-text-muted)]">
          {t('meshMessages')}:{' '}
          <span class="text-[var(--color-text-primary)] font-semibold">
            {status()?.message_count ?? 0}
          </span>
        </span>
        <span class="text-[var(--color-text-muted)]">
          {t('meshStore')}:{' '}
          <span class="text-[var(--color-text-primary)] font-semibold">
            {formatSize(status()?.file_size_bytes)}
          </span>
        </span>
        <span class="text-[var(--color-text-muted)] min-w-0 break-all">
          {t('meshPath')}:{' '}
          <span class="text-[var(--color-text-primary)] font-semibold">
            {status()?.store_path ?? '--'}
          </span>
        </span>
      </div>

      <div class="flex flex-wrap items-center gap-x-5 gap-y-2 px-6 py-4 border-b border-[var(--color-border-subtle)] text-[0.82rem]">
        <span class="text-[var(--color-text-muted)]">{t('meshImPush')}:</span>
        <Show
          when={imChannels().length > 0}
          fallback={<span class="text-[var(--color-text-muted)]">{t('meshImNone')}</span>}
        >
          <For each={imChannels()}>
            {(c) => (
              <label
                class="inline-flex items-center gap-1.5 transition-colors"
                classList={{
                  'cursor-pointer text-[var(--color-green)]': imBusy() !== c.channel,
                  'opacity-60 cursor-wait text-[var(--color-green)]': imBusy() === c.channel,
                }}
                title={
                  !c.has_target
                    ? (t('meshImNoTarget') as string)
                    : ''
                }
              >
                <input
                  type="checkbox"
                  checked={c.enabled}
                  disabled={imBusy() === c.channel}
                  class="accent-[var(--color-green)]"
                  onChange={(e) => void toggleImChannel(c.channel, e.currentTarget.checked)}
                />
                <span class="text-current font-medium">
                  {t(('meshImCh_' + c.channel) as never) || c.channel}
                </span>
                <Show when={c.enabled && !c.has_target}>
                  <span class="text-[var(--color-warning,#d29922)] text-[0.7rem]">
                    ({t('meshImNoTarget')})
                  </span>
                </Show>
              </label>
            )}
          </For>
        </Show>
      </div>

      <div class="overflow-x-auto">
        <table class="w-full border-collapse text-[0.82rem]">
          <thead>
            <tr class="text-left text-[var(--color-accent)] bg-white/[0.02]">
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColIndex')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColSender')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColFromNum')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColPacketId')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColChannel')}</th>
              <th class="px-3 py-2.5">{t('meshColMessage')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColTime')}</th>
              <th class="px-3 py-2.5 whitespace-nowrap">{t('meshColMore')}</th>
            </tr>
          </thead>
          <tbody>
            <Show
              when={messages().length > 0}
              fallback={
                <tr>
                  <td colspan="8" class="px-3 py-10 text-center text-[var(--color-text-muted)]">
                    {loading() ? t('meshLoading') : t('meshEmpty')}
                  </td>
                </tr>
              }
            >
              <For each={messages()}>
                {(m, i) => (
                  <>
                    <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.03]">
                      <td class="px-3 py-2 align-top text-[var(--color-text-muted)]">{i() + 1}</td>
                      <td class="px-3 py-2 align-top text-[var(--color-text-primary)] break-all">
                        {m.from ?? '--'}
                      </td>
                      <td class="px-3 py-2 align-top text-[var(--color-text-secondary)]">
                        {m.from_num ?? '--'}
                      </td>
                      <td class="px-3 py-2 align-top text-[var(--color-text-secondary)]">
                        {m.packet_id ?? '--'}
                      </td>
                      <td class="px-3 py-2 align-top text-[var(--color-text-secondary)]">
                        {m.channel ?? '--'}
                      </td>
                      <td class="px-3 py-2 align-top text-[var(--color-text-primary)] break-words max-w-md">
                        {m.text ?? ''}
                      </td>
                      <td class="px-3 py-2 align-top whitespace-nowrap text-[var(--color-text-muted)]">
                        {formatTime(m.ts)}
                      </td>
                      <td class="px-3 py-2 align-top">
                        <Button variant="ghost" size="xs" onClick={() => toggleDetail(i())}>
                          JSON
                        </Button>
                      </td>
                    </tr>
                    <Show when={expanded().has(i())}>
                      <tr class="border-t border-[var(--color-border-subtle)] bg-black/20">
                        <td colspan="8" class="px-3 py-2">
                          <pre class="text-[0.75rem] text-[var(--color-text-secondary)] overflow-auto max-h-48 p-3 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]">
                            {JSON.stringify(m, null, 2)}
                          </pre>
                        </td>
                      </tr>
                    </Show>
                  </>
                )}
              </For>
            </Show>
          </tbody>
        </table>
      </div>
    </TabShell>
  );
};
