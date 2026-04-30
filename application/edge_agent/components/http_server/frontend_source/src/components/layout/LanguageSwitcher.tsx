import { Languages } from 'lucide-solid';
import { createEffect, createSignal, For, onCleanup } from 'solid-js';
import { currentLocale, LOCALES, setLocale } from '../../i18n';

export function LanguageSwitcher() {
  const [open, setOpen] = createSignal(false);
  let rootRef: HTMLDivElement | undefined;

  const handleDocClick = (event: MouseEvent) => {
    if (rootRef && !rootRef.contains(event.target as Node)) {
      setOpen(false);
    }
  };

  createEffect(() => {
    if (open()) {
      document.addEventListener('click', handleDocClick);
    } else {
      document.removeEventListener('click', handleDocClick);
    }
  });

  onCleanup(() => document.removeEventListener('click', handleDocClick));

  const currentLabel = () => LOCALES.find((loc) => loc.id === currentLocale())?.label ?? 'English';

  return (
    <div class="relative" ref={rootRef}>
      <button
        type="button"
        class="inline-flex items-center gap-1 text-[0.78rem] text-[var(--color-text-secondary)] px-2.5 py-1.5 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] hover:bg-white/[0.04] hover:border-white/[0.12]"
        onClick={() => setOpen(!open())}
        aria-expanded={open()}
      >
        <Languages class="w-4 h-4" />
        <span>{currentLabel()}</span>
        <svg width="10" height="10" viewBox="0 0 10 10" fill="none" class="opacity-75">
          <path d="M2.5 4L5 6.5L7.5 4" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round" />
        </svg>
      </button>
      {open() && (
        <ul class="absolute right-0 top-[calc(100%+6px)] min-w-[9rem] py-1 list-none bg-[var(--color-bg-card)] border border-[var(--color-border-subtle)] rounded-[var(--radius-md)] shadow-[0_12px_40px_rgba(0,0,0,0.45)] z-50">
          <For each={LOCALES}>
            {(locale) => (
              <li>
                <button
                  type="button"
                  class={[
                    'w-full text-left px-3.5 py-1.5 text-[0.82rem] hover:bg-white/[0.06] hover:text-[var(--color-text-primary)] transition',
                    locale.id === currentLocale()
                      ? 'text-[var(--color-accent-soft)] font-semibold'
                      : 'text-[var(--color-text-secondary)]',
                  ].join(' ')}
                  disabled={locale.id === currentLocale()}
                  onClick={() => {
                    setLocale(locale.id);
                    setOpen(false);
                  }}
                >
                  {locale.label}
                </button>
              </li>
            )}
          </For>
        </ul>
      )}
    </div>
  );
}
