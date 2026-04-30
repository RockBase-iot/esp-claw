import { For } from 'solid-js';
import { Portal } from 'solid-js/web';
import { dismissToast, toastList } from '../../state/toast';

export function ToastViewport() {
  return (
    <Portal>
      <div class="fixed top-4 right-4 z-[2000] flex flex-col gap-2 max-w-xs">
        <For each={toastList()}>
          {(toast) => (
            <div
              onClick={() => dismissToast(toast.id)}
              class={[
                'px-4 py-3 rounded-[var(--radius-md)] text-[0.85rem] shadow-[0_12px_40px_rgba(0,0,0,0.45)] cursor-pointer border',
                toast.kind === 'success'
                  ? 'bg-[var(--color-green-dim)] border-[rgba(104,211,145,0.2)] text-[var(--color-green)]'
                  : toast.kind === 'error'
                  ? 'bg-[var(--color-accent-dim)] border-[var(--color-border-accent)] text-[var(--color-danger)]'
                  : 'bg-[var(--color-bg-card)] border-[var(--color-border-subtle)] text-[var(--color-text-primary)]',
              ].join(' ')}
              role="status"
            >
              {toast.message}
            </div>
          )}
        </For>
      </div>
    </Portal>
  );
}
