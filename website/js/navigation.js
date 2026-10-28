'use strict';
const menu = document.getElementById('menu');
const navigation = document.getElementById('navigation');
function closeMenu() {
  navigation.classList.remove('open');
  menu.setAttribute('aria-expanded', 'false');
}
menu.addEventListener('click', () => {
  const open = navigation.classList.toggle('open');
  menu.setAttribute('aria-expanded', String(open));
});
navigation.addEventListener('click', event => { if (event.target.closest('a')) closeMenu(); });
document.addEventListener('keydown', event => { if (event.key === 'Escape') closeMenu(); });
matchMedia('(min-width: 761px)').addEventListener('change', event => { if (event.matches) closeMenu(); });

const header = document.querySelector('.masthead');
const trackedNavigations = [navigation, document.querySelector('.article-toc')]
  .filter(Boolean)
  .map(root => ({
    links: [...root.querySelectorAll('a[href^="#"]')]
      .map(link => ({ link, section: document.getElementById(link.hash.slice(1)) }))
      .filter(item => item.section),
    activeLink: null
  }));
let framePending = false;
function updateScrollFeedback() {
  framePending = false;
  const distance = document.documentElement.scrollHeight - window.innerHeight;
  const progress = distance > 0 ? Math.min(1, Math.max(0, window.scrollY / distance)) : 0;
  const readingLine = Math.max(header.offsetHeight + 30, window.innerHeight * 0.3);
  header.classList.toggle('scrolled', window.scrollY > 32);
  header.style.setProperty('--reading-progress', progress);
  for (const group of trackedNavigations) {
    const current = group.links.find(({ section }) => {
      const bounds = section.getBoundingClientRect();
      return bounds.top <= readingLine && bounds.bottom > readingLine;
    })?.link || null;
    if (current !== group.activeLink) {
      group.activeLink?.removeAttribute('aria-current');
      current?.setAttribute('aria-current', 'location');
      group.activeLink = current;
    }
  }
}
function scheduleScrollFeedback() {
  if (framePending) return;
  framePending = true;
  requestAnimationFrame(updateScrollFeedback);
}
window.addEventListener('scroll', scheduleScrollFeedback, { passive: true });
window.addEventListener('resize', scheduleScrollFeedback);
window.addEventListener('pageshow', scheduleScrollFeedback);
new ResizeObserver(scheduleScrollFeedback).observe(document.body);
updateScrollFeedback();

// Content is visible without JavaScript. Only animate its first entrance.
if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
  const arrivals = new IntersectionObserver(entries => {
    for (const entry of entries) {
      if (!entry.isIntersecting) continue;
      entry.target.classList.add('scroll-arrival');
      arrivals.unobserve(entry.target);
    }
  }, { threshold: 0.08 });
  document.querySelectorAll('.section-pad > *').forEach(element => arrivals.observe(element));
}
