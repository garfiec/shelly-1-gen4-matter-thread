<script lang="ts">
  import { upload } from '../lib/api';
  import Card from '../lib/Card.svelte';
  import { app } from '../lib/store.svelte';

  let files = $state<FileList | null>(null);
  let older = $state(false);
  // A file dragged over the window: anywhere on this tab takes the drop.
  let dragging = $state(false);
  const up = app.up;

  const file = $derived(files?.[0] ?? null);
  const sizeKb = $derived(file ? Math.round(file.size / 1024) : 0);

  function hasFiles(e: DragEvent) {
    return !!e.dataTransfer && Array.from(e.dataTransfer.types).includes('Files');
  }
  function over(e: DragEvent) {
    if (!hasFiles(e)) return;
    // Without this the browser opens the dropped file in place of the page.
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = app.busy ? 'none' : 'copy';
    dragging = true;
  }
  function leave(e: DragEvent) {
    // relatedTarget is null only when the drag leaves the window.
    if (!e.relatedTarget) dragging = false;
  }
  function drop(e: DragEvent) {
    if (!hasFiles(e)) return;
    e.preventDefault();
    dragging = false;
    if (!app.busy && e.dataTransfer?.files.length) files = e.dataTransfer.files;
  }

  function install() {
    if (!file) return;
    app.busy = true;
    up.pct = 0;
    up.kind = '';
    up.msg = `Uploading ${file.name}…`;
    upload(file, older, (p) => (up.pct = p)).then((r) => {
      app.busy = false;
      up.msg = r.msg;
      up.kind = r.ok ? 'good' : 'bad';
      if (r.ok) {
        up.pct = 100;
        // Restarting into the new firmware: show the note in place of the page.
        setTimeout(() => (app.done = 'installed'), 2500);
      } else {
        up.pct = -1;
      }
    });
  }
</script>

<svelte:window ondragover={over} ondragleave={leave} ondrop={drop} />

<Card title="Update firmware">
  <p class="muted small">
    Choose the <b>.ota</b> file for this device, the same file used for Matter updates. The device checks the file
    before erasing anything.
  </p>
  <label class="pick" class:has={file} class:dragging>
    <input type="file" accept=".ota" bind:files disabled={app.busy} />
    <span class="pickname">{file ? file.name : dragging ? 'Drop the .ota file' : 'Choose or drop a .ota file'}</span>
    {#if file}<span class="muted small num">{sizeKb} KB</span>{/if}
  </label>
  <label class="check">
    <input type="checkbox" bind:checked={older} disabled={app.busy} />
    <span>Allow installing an older version</span>
  </label>
  <div class="buttons">
    <button class="primary" disabled={!file || app.busy} onclick={install}>Install</button>
  </div>
  {#if up.pct >= 0}
    <div class="bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow={up.pct}>
      <div style:width="{up.pct}%"></div>
    </div>
  {/if}
  <div class="msg {up.kind}" role="status" aria-live="polite">{up.msg}</div>
  <p class="muted small" style="margin:14px 0 0">
    The LED stays solid while the file uploads and installs. Keep this page open until it says it's done.
  </p>
  <p class="muted small" style="margin:8px 0 0">
    If choosing a file does nothing, this page is in the phone's sign-in window: open http://192.168.33.1 in your
    browser instead.
  </p>
</Card>

<Card title="If this page misbehaves">
  <p class="muted small" style="margin:0">
    The <a href="/recovery">simple install page</a> does only the firmware install, with nothing shared with this page.
  </p>
</Card>

<style>
  .pick {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    border: 1.5px dashed var(--line);
    border-radius: 12px;
    padding: 16px;
    margin: 6px 0 12px;
    cursor: pointer;
    background: var(--field);
  }
  .pick.has { border-style: solid; border-color: var(--accent); }
  .pick.dragging { border-style: dashed; border-color: var(--accent); background: color-mix(in srgb, var(--accent) 12%, var(--field)); }
  .pick input { position: absolute; inset: 0; opacity: 0; cursor: pointer; width: 100%; }
  .pickname { font-weight: 600; overflow-wrap: anywhere; }
  .check { display: flex; gap: 10px; align-items: center; color: var(--muted); }
  .check input { width: 18px; height: 18px; accent-color: var(--accent); }
  .bar { height: 10px; border-radius: 6px; background: var(--line); overflow: hidden; margin-top: 14px; }
  .bar div { height: 100%; background: var(--accent); transition: width 0.2s; }
</style>
