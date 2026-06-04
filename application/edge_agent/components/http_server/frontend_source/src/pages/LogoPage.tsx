import { createSignal, onMount, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import {
  fetchLogoStatus,
  uploadLogo,
  deleteLogo,
  type LogoStatus,
} from '../api/client';
import { pushToast } from '../state/toast';

function humanSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

export const LogoPage: Component = () => {
  const [status, setStatus] = createSignal<LogoStatus | null>(null);
  const [loading, setLoading] = createSignal(false);
  const [uploading, setUploading] = createSignal(false);
  const [previewKey, setPreviewKey] = createSignal(0);
  let fileInputRef: HTMLInputElement | undefined;

  const reload = async () => {
    setLoading(true);
    try {
      const s = await fetchLogoStatus();
      setStatus(s);
    } catch (err) {
      pushToast((err as Error).message, 'error');
    } finally {
      setLoading(false);
    }
  };

  onMount(() => { void reload(); });

  const handleUpload = async (e: Event) => {
    const target = e.target as HTMLInputElement;
    const file = target.files?.[0];
    if (!file) return;

    if (!file.name.toLowerCase().endsWith('.svg')) {
      pushToast('Only .svg files are accepted', 'error');
      target.value = '';
      return;
    }

    setUploading(true);
    try {
      await uploadLogo(file);
      pushToast(t('logoUploadSuccess') as string, 'success');
      setPreviewKey((k) => k + 1);
      await reload();
    } catch (err) {
      pushToast(t('logoUploadError') as string + ': ' + (err as Error).message, 'error');
    } finally {
      setUploading(false);
      if (fileInputRef) fileInputRef.value = '';
    }
  };

  const handleDelete = async () => {
    if (!window.confirm(t('logoDeleteBtn') as string + '?')) return;
    setUploading(true);
    try {
      await deleteLogo();
      pushToast(t('logoDeleteSuccess') as string, 'success');
      setPreviewKey((k) => k + 1);
      await reload();
    } catch (err) {
      pushToast(t('logoDeleteError') as string + ': ' + (err as Error).message, 'error');
    } finally {
      setUploading(false);
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('logoPageTitle') as string}
        description={t('logoPageDesc') as string}
        actions={
          <Button size="sm" variant="secondary" onClick={reload} disabled={loading()}>
            {t('sysInfoReload') as string}
          </Button>
        }
      />

      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('logoPreviewLabel') as string}>
          <div class="flex items-center justify-center p-4 rounded-[var(--radius-sm)] bg-black/20 min-h-[200px]">
            <Show
              when={status()?.exists}
              fallback={
                <span class="text-[var(--color-text-muted)] text-sm">
                  {t('logoNoPreview') as string}
                </span>
              }
            >
              <img
                src={'/logo.svg?_=' + previewKey()}
                alt="Logo preview"
                style={{
                  'max-width': '320px',
                  'max-height': '240px',
                  'width': 'auto',
                  'height': 'auto',
                }}
              />
            </Show>
          </div>
        </StaticConfigBlock>

        <StaticConfigBlock title={t('logoPageTitle') as string}>
          <div class="grid gap-3 pt-2">
            <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02]">
              <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
                Status
              </span>
              <span class={[
                'text-[0.88rem] font-semibold',
                status()?.active
                  ? 'text-emerald-400'
                  : 'text-[var(--color-text-muted)]',
              ].join(' ')}>
                {status()?.active
                  ? (t('logoStatusActive') as string)
                  : (t('logoStatusInactive') as string)}
              </span>
            </div>

            <Show when={status()?.exists}>
              <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02]">
                <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
                  {t('logoFileLabel') as string}
                </span>
                <span class="text-[0.88rem] text-[var(--color-text-primary)] font-mono">
                  {humanSize(status()?.size ?? 0)}
                </span>
              </div>
            </Show>

            <div class="flex gap-3 mt-2">
              <input
                ref={fileInputRef}
                type="file"
                accept=".svg"
                style={{ display: 'none' }}
                onChange={handleUpload}
              />
              <Button
                size="sm"
                variant="secondary"
                disabled={uploading()}
                onClick={() => fileInputRef?.click()}
              >
                {uploading() ? '...' : (t('logoUploadBtn') as string)}
              </Button>

              <Show when={status()?.exists}>
                <Button
                  size="sm"
                  variant="secondary"
                  disabled={uploading()}
                  onClick={handleDelete}
                >
                  {t('logoDeleteBtn') as string}
                </Button>
              </Show>
            </div>
          </div>
        </StaticConfigBlock>
      </div>
    </TabShell>
  );
};
