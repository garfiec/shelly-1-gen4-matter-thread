<script lang="ts">
  import { onMount } from 'svelte';
  import { act, MODEL_NAMES } from './lib/api';
  import { app, start, TABS } from './lib/store.svelte';
  import PendingBanner from './lib/PendingBanner.svelte';
  import Home from './tabs/Home.svelte';
  import Pairing from './tabs/Pairing.svelte';
  import Switch from './tabs/Switch.svelte';
  import MeterTab from './tabs/Meter.svelte';
  import Update from './tabs/Update.svelte';
  import Diag from './tabs/Diag.svelte';

  onMount(start);

  const s = $derived(app.state);
  const title = $derived(s ? (MODEL_NAMES[s.device.hw] ?? `Shelly ${s.device.hw}`) : 'Shelly');
  const tabs = $derived(s ? TABS.filter((t) => t.shown(s)) : TABS.slice(0, 1));

  // Keep the open tab in view in the scrolling tab bar on a narrow screen.
  let nav = $state<HTMLElement>();
  $effect(() => {
    app.tab;
    nav?.querySelector('a.active')?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  });

  // Exit restarts the device: a second tap within 4 s confirms it.
  let armed = $state(false);
  let disarm: ReturnType<typeof setTimeout> | undefined;
  function exit() {
    if (!armed) {
      armed = true;
      disarm = setTimeout(() => (armed = false), 4000);
      return;
    }
    clearTimeout(disarm);
    armed = false;
    // Only a reply means it restarts: otherwise the device is still in Wi-Fi mode.
    act({ action: 'exit' }).then((r) => {
      if (r.ok) app.done = 'exit';
      else app.offline = true;
    });
  }
</script>

<header class="top">
  <div class="bar">
    <div class="who">
      <div class="title">{title}</div>
      <div class="sub">Wi-Fi mode{s ? ` · ${s.device.variant} ${s.device.version}` : ''}</div>
    </div>
    {#if !app.done}
      <button class="exit" class:armed onclick={exit}>{armed ? 'Tap again to restart' : 'Exit'}</button>
    {/if}
  </div>
  {#if !app.done}
    <nav class="tabs" aria-label="Sections" bind:this={nav}>
      {#each tabs as t (t.id)}
        <a href="#{t.id}" class:active={app.tab === t.id} aria-current={app.tab === t.id ? 'page' : undefined}>{t.label}</a>
      {/each}
    </nav>
  {/if}
  {#if app.offline && !app.done}
    <div class="offline" role="alert">
      No reply from the device. It may be restarting, or this phone left its Wi-Fi network. To install firmware anyway,
      use the <a href="/recovery">simple install page</a>.
    </div>
  {/if}
  {#if !app.done}<PendingBanner />{/if}
  {#if app.state?.crashed && !app.done}
    <div class="offline" role="alert">
      The device started in Wi-Fi mode by itself because it failed to start normally 3 times in a row. Choose another
      device type on the Switch tab, or install firmware on the Update tab.
    </div>
  {/if}
</header>

<main>
  {#if app.done}
    <section class="done">
      <h2>{app.done === 'exit' ? 'Restarting' : 'Installed'}</h2>
      {#if app.done === 'installed' && app.up.msg}<p>{app.up.msg}.</p>{/if}
      <p class="muted">
        The device is going back to normal operation{app.done === 'installed' ? ' with the new firmware' : ''}. You can close this page.
      </p>
    </section>
  {:else if !s}
    <p class="muted loading">
      Connecting to the device…<br />
      <span class="small">If this doesn't load, the <a href="/recovery">simple install page</a> still installs firmware.</span>
    </p>
  {:else if app.tab === 'home'}
    <Home />
  {:else if app.tab === 'pairing'}
    <Pairing />
  {:else if app.tab === 'switch'}
    <Switch />
  {:else if app.tab === 'meter'}
    <MeterTab />
  {:else if app.tab === 'update'}
    <Update />
  {:else if app.tab === 'diag'}
    <Diag />
  {/if}
</main>

<style>
  .top {
    position: sticky;
    top: 0;
    z-index: 10;
    background: var(--brand);
    color: var(--brand-ink);
    padding-top: env(safe-area-inset-top);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
  }
  .bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    max-width: 44rem;
    margin: 0 auto;
    padding: 12px 16px 6px;
  }
  .who { min-width: 0; }
  .title { font-size: 1.15rem; font-weight: 650; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .sub { font-size: 0.85rem; color: var(--brand-dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .exit {
    flex: none;
    background: rgba(255, 255, 255, 0.14);
    border-color: rgba(255, 255, 255, 0.35);
    color: #fff;
    padding: 8px 14px;
    min-height: 40px;
  }
  .exit.armed { background: #fff; color: var(--bad); border-color: #fff; }
  .tabs {
    display: flex;
    gap: 4px;
    max-width: 44rem;
    margin: 0 auto;
    padding: 0 8px;
    overflow-x: auto;
    scrollbar-width: none;
    -webkit-overflow-scrolling: touch;
    /* A hint that more tabs are off to the right on a narrow screen. */
    -webkit-mask-image: linear-gradient(to right, #000 calc(100% - 24px), transparent);
    mask-image: linear-gradient(to right, #000 calc(100% - 24px), transparent);
  }
  .tabs::-webkit-scrollbar { display: none; }
  .tabs a {
    flex: none;
    color: var(--brand-dim);
    text-decoration: none;
    font-weight: 550;
    padding: 10px 12px 9px;
    border-bottom: 3px solid transparent;
    white-space: nowrap;
  }
  .tabs a.active { color: #fff; border-bottom-color: #fff; }
  .offline { background: var(--bad); color: #fff; padding: 8px 16px; font-size: 0.92rem; text-align: center; }
  .offline a { color: #fff; font-weight: 600; }
  main { max-width: 44rem; margin: 0 auto; padding: 16px 16px calc(32px + env(safe-area-inset-bottom)); }
  .loading { text-align: center; padding: 40px 0; }
  .done { text-align: center; padding: 48px 8px; }
</style>
