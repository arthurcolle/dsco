// Optional real Chrome UI test. Node >=22; no npm dependencies or providers.
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import assert from 'node:assert/strict';
const binary = resolve(process.argv[2] || './dsco');
const chromePath = process.env.CHROME_BIN || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const temporary = await mkdtemp(join(tmpdir(), 'dsco-prompt-browser-'));
let service, chrome, socket;
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn) { for (let n=0;n<100;n++) { try { const v=await fn(); if(v) return v; } catch {} await sleep(100); } throw Error('Browser test timed out'); }
try {
  service = spawn('python3', [resolve('web/prompt_branches/run_local.py'), '--binary', binary, '--data-dir', join(temporary,'store'), '--port','0'], {stdio:['ignore','pipe','pipe']});
  let logs = ''; service.stdout.on('data', x => {logs += x;});
  const port = await until(() => /Ready: http:\/\/127\.0\.0\.1:(\d+)/.exec(logs)?.[1]);
  const credentials = JSON.parse(await readFile(join(temporary,'store/access.json'),'utf8'));
  chrome = spawn(chromePath, ['--headless=new','--disable-background-networking','--disable-component-update','--disable-sync','--no-first-run','--disable-default-apps','--remote-debugging-port=0','--user-data-dir='+join(temporary,'chrome'),'about:blank'], {stdio:'ignore'});
  const active = await until(async () => (await readFile(join(temporary,'chrome/DevToolsActivePort'),'utf8')).trim().split('\n'));
  socket = new WebSocket('ws://127.0.0.1:'+active[0]+active[1]);
  await new Promise((resolve,reject) => {socket.addEventListener('open',resolve,{once:true}); socket.addEventListener('error',reject,{once:true});});
  let sequence=0; const pending=new Map();
  socket.addEventListener('message',event => {const message=JSON.parse(event.data); const waiter=pending.get(message.id); if(waiter){pending.delete(message.id); message.error ? waiter.reject(Error(JSON.stringify(message.error))) : waiter.resolve(message.result);}});
  async function rpc(method,params={},sessionId) {const id=++sequence; return new Promise((resolve,reject) => {const timer=setTimeout(()=>{pending.delete(id);reject(Error('CDP timeout '+method));},10000);pending.set(id,{resolve:x=>{clearTimeout(timer);resolve(x);},reject:e=>{clearTimeout(timer);reject(e);}});socket.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));});}
  const target=await rpc('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await rpc('Target.attachToTarget',{targetId:target.targetId,flatten:true});
  const evaluate=async expression => {const r=await rpc('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sessionId);if(r.exceptionDetails)throw Error('Page exception');return r.result.value;};
  await rpc('Page.navigate',{url:'http://127.0.0.1:'+port},sessionId);
  await until(()=>evaluate('!!document.getElementById("token")'));
  await evaluate('window.confirm=()=>true; document.getElementById("token").value='+JSON.stringify(credentials.owner_token)+'; document.getElementById("document").value="browser-fixture"; document.getElementById("content").value="Browser main π"');
  async function click(id) {await evaluate('document.getElementById('+JSON.stringify(id)+').click()');await until(()=>evaluate('!busy'));}
  await click('init');
  assert.equal(await evaluate('document.getElementById("content").value'),'Browser main π');
  await evaluate('document.getElementById("newBranch").value="agent.browser"');
  await click('fork');
  assert.equal(await evaluate('document.getElementById("branch").value'),'agent.browser');
  await evaluate('document.getElementById("content").value="Candidate π <script>window.injected=1</script>";document.getElementById("content").dispatchEvent(new Event("input"))');
  await click('commit');
  assert.match(await evaluate('document.getElementById("content").value'),/^Candidate π/);
  assert.equal(await evaluate('typeof window.injected'),'undefined');
  assert.equal(await evaluate('localStorage.length'),0);
  // Create a competing revision, then prove the stale editor retains its draft.
  await evaluate('api({action:"commit",document:"browser-fixture",branch:loaded.branch,expected:loaded.revision,content:"Competing revision"})');
  await evaluate('document.getElementById("content").value="Unsaved stale draft";document.getElementById("content").dispatchEvent(new Event("input"))');
  await click('commit');
  assert.match(await evaluate('document.getElementById("result").textContent'),/conflict/);
  assert.equal(await evaluate('document.getElementById("content").value'),'Unsaved stale draft');
  await click('load');
  assert.equal(await evaluate('document.getElementById("content").value'),'Competing revision');
  await click('promote');
  assert.equal(await evaluate('document.getElementById("branch").value'),'main');
  assert.equal(await evaluate('document.getElementById("content").value'),'Competing revision');
  await click('history');
  assert.match(await evaluate('document.getElementById("result").textContent'),/merge_parent/);
  console.log('PASS: real Chrome UI init → fork → edit → commit → stale-draft preservation → promotion → history; no HTML execution or localStorage');
} finally {
  if(socket) socket.close();
  for(const child of [chrome,service]) if(child && child.exitCode===null && child.signalCode===null) {
    const exited = new Promise(r=>child.once('exit',r));
    child.kill('SIGTERM');
    await Promise.race([exited,sleep(3000)]);
    if(child.exitCode===null && child.signalCode===null) {child.kill('SIGKILL');await exited;}
  }
  await rm(temporary,{recursive:true,force:true});
}
