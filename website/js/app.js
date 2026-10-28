'use strict';
const explorer = document.querySelector('.feature-explorer');
const panels = [...explorer.querySelectorAll('.feature-panel')];
const featureTabs = explorer.querySelector('.feature-tabs');
const picker = explorer.querySelector('.feature-picker select');
const categoryButtons = panels.map(panel => {
  const button = document.createElement('button');
  button.type = 'button';
  button.id = `${panel.id}-tab`;
  button.textContent = panel.querySelector('h3').textContent;
  button.setAttribute('role', 'tab');
  button.setAttribute('aria-controls', panel.id);
  featureTabs.append(button);
  picker.add(new Option(button.textContent, panel.id));
  panel.setAttribute('role', 'tabpanel');
  panel.setAttribute('aria-labelledby', button.id);
  panel.tabIndex = 0;
  return button;
});
function selectFeature(index) {
  panels.forEach((panel, i) => {
    const selected = i === index;
    panel.classList.toggle('is-active', selected);
    panel.inert = !selected;
    panel.setAttribute('aria-hidden', String(!selected));
    categoryButtons[i].setAttribute('aria-selected', String(selected));
    categoryButtons[i].tabIndex = selected ? 0 : -1;
  });
  picker.value = panels[index].id;
}
categoryButtons.forEach((button, index) => {
  button.addEventListener('click', () => selectFeature(index));
  button.addEventListener('keydown', event => {
    let next = index;
    if (event.key === 'ArrowDown') next = (index + 1) % panels.length;
    else if (event.key === 'ArrowUp') next = (index + panels.length - 1) % panels.length;
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = panels.length - 1;
    else return;
    event.preventDefault();
    selectFeature(next);
    categoryButtons[next].focus({ preventScroll: true });
  });
});
picker.addEventListener('change', () => selectFeature(picker.selectedIndex));
selectFeature(0);
explorer.classList.add('is-enhanced');
explorer.querySelector('.feature-controls').hidden = false;

const dialog = document.getElementById('image-dialog');
let imageTrigger;
for (const trigger of document.querySelectorAll('[data-image]')) {
  trigger.addEventListener('click', () => {
    imageTrigger = trigger;
    const image = document.getElementById('expanded-image');
    image.src = trigger.dataset.image;
    image.alt = trigger.dataset.caption;
    document.getElementById('image-caption').textContent = trigger.dataset.caption;
    dialog.showModal();
    document.body.style.overflow = 'hidden';
  });
}
document.getElementById('close-image').addEventListener('click', () => dialog.close());
dialog.addEventListener('click', event => {
  if (event.target !== dialog) return;
  const box = dialog.getBoundingClientRect();
  if (event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom) dialog.close();
});
dialog.addEventListener('close', () => {
  document.body.style.overflow = '';
  imageTrigger?.focus({ preventScroll: true });
});
