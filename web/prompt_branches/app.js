'use strict';
const $ = id => document.getElementById(id);
let loaded = null, mainHead = null, dirty = false, busy = false;
const report = value => { $('result').textContent = typeof value === 'string' ? value : JSON.stringify(value, null, 2); };
async function api(request) {
  const response = await fetch('/api', {method: 'POST', headers: {'Content-Type':'application/json', Authorization:'Bearer ' + $('token').value}, body:JSON.stringify(request), cache:'no-store', credentials:'omit'});
  const value = await response.json();
  if (!response.ok || !value.ok) throw Error(JSON.stringify(value));
  return value;
}
function doc() { return $('document').value; }
function current() {
  if (!loaded || loaded.document !== doc() || loaded.branch !== $('branch').value) throw Error('Load the selected branch before editing or forking.');
  return loaded;
}
async function branches(preferred) {
  const result = await api({action:'list', document:doc()});
  mainHead = result.branches.find(b => b.name === 'main')?.head || null;
  const selected = preferred || $('branch').value;
  $('branch').replaceChildren(...result.branches.map(b => { const option = document.createElement('option'); option.value = b.name; option.textContent = b.name; return option; }));
  if (result.branches.some(b => b.name === selected)) $('branch').value = selected;
  report(result);
}
async function load() {
  if (dirty && !confirm('Discard the unsaved editor draft?')) return;
  const document = doc(), branch = $('branch').value;
  const result = await api({action:'get', document, branch});
  loaded = {document, branch, revision:result.revision};
  $('content').value = result.content; dirty = false;
  $('revision').textContent = JSON.stringify({revision:result.revision, ...result.metadata}, null, 2);
  report(result.metadata);
}
function bind(id, action) { $(id).addEventListener('click', async () => {
  if (busy) return; busy = true;
  const buttons = [...document.querySelectorAll('button')]; buttons.forEach(b => b.disabled = true);
  try { await action(); } catch (error) { report(error.message); }
  finally { busy = false; buttons.forEach(b => b.disabled = false); }
}); }
$('content').addEventListener('input', () => { dirty = true; });
window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
bind('refresh', () => branches());
bind('load', load);
bind('init', async () => {
  if (!confirm('Create this document’s immutable root and main branch?')) return;
  const result = await api({action:'init', document:doc(), content:$('content').value, message:$('message').value});
  dirty = false; await branches('main'); await load(); report(result);
});
bind('commit', async () => {
  const state = current();
  const result = await api({action:'commit', document:state.document, branch:state.branch, expected:state.revision, content:$('content').value, message:$('message').value});
  dirty = false; await branches(state.branch); await load(); report(result);
});
bind('fork', async () => {
  const state = current();
  if (dirty) throw Error('Commit or save the draft first. Fork copies a stored revision, not unsaved editor text.');
  const branch = $('newBranch').value;
  const request = {action:'fork', document:state.document, branch, from:state.branch, expected:state.revision};
  if ($('historical').value) request.revision = $('historical').value;
  const result = await api(request); await branches(branch); await load(); report(result);
});
bind('history', async () => { const state = current(); report(await api({action:'history', document:state.document, branch:state.branch})); });
bind('promote', async () => {
  const state = current();
  if (dirty) throw Error('Commit the draft before promoting.');
  if (!mainHead || state.branch === 'main') throw Error('Load branches and select a non-main branch.');
  if (!confirm('Replace main content with revision ' + state.revision + '? Existing history is retained. This does not change runtime governance.')) return;
  const result = await api({action:'promote', document:state.document, from:state.branch, expected:mainHead, source_expected:state.revision, message:$('message').value});
  await branches('main'); await load(); report(result);
});
