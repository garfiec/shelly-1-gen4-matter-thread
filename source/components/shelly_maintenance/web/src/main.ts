import { mount } from 'svelte';
import App from './App.svelte';
import './theme.css';

declare global {
  interface Window {
    __shellyWatchdog?: ReturnType<typeof setTimeout>;
    __shellyFailed?: boolean;
  }
}

mount(App, { target: document.getElementById('app')! });
// Started: drop the fallback text and stand the watchdog down.
clearTimeout(window.__shellyWatchdog);
document.getElementById('boot')?.remove();
// A slow phone may have passed the watchdog's 4 s: hide its banner unless a
// real error showed it.
if (!window.__shellyFailed) document.getElementById('oops')!.style.display = 'none';
