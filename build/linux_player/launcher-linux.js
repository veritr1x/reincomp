async function request(action, value) {
  const response = await window.webkit.messageHandlers.bunker.postMessage(JSON.stringify({action, value}));
  return JSON.parse(response);
}

async function prefs(changes) {
  settings = await request('preferences', changes?.assetPort);
}

async function update() {
  const state = await request('state');
  el('files').textContent = state.ready && state.archive ? 'Archive selected' : words[state.ready ? 'chip.ready' : 'chip.no_files'];
  el('files').classList.toggle('good', state.ready);
  el('server').textContent = words[state.busy ? 'chip.starting' : state.ready ? 'chip.standing_by' : 'chip.waiting_for_files'];
  el('save').textContent = state.saveBytes ? 'game.db · ' + words['size.kb'].replace('__N__', number.format(state.saveBytes / 1024)) : words['main.no_save'];
  el('choose').textContent = words[state.ready ? 'main.change' : 'main.choose'];
  el('choose').disabled = el('more').disabled = state.busy;
  el('deploy').disabled = state.busy || !state.ready;
  el('meter').hidden = el('progress-label').hidden = state.progress === null;
  if (state.progress !== null) {
    el('meter').setAttribute('aria-valuenow', state.progress);
    el('meter').style.setProperty('--progress', state.progress + '%');
    el('progress-label').textContent = state.progress + '%';
  }
  detail(state.message || (state.ready ? state.source : 'Choose an Android resource archive or an imported folder.'));
  if (state.override !== null) {
    el('port-range').textContent = '8080–65535 · REINCOMP_BUNKER_PORT_OFFSET overrides this setting for this launch.';
  }
}

async function act(action) {
  try {
    await request(action);
    await update();
  } catch (error) {
    detail(error.message || String(error));
  }
}

for (const button of document.querySelectorAll('[data-source]')) {
  button.onclick = () => { el('files-dialog').close(); act(button.dataset.source); };
}
el('deploy').onclick = () => act('deploy');
function followTheme() { document.documentElement.dataset.theme = systemDark.matches ? 'dark' : 'light'; }
followTheme();
systemDark.addEventListener('change', followTheme);
update().catch(error => detail(error.message));
