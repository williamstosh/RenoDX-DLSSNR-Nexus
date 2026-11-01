// Run after packaging: node tests/test_offline_guide_browser.cjs <extracted HTML>
// Reuses the comparison tests' local Playwright installation; never downloads.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const root = path.resolve(__dirname, '..');

async function main() {
  const input = process.argv[2];
  assert(input, 'Pass the HTML extracted from the Windows ZIP.');
  let playwright;
  try { playwright = require('playwright'); } catch (error) {
    if (error.code !== 'MODULE_NOT_FOUND') throw error;
    playwright = require(path.join(root, '.local/game-compare-browser/node_modules/playwright'));
  }
  const output = await fs.mkdtemp(path.join(root, 'tmp/offline-guide-browser-'));
  // Copy just the HTML, with no adjacent images/Markdown, and disable networking.
  const standalone = path.join(output, 'NR-Nexus-Guide.html');
  await fs.copyFile(path.resolve(input), standalone);
  const browser = await playwright.chromium.launch({channel: 'msedge', headless: true});
  let checks = 0;
  try {
    const context = await browser.newContext({offline: true, javaScriptEnabled: false,
      serviceWorkers: 'block', viewport: {width: 1440, height: 1000}});
    const page = await context.newPage();
    const externalRequests = [];
    const errors = [];
    page.on('request', request => {
      if (/^https?:/i.test(request.url())) externalRequests.push(request.url());
    });
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(pathToFileURL(standalone).href, {waitUntil: 'load'});
    const state = await page.evaluate(() => ({
      protocol: location.protocol, charset: document.characterSet,
      sources: [...document.querySelectorAll('main article')].map(e => [e.id, e.lang]),
      images: [...document.images].map(e => ({loaded: e.complete && e.naturalWidth > 0,
        embedded: e.src.startsWith('data:image/'), alt: e.alt})),
      chinese: document.getElementById('guide').textContent.includes('快速上手'),
      scripts: document.scripts.length,
      ids: [...document.querySelectorAll('[id]')].map(e => e.id),
      broken: [...document.querySelectorAll('a[href^="#"]')].filter(e => {
        const id = decodeURIComponent(e.hash.slice(1));
        return !document.getElementById(id) && !document.getElementsByName(id).length;
      }).map(e => e.hash)
    }));
    assert.equal(state.protocol, 'file:'); checks++;
    assert.equal(state.charset, 'UTF-8'); checks++;
    assert.deepEqual(state.sources, [['readme', 'zh-CN'], ['guide', 'zh-CN'], ['readme-en', 'en'], ['guide-en', 'en']]); checks++;
    assert(state.chinese); checks++;
    assert.equal(state.images.length, 16); checks++;
    assert(state.images.every(e => e.loaded && e.embedded && e.alt)); checks += state.images.length;
    assert.equal(state.scripts, 0); checks++;
    assert.equal(state.ids.length, new Set(state.ids).size); checks++;
    assert.deepEqual(state.broken, []); checks++;
    await page.screenshot({path: path.join(output, 'desktop.png')});
    for (const [index, name] of ['hierarch-square', 'crookback-bog'].entries()) {
      await page.locator('#readme img').nth(index).scrollIntoViewIfNeeded();
      await page.screenshot({path: path.join(output, name + '.png')});
    }
    for (const [selector, target] of [
      ['header a[href="#readme-english"]', 'readme-english'],
      ['#readme-en a[href="#guide-en-multipass"]', 'guide-en-multipass'],
      ['#guide-en a[href="#guide-contents"]', 'guide-contents'],
      ['header a[href="#readme-简体中文"]', 'readme-简体中文'],
      ['#readme a[href="#guide-zh-multipass"]', 'guide-zh-multipass']
    ]) {
      await page.locator(selector).first().click();
      assert.equal(await page.evaluate(() => decodeURIComponent(location.hash.slice(1))), target); checks++;
      const top = await page.evaluate(id => document.getElementById(id).getBoundingClientRect().top, target);
      assert(top >= 0 && top < 200, 'Navigation target hidden by header: ' + target); checks++;
    }
    await page.screenshot({path: path.join(output, 'guide.png')});
    for (const width of [1440, 900, 390, 320]) {
      await page.setViewportSize({width, height: 844});
      const geometry = await page.evaluate(() => ({
        pageWidth: document.documentElement.scrollWidth, viewport: innerWidth,
        imagesFit: [...document.images].every(e => e.getBoundingClientRect().width <= innerWidth),
        tablesScroll: [...document.querySelectorAll('table')].every(e => getComputedStyle(e).overflowX === 'auto')
      }));
      assert(geometry.pageWidth <= geometry.viewport, 'Page overflows at ' + width); checks++;
      assert(geometry.imagesFit && geometry.tablesScroll); checks++;
      if (width === 390) {
        await page.goto(pathToFileURL(standalone).href);
        await page.locator('.contents summary').click();
        assert.equal(await page.locator('.contents details').getAttribute('open'), null); checks++;
        await page.screenshot({path: path.join(output, 'mobile.png')});
      }
    }
    await page.emulateMedia({media: 'print'});
    const print = await page.evaluate(() => ({
      navHidden: getComputedStyle(document.querySelector('.contents')).display === 'none',
      paper: getComputedStyle(document.documentElement).backgroundColor,
      tables: [...document.querySelectorAll('table')].every(e => getComputedStyle(e).display === 'table'),
      code: [...document.querySelectorAll('pre code')].every(e => getComputedStyle(e).whiteSpace === 'pre-wrap')
    }));
    assert(print.navHidden && print.tables && print.code); checks++;
    assert.equal(print.paper, 'rgb(255, 255, 255)'); checks++;
    assert.deepEqual(externalRequests, []); checks++;
    assert.deepEqual(errors, []); checks++;
    const result = {checks, browser: await browser.version(), protocol: state.protocol,
      offline: true, javaScriptEnabled: false, externalRequests, errors, images: state.images.length, output};
    await fs.writeFile(path.join(output, 'result.json'), JSON.stringify(result, null, 2));
    console.log(JSON.stringify(result));
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
