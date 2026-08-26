
const lines = [];
process.stdin.setEncoding('utf8');
let buf='';
process.stdin.on('data', c => { buf += c; let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i).trim(); buf = buf.slice(i+1);
    if (line) handle(JSON.parse(line));
  }
});
function handle(msg){
  if (msg.method === 'ping_with_delay') {
    setTimeout(() => reply(msg.id, { pong: true }), 400); return;
  }
  if (msg.method === 'boom') return send({jsonrpc:'2.0',id:msg.id,error:{code:-32601,message:'method not found'}});
  if (msg.method && msg.id === undefined) return void 0; // notification
  if (msg.method === 'greet') {
    send({jsonrpc:'2.0',method:'hello',params:{from:'server'}});
    return reply(msg.id, { hi: msg.params.name });
  }
  reply(msg.id, { echoed: msg });
}
function send(o){ process.stdout.write(JSON.stringify(o)+'\n'); }
function reply(id,r){ send({jsonrpc:'2.0',id,result:r}); }
