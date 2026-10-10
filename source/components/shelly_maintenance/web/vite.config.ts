import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { mockDevice } from './mock/device';

// One self-contained HTML file: the device serves it gzipped from flash and
// the network it runs has no internet, so nothing may load from elsewhere.
export default defineConfig({
  plugins: [svelte(), viteSingleFile({ removeViteModuleLoader: true }), mockDevice()],
  build: {
    // Svelte 5 needs Proxy; ES2019 covers Safari 13 and Chrome 73 and later,
    // including the captive-portal browsers phones open on this network.
    // This lowers syntax only: index.html polyfills the one newer method
    // Svelte calls (String.replaceAll).
    target: 'es2019',
    cssCodeSplit: false,
    sourcemap: false,
    modulePreload: { polyfill: false },
    reportCompressedSize: false,
  },
});
