import { createUniqueId, Show, splitProps, type Component, type JSX } from 'solid-js';

type CommonFieldProps = {
  label?: JSX.Element;
  hint?: JSX.Element;
  error?: JSX.Element;
  full?: boolean;
  fieldClass?: string;
};

type FieldRendererProps = CommonFieldProps & {
  children: (api: { id: string }) => JSX.Element;
};

export const Field: Component<FieldRendererProps> = (props) => {
  const id = createUniqueId();
  return (
    <div
      class={[
        'flex flex-col gap-1.5',
        props.full ? 'sm:col-span-2' : '',
        props.fieldClass ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      <Show when={props.label}>
        <label for={id} class="text-[0.8rem] text-[var(--color-text-secondary)] font-medium">
          {props.label}
        </label>
      </Show>
      {props.children({ id })}
      <Show when={props.hint}>
        <small class="text-[0.72rem] leading-snug text-[var(--color-text-secondary)]/85">
          {props.hint}
        </small>
      </Show>
      <Show when={props.error}>
        <small class="text-[0.75rem] text-[var(--color-danger)]">{props.error}</small>
      </Show>
    </div>
  );
};

type TextInputProps = Omit<JSX.InputHTMLAttributes<HTMLInputElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const TextInput: Component<TextInputProps> = (props) => {
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
  ]);
  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <input
          id={id}
          type="text"
          autocomplete="off"
          {...rest}
          class={[
            'w-full rounded-[var(--radius-sm)] border bg-[var(--color-bg-input)] px-3 py-2 text-sm text-[var(--color-text-primary)] transition',
            local.error
              ? 'border-[var(--color-danger)]'
              : 'border-[var(--color-border-subtle)] focus:border-[rgba(232,54,45,0.4)]',
            local.inputClass ?? '',
          ]
            .filter(Boolean)
            .join(' ')}
        />
      )}
    </Field>
  );
};

type SelectInputProps = Omit<JSX.SelectHTMLAttributes<HTMLSelectElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const SelectInput: Component<SelectInputProps> = (props) => {
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
    'children',
  ]);
  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <select
          id={id}
          {...rest}
          class={[
            'w-full rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-3 py-2 text-sm text-[var(--color-text-primary)] transition focus:border-[rgba(232,54,45,0.4)]',
            local.inputClass ?? '',
          ]
            .filter(Boolean)
            .join(' ')}
        >
          {local.children}
        </select>
      )}
    </Field>
  );
};

type TextAreaProps = Omit<JSX.TextareaHTMLAttributes<HTMLTextAreaElement>, 'class'> &
  CommonFieldProps & {
    inputClass?: string;
  };

export const TextArea: Component<TextAreaProps> = (props) => {
  const [local, rest] = splitProps(props, [
    'label',
    'hint',
    'error',
    'full',
    'fieldClass',
    'inputClass',
  ]);
  return (
    <Field
      label={local.label}
      hint={local.hint}
      error={local.error}
      full={local.full}
      fieldClass={local.fieldClass}
    >
      {({ id }) => (
        <textarea
          id={id}
          spellcheck={false}
          {...rest}
          class={[
            'w-full min-h-[200px] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-4 py-3 text-[0.88rem] leading-6 text-[var(--color-text-primary)] transition font-mono focus:border-[rgba(232,54,45,0.4)]',
            local.inputClass ?? '',
          ]
            .filter(Boolean)
            .join(' ')}
        />
      )}
    </Field>
  );
};
