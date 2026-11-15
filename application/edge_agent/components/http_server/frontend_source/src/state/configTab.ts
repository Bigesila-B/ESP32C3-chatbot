import { batch, createEffect, createMemo, createSignal, onCleanup, onMount } from 'solid-js';
import { createStore, reconcile, type SetStoreFunction } from 'solid-js/store';
import type { AppConfig, ConfigGroup } from '../api/client';
import {
  appConfig,
  ensureConfigGroups,
  isGroupLoaded,
  patchConfigLocal,
  reloadConfigFields,
  reloadConfigGroups,
} from './config';
import { markDirty, type TabId } from './dirty';
import { pushToast } from './toast';
import { t } from '../i18n';
import { saveConfigPatch, type ConfigPatch } from '../api/client';

type ConfigSource =
  | { groups: ConfigGroup[]; fields?: never }
  | { groups?: never; fields: (keyof AppConfig)[] };

export type ConfigTabOptions<T extends object> = ConfigSource & {
  tab: TabId;
  toForm: (config: Partial<AppConfig>) => T;
  fromForm: (form: T) => ConfigPatch;
  isDirty?: (form: T, baseline: T) => boolean;
  saveMessage?: () => string;
};

export type ConfigTabApi<T extends object> = {
  form: T;
  setForm: SetStoreFunction<T>;
  dirty: () => boolean;
  loading: () => boolean;
  saving: () => boolean;
  error: () => string | null;
  save: () => Promise<void>;
  discard: () => void;
  reload: () => Promise<void>;
};

export function createConfigTab<T extends object>(options: ConfigTabOptions<T>): ConfigTabApi<T> {
  const initial = options.toForm(appConfig());
  const [form, setForm] = createStore<T>(initial);
  let baseline: T = clone(initial);
  const [baselineTick, setBaselineTick] = createSignal(0);
  const [saving, setSaving] = createSignal(false);
  const [loading, setLoading] = createSignal(!options.groups || !isGroupLoadedAll(options.groups));
  const [error, setError] = createSignal<string | null>(null);

  const applyBaseline = () => {
    const next = options.toForm(appConfig());
    baseline = clone(next);
    setForm(reconcile(next));
    setBaselineTick((value) => value + 1);
  };

  const load = async (refresh = false) => {
    setError(null);
    setLoading(true);
    try {
      if (options.fields) await reloadConfigFields(options.fields);
      else if (refresh) await reloadConfigGroups(options.groups);
      else await ensureConfigGroups(options.groups);
      applyBaseline();
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  };

  onMount(() => {
    load();
  });

  /* Keep the form in sync if another tab updates the same group (e.g. a
   * wechat login writes wechat_token). Only refresh the baseline when the
   * form is not dirty, so we never clobber in-progress edits. */
  createEffect(() => {
    void appConfig();
    if (options.groups ? !isGroupLoadedAll(options.groups) : loading() || error()) return;
    if (isDirtyMemo()) return;
    applyBaseline();
  });

  const isDirtyMemo = createMemo(() => {
    void baselineTick();
    return options.isDirty ? options.isDirty(form as T, baseline) : !shallowEqual(form, baseline);
  });

  createEffect(() => {
    markDirty(options.tab, isDirtyMemo());
  });

  onCleanup(() => {
    markDirty(options.tab, false);
  });

  const save = async () => {
    if (saving()) return;
    setSaving(true);
    setError(null);
    try {
      const patch = options.fromForm(form as T);
      await saveConfigPatch(patch);
      batch(() => {
        patchConfigLocal(patch);
        baseline = clone(options.toForm(appConfig()));
        setForm(reconcile(baseline));
        setBaselineTick((value) => value + 1);
        markDirty(options.tab, false);
      });
      pushToast(
        options.saveMessage ? options.saveMessage() : (t('saveSuccess') as string),
        'success',
      );
    } catch (err) {
      setError((err as Error).message);
      pushToast((err as Error).message || (t('saveError') as string), 'error', 5000);
      throw err;
    } finally {
      setSaving(false);
    }
  };

  const discard = () => {
    setForm(reconcile(clone(baseline)));
    setBaselineTick((value) => value + 1);
  };

  return {
    form,
    setForm,
    dirty: isDirtyMemo,
    loading,
    saving,
    error,
    save,
    discard,
    reload: () => load(true),
  };
}

function isGroupLoadedAll(groups: ConfigGroup[]) {
  return groups.every((group) => isGroupLoaded(group));
}

function clone<T>(value: T): T {
  if (typeof structuredClone === 'function') {
    return structuredClone(value);
  }
  return JSON.parse(JSON.stringify(value));
}

function shallowEqual(a: any, b: any): boolean {
  if (a === b) return true;
  if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return false;
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const key of ka) {
    const av = a[key];
    const bv = b[key];
    if (Array.isArray(av) && Array.isArray(bv)) {
      if (av.length !== bv.length) return false;
      for (let i = 0; i < av.length; i++) {
        if (av[i] !== bv[i]) return false;
      }
      continue;
    }
    if (av !== bv) return false;
  }
  return true;
}
