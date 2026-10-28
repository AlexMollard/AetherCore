const path = require('node:path');
const fs = require('node:fs');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.AETHER_PLAYWRIGHT_MODULE
  || path.resolve(__dirname, '../../.temp/website-checks/node_modules/playwright'));
const siteUrl = process.argv[2] || 'http://127.0.0.1:8000/website/';
const outputDir = path.resolve(__dirname, '../../logs/website');
fs.mkdirSync(outputDir, { recursive: true });
(async () => {
  const browser = await chromium.launch({ headless: true });
  try {
    for (const width of [1920, 1440, 768, 390]) {
      const page = await browser.newPage({ viewport: { width, height: 900 }, deviceScaleFactor: 1 });
      const errors = [];
      const gameRequests = [];
      page.on('request', request => {
        if (new URL(request.url()).pathname.startsWith(new URL('demos/', siteUrl).pathname)) {
          gameRequests.push(request.url());
        }
      });
      page.on('pageerror', error => errors.push(error.message));
      await page.goto(siteUrl);
      await page.evaluate(() => scrollTo(0, 500));
      await page.waitForFunction(() => document.querySelector('.masthead').classList.contains('scrolled'));
      assert.equal(await page.locator('.masthead').evaluate(el => el.getBoundingClientRect().top), 0);
      await page.locator('#tab-2d').click();
      assert.match(await page.locator('#workspace-image').getAttribute('src'), /^data:image\/svg\+xml/);
      assert.equal(await page.locator('#workspace-image').getAttribute('data-capture-src'), 'assets/editor-2d.png');
      await page.locator('#tab-2d').press('ArrowRight');
      assert.equal(await page.locator('#tab-launcher').getAttribute('aria-selected'), 'true');
      await page.locator('#tab-3d').click();
      await page.locator('.workspace-shot').click();
      assert.equal(await page.locator('#image-dialog').evaluate(el => el.open), true);
      await page.keyboard.press('Escape');
      assert.equal(await page.locator('#image-dialog').evaluate(el => el.open), false);
      const systems = page.locator('.system-list details');
      for (const index of [1, 2, 3, 0]) {
        const system = systems.nth(index);
        await system.locator('summary').focus();
        await system.locator('summary').press('Enter');
        assert.equal(await system.evaluate(el => el.open), true);
        assert.equal(await page.locator('.system-list details[open]').count(), 1, 'System explorer must show one layer at a time');
        assert.equal(await system.locator('.system-body').isVisible(), true);
        await system.locator('.system-capture img').scrollIntoViewIfNeeded();
        await system.locator('.system-capture img').evaluate(img => img.decode());
        const bounds = await system.locator('.system-body').evaluate(el => ({
          bottom: el.getBoundingClientRect().bottom,
          footer: document.querySelector('.workbench-foot').getBoundingClientRect().top
        }));
        assert(bounds.bottom <= bounds.footer, `System content overlaps its footer at ${width}px`);
      }
      await systems.first().locator('summary').press('Enter');
      assert.equal(await page.locator('.system-empty').isVisible(), true);
      await systems.first().locator('summary').press('Enter');
      if (width === 390) {
        await page.locator('#menu').click();
        assert.equal(await page.locator('#menu').getAttribute('aria-expanded'), 'true');
        await page.locator('#navigation a').first().click();
        assert.equal(await page.locator('#menu').getAttribute('aria-expanded'), 'false');
      }
      for (const image of await page.locator('main img:visible').all()) {
        await image.scrollIntoViewIfNeeded();
        await image.evaluate(img => img.decode());
      }
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, `Overflow at ${width}`);
      assert.deepEqual(errors, []);
      await page.evaluate(() => scrollTo(0, 0));
      await page.waitForFunction(() => !document.querySelector('.masthead').classList.contains('scrolled'));
      await page.screenshot({ path: path.join(outputDir, `product-${width}.png`), fullPage: true });
      assert.deepEqual(gameRequests, [], 'Browsing the website must not download game packages before Play');
      console.log(`PASS ${width}px: sticky header, tabs, keyboard, dialog, disclosures, images, overflow, runtime errors${width === 390 ? ', mobile navigation' : ''}`);
      await page.close();
    }
    const reduced = await browser.newPage({ reducedMotion: 'reduce' });
    await reduced.goto(siteUrl);
    await reduced.locator('.note-row').first().hover();
    assert.equal(await reduced.evaluate(() => getComputedStyle(document.documentElement).scrollBehavior), 'auto');
    assert.equal(await reduced.locator('.note-row').first().evaluate(el => getComputedStyle(el).transform), 'none');
    assert.equal(await reduced.locator('.scroll-arrival').count(), 0);
    await reduced.close();
    const noScript = await browser.newPage({ javaScriptEnabled: false });
    await noScript.goto(siteUrl);
    await noScript.locator('.system-list details').nth(2).locator('summary').click();
    assert.equal(await noScript.locator('.system-list details').nth(2).locator('.system-body').isVisible(), true);
    await noScript.locator('.note-row').first().click();
    assert.equal(await noScript.locator('.article-content section').first().isVisible(), true);
    await noScript.close();
    console.log('PASS reduced motion and no-JavaScript system/article access');
    for (const slug of ['render-frame', 'csharp-iteration', 'editor-mcp']) {
      for (const width of [1440, 390]) {
      const page = await browser.newPage({ viewport: { width, height: 900 } });
      const errors = [];
      page.on('pageerror', error => errors.push(error.message));
      const response = await page.goto(new URL(`articles/${slug}.html`, siteUrl).href);
      if (response) assert.equal(response.ok(), true);
      await page.evaluate(() => scrollTo(0, 600));
      await page.waitForFunction(() => document.querySelector('.masthead').classList.contains('scrolled'));
      assert.equal(await page.locator('.masthead').evaluate(el => el.getBoundingClientRect().top), 0);
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      const chapter = page.locator('.article-toc a').last();
      const target = await chapter.getAttribute('href');
      await chapter.click();
      await page.waitForFunction(target => document.querySelector(`.article-toc a[href="${target}"]`).getAttribute('aria-current') === 'location', target);
      const chapterBounds = await page.locator(target).evaluate(el => ({
        top: el.getBoundingClientRect().top,
        header: document.querySelector('.masthead').getBoundingClientRect().bottom
      }));
      assert(chapterBounds.top >= chapterBounds.header, 'Sticky header obscures article chapter');
      if (width === 390) {
      await page.locator('#menu').click();
      }
      await page.locator('#navigation a[href="../#notes"]').click();
      await page.waitForURL('**/#notes');
      const bounds = await page.evaluate(() => ({
        section: document.querySelector('#notes').getBoundingClientRect().top,
        header: document.querySelector('.masthead').getBoundingClientRect().bottom
      }));
      assert(bounds.section >= bounds.header, 'Sticky header obscures anchor destination');
      assert.deepEqual(errors, []);
      console.log(`PASS article ${slug} ${width}px: chapter navigation, active chapter, sticky header, return to notes, overflow`);
      await page.close();
      }
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
