'use strict';
const views = {
  '3d': {
    image: 'assets/native-authoring.webp', alt: '3D workspace capture pending',
    label: 'ONE SCENE. THE FULL RENDERER.', title: 'The world, in context.',
    description: 'Author with a dockable viewport, hierarchy and component inspectors. The editor runs the full render graph: GPU-driven culling, tiled lighting, shadows and post-processing.',
    link: 'https://github.com/AlexMollard/AetherCore#editor-workflow', linkText: 'Read the editor workflow ↗'
  },
  '2d': {
    image: 'assets/editor-2d.png', alt: '2D authoring capture pending',
    label: 'SAME ENGINE. ANOTHER DIMENSION.', title: 'From sheet to scene.',
    description: 'Place sprites with orthographic XY tools, slice sheets non-destructively, and author animation timing and events. Compatible re-slices preserve sprite identities. The same project structure carries your scripts, scenes and assets.',
    link: 'https://github.com/AlexMollard/AetherCore#2d-workflow', linkText: 'Explore 2D authoring ↗'
  },
  launcher: {
    image: 'assets/launcher.png', alt: 'Project launcher capture pending',
    label: 'A WORKSPACE FOR EACH PROJECT.', title: 'Start focused.',
    description: 'Create or open a project in the Launcher. It hands control to a dedicated Editor process, then closes after startup. Your project owns its scenes, scripts, prefabs, assets and publish settings.',
    link: 'https://github.com/AlexMollard/AetherCore#quick-start', linkText: 'Open your first project ↗'
  }
};
const workspacePlaceholder = document.getElementById('workspace-image').src;
const tabs = [...document.querySelectorAll('[data-view]')];
function selectView(tab) {
  const view = views[tab.dataset.view];
  for (const item of tabs) {
    const selected = item === tab;
    item.setAttribute('aria-selected', String(selected));
    item.tabIndex = selected ? 0 : -1;
  }
  document.getElementById('workspace-panel').setAttribute('aria-labelledby', tab.id);
  const image = document.getElementById('workspace-image');
  image.src = workspacePlaceholder;
  image.alt = view.alt;
  image.dataset.captureSrc = view.image;
  image.parentElement.dataset.image = workspacePlaceholder;
  image.parentElement.dataset.captureImage = view.image;
  image.parentElement.dataset.caption = view.alt;
  document.getElementById('workspace-label').textContent = view.label;
  document.getElementById('workspace-title').textContent = view.title;
  document.getElementById('workspace-description').textContent = view.description;
  const link = document.getElementById('workspace-link');
  link.href = view.link;
  link.textContent = view.linkText;
}
for (const tab of tabs) {
  tab.addEventListener('click', () => selectView(tab));
  tab.addEventListener('keydown', event => {
    let index = tabs.indexOf(tab);
    if (event.key === 'ArrowRight') index = (index + 1) % tabs.length;
    else if (event.key === 'ArrowLeft') index = (index + tabs.length - 1) % tabs.length;
    else if (event.key === 'Home') index = 0;
    else if (event.key === 'End') index = tabs.length - 1;
    else return;
    event.preventDefault();
    selectView(tabs[index]);
    tabs[index].focus();
  });
}

const dialog = document.getElementById('image-dialog');
let imageTrigger;
for (const trigger of document.querySelectorAll('[data-image]')) {
  trigger.addEventListener('click', () => {
    imageTrigger = trigger;
    const image = document.getElementById('expanded-image');
    image.src = trigger.dataset.image;
    image.dataset.captureSrc = trigger.dataset.captureImage;
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
