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
      const notesTop = () => page.locator('#notes').evaluate(el => el.getBoundingClientRect().top + scrollY);
      const beforeSwitch = await notesTop();
      for (const id of ['feature-2d', 'feature-audio', 'feature-networking', 'feature-agents', 'feature-graphics']) {
        if (width <= 760) await page.locator('.feature-picker select').selectOption(id);
        else await page.locator(`#${id}-tab`).click();
        assert.equal(await page.locator(`#${id}`).getAttribute('aria-hidden'), 'false');
        assert.equal(await page.locator('.feature-panel:not([inert])').count(), 1);
        assert(Math.abs(await notesTop() - beforeSwitch) < 1, 'Feature switching moves following content');
        await page.locator(`#${id} img`).scrollIntoViewIfNeeded();
        await page.locator(`#${id} img`).evaluate(img => img.decode());
      }
      if (width > 760) {
        await page.locator('#feature-graphics-tab').press('End');
        assert.equal(await page.locator('#feature-publishing-tab').getAttribute('aria-selected'), 'true');
        await page.locator('#feature-publishing-tab').press('Home');
      }
      const imageButton = page.locator('.feature-panel.is-active .image-button');
      const source = await imageButton.getAttribute('data-image');
      await imageButton.click();
      assert.equal(await page.locator('#expanded-image').getAttribute('src'), source);
      assert.equal(await page.locator('#image-dialog').evaluate(el => el.open), true);
      await page.keyboard.press('Escape');
      await page.waitForFunction(() => !document.querySelector('#image-dialog').open && document.body.style.overflow === '');
      assert.equal(await imageButton.evaluate(el => el === document.activeElement), true);
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
      console.log(`PASS ${width}px: sticky header, stable feature switching, keyboard, dialog, images, overflow, runtime errors${width === 390 ? ', mobile navigation' : ''}`);
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
    for (const panel of await noScript.locator('.feature-panel').all()) assert.equal(await panel.isVisible(), true);
    await noScript.locator('.note-row').first().click();
    assert.equal(await noScript.locator('.article-content section').first().isVisible(), true);
    await noScript.close();
    console.log('PASS reduced motion and no-JavaScript feature/article access');
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
      await page.locator('#navigation a[href="../notes.html"]').click();
      await page.waitForURL('**/notes.html');
      assert.equal(await page.locator('.notes-heading').isVisible(), true);
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      assert.deepEqual(errors, []);
      console.log(`PASS article ${slug} ${width}px: chapter navigation, active chapter, sticky header, return to notes, overflow`);
      await page.close();
      }
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
