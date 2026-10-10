// dist/index.html -> ../page.html.gz, the file the firmware embeds.
// Same input, same bytes: no file name or time in the header, and the OS
// byte fixed, so a rebuild on another machine changes nothing.
import { readFileSync, writeFileSync } from 'node:fs';
import { gzipSync, constants } from 'node:zlib';

const html = readFileSync(new URL('../dist/index.html', import.meta.url));
const gz = gzipSync(html, { level: constants.Z_BEST_COMPRESSION });
gz[4] = gz[5] = gz[6] = gz[7] = 0; // MTIME
gz[9] = 0xff; // OS: unknown
writeFileSync(new URL('../../page.html.gz', import.meta.url), gz);
console.log(`page.html.gz: ${html.length} bytes -> ${gz.length} gzipped`);
