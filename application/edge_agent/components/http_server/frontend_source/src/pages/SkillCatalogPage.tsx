import { createResource, For, Show, type Component } from 'solid-js';
import { fetchSkillCatalog } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { Banner } from '../components/ui/Banner';
import { Button } from '../components/ui/Button';
import { PageHeader } from '../components/ui/PageHeader';
import { t, tf } from '../i18n';

export const SkillCatalogPage: Component = () => {
  const [catalog, { refetch }] = createResource(fetchSkillCatalog);

  return (
    <TabShell>
      <PageHeader
        title={t('navSkillCatalog')}
        description={t('skillCatalogDescription')}
        actions={
          <Button variant="secondary" size="sm" disabled={catalog.loading} onClick={() => void refetch()}>
            {t('commandRefresh')}
          </Button>
        }
      />
      <div class="space-y-4 px-5 py-4">
        <Show when={catalog.loading}>
          <p class="m-0 text-sm text-[var(--color-text-muted)]">{t('statusLoading')}</p>
        </Show>
        <Show when={!catalog.loading}>
          <Show when={!catalog.error} fallback={<Banner kind="error" message={t('skillCatalogFailed')} />}>
            <Show when={catalog()?.skills.length} fallback={<Banner message={t('skillCatalogEmpty')} />}>
              <ul class="m-0 list-none divide-y divide-[var(--color-border-subtle)] p-0">
                <For each={catalog()?.skills}>
                  {(skill) => (
                    <li class="space-y-1.5 py-3">
                      <code class="break-all text-sm font-semibold">{skill.id}</code>
                      <p class="m-0 break-words text-sm text-[var(--color-text-secondary)]">
                        {skill.id === 'cap_micropython' ? t('skillCatalogPython') : skill.summary}
                      </p>
                      <Show when={skill.unavailable_groups.length}>
                        <p class="m-0 break-words text-xs text-[var(--color-text-muted)]">
                          {tf('skillCatalogUnavailable', { groups: skill.unavailable_groups.join(', ') })}
                        </p>
                      </Show>
                    </li>
                  )}
                </For>
              </ul>
            </Show>
          </Show>
        </Show>
        <p class="m-0 text-sm text-[var(--color-text-secondary)]">
          {t('skillCatalogUseChat')}{' '}
          <a class="underline" href="#webim">{t('navWebIm')}</a>
        </p>
        <p class="m-0 text-sm text-[var(--color-text-secondary)]">
          {t('skillCatalogFiles')}{' '}
          <a class="underline" href="#files">{t('navFiles')}</a>
        </p>
      </div>
    </TabShell>
  );
};
