// node cold-build.cjs site-directory-or-https-url original.apk master.bin.e new-run-directory [expected-sha256] [--linux]
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");
const {spawn} = require("node:child_process");
const {performance} = require("node:perf_hooks");
const {once} = require("node:events");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(read, timeout) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    const value = await read();
    if (value) return value;
    await sleep(200);
  }
  throw Error("Timed out waiting for " + read);
}
async function main() {
  const published = /^https:\/\//.test(process.argv[2]) ? new URL(process.argv[2]) : null;
  const site = published ? null : path.resolve(process.argv[2]);
  const [apk, master, run] = process.argv.slice(3, 6).map(p => path.resolve(p));
  const linux = process.argv.slice(6).includes("--linux");
  const expected = process.argv.slice(6).find(arg => arg !== "--linux");
  const megabits = Number(process.env.BUILD_TEST_MBPS || 0);
  assert.ok(Number.isFinite(megabits) && megabits >= 0);
  if (published) assert.equal(megabits,0,"Rate limiting requires a local site");
  fs.mkdirSync(run); // Refuse to reuse a previous browser profile or download.
  const profile = path.join(run, "edge"), downloads = path.join(run, "download");
  fs.mkdirSync(downloads);
  const requests = [];
  const siteVersion = published ? await (await fetch(new URL("version.json",published))).json() :
    JSON.parse(fs.readFileSync(path.join(site, "version.json")));
  let corrupt = false;
  let nextSend = 0;
  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://127.0.0.1");
    if (url.pathname === "/probe") {
      res.writeHead(200, {"Content-Type": "text/html", "Cache-Control": "no-store"});
      return res.end("<title>Storage probe</title>");
    }
    const name = decodeURIComponent(url.pathname.slice(1)) || "index.html";
    const member = path.resolve(site, name);
    if (req.method !== "GET" || !member.startsWith(site + path.sep) || !fs.statSync(member, {throwIfNoEntry:false})?.isFile()) {
      res.writeHead(404);
      return res.end();
    }
    const size = fs.statSync(member).size;
    requests.push({name, size});
    const types = {".html":"text/html", ".mjs":"text/javascript", ".js":"text/javascript", ".json":"application/json", ".wasm":"application/wasm"};
    res.writeHead(200, {"Content-Type":types[path.extname(member)] || "application/octet-stream", "Content-Length":size, "Cache-Control":"no-store"});
    if (corrupt && name === "windows/runtime/runtime.part001") {
      const bytes = fs.readFileSync(member);
      bytes[0] ^= 1;
      return res.end(bytes);
    }
    const stream = fs.createReadStream(member, {highWaterMark:128*1024});
    if (!megabits) return stream.on("error", e => res.destroy(e)).pipe(res);
    try {
      for await (const chunk of stream) {
        nextSend = Math.max(performance.now(), nextSend) + chunk.length * 8 / (megabits * 1000);
        await sleep(Math.max(0, nextSend - performance.now()));
        if(res.destroyed) break;
        if(!res.write(chunk)) await once(res,"drain");
      }
      res.end();
    } catch (error) { res.destroy(error); }
  });
  if (!published) await new Promise((resolve, reject) => server.once("error", reject).listen(0, "127.0.0.1", resolve));
  const origin = published ? published.origin : "http://127.0.0.1:" + server.address().port;
  const probe = published ? new URL("version.json",published).href : origin+"/probe";
  const browser = spawn(process.env.EDGE_PATH || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe", [
    "--headless=new", "--no-first-run", "--disable-background-networking", "--disable-sync", "--disable-extensions",
    "--remote-debugging-port=0", "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1"+(published?", EXCLUDE "+published.hostname:""),
    "--user-data-dir=" + profile, "about:blank"
  ], {windowsHide:true, stdio:"ignore"});
  let ws, call;
  try {
    const target = await until(async () => {
      try {
        const port = fs.readFileSync(path.join(profile,"DevToolsActivePort"),"utf8").split(/\s/)[0];
        const response = await fetch("http://127.0.0.1:"+port+"/json/new?"+encodeURIComponent(probe), {method:"PUT"});
        return response.ok && response.json();
      } catch { return false; }
    }, 45000);
    ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {ws.onopen=resolve; ws.onerror=reject});
    const pending = new Map();
    let sequence = 0;
    ws.onmessage = event => {
      const v = JSON.parse(event.data), handlers = pending.get(v.id);
      if (!handlers) return;
      pending.delete(v.id);
      if (v.error) handlers.reject(Error(v.error.message));
      else handlers.resolve(v.result);
    };
    call = (method, params={}) => new Promise((resolve,reject) => {
      const id = ++sequence;
      pending.set(id, {resolve, reject});
      ws.send(JSON.stringify({id, method, params}));
    });
    const evaluate = async expression => {
      const r = await call("Runtime.evaluate", {expression, returnByValue:true, awaitPromise:true, userGesture:true});
      if (r.exceptionDetails) throw Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    };
    await call("Network.enable");
    await call("Network.setCacheDisabled", {cacheDisabled:true});
    await until(() => evaluate("location.href==="+JSON.stringify(probe)+" && document.readyState==='complete'"), 45000);
    const storage = await evaluate(`(async () => {
      const files = [];
      for await (const key of (await navigator.storage.getDirectory()).keys()) files.push(key);
      return {databases:await indexedDB.databases(),caches:await caches.keys(),files,
        serviceWorkers:(await navigator.serviceWorker.getRegistrations()).length,localStorage:localStorage.length};
    })()`);
    assert.deepEqual(storage,{databases:[],caches:[],files:[],serviceWorkers:0,localStorage:0});
    console.log("Verified empty storage", JSON.stringify(storage));
    await call("Browser.setDownloadBehavior",{behavior:"allow",downloadPath:downloads});
    const opened = performance.now();
    await call("Page.navigate",{url:published?.href || origin+"/"});
    await until(() => evaluate('document.querySelector("#bunker-version")?.textContent.includes('+JSON.stringify(siteVersion.version)+')'),30000);
    const platform = linux ? "linux" : "windows";
    await evaluate(`document.querySelector("input[value=${platform}]").click()`);
    if (linux) assert.equal(await evaluate('document.querySelector("#linux-option").hidden'), false);
    const doc = await call("DOM.getDocument");
    async function select(selector, file) {
      const element = await call("DOM.querySelector",{nodeId:doc.root.nodeId, selector});
      await call("DOM.setFileInputFiles",{nodeId:element.nodeId,files:[file]});
    }
    await select("#game",apk);
    await select("#master",master);
    assert.equal(await evaluate('document.querySelector("#build").disabled'),false);
    const started = performance.now();
    await evaluate('document.querySelector("#build").click()');
    const stages = [], log = 'document.querySelector("#log").textContent.trim()';
    let previous = "";
    await until(async () => {
      const current = await evaluate(log);
      if (current !== previous) {
        const line = current.split("\n").at(-1);
        const seconds = +((performance.now()-started)/1000).toFixed(2);
        stages.push({seconds,line});
        console.log(seconds, line);
        previous = current;
      }
      if (current.includes("Build failed:")) throw Error(current);
      return current.endsWith("Done.");
    }, 24*3600000);
    const ready = performance.now();
    await evaluate('document.querySelector("#download").click()');
    const file = path.join(downloads,linux ? "NieR-Reincarnation-Linux-x86_64.tar" : "Nier Reincarnation.exe");
    // Edge renames the temporary .crdownload when the file is ready. Reading
    // the blob from the page here would add another 1 GB read to the timing.
    await until(() => fs.statSync(file,{throwIfNoEntry:false})?.size > 0 &&
      !fs.readdirSync(downloads).some(name=>name.endsWith(".crdownload")), 180000);
    const written = performance.now();
    const bytes = fs.statSync(file).size;
    const digest = crypto.createHash("sha256");
    for await (const chunk of fs.createReadStream(file)) digest.update(chunk);
    const sha256 = digest.digest("hex");
    const report = {storage, httpCacheDisabled:true, bandwidthLimitMbps:megabits || null, origin, version:siteVersion, bytes, sha256,
      buildSeconds:(ready-started)/1000, clickToDiskSeconds:(written-started)/1000, pageToDiskSeconds:(written-opened)/1000,
      siteBytes:published ? null : requests.reduce((sum,r)=>sum+r.size,0),
      runtimeBytes:published ? null : requests.filter(r=>r.name.includes("/runtime/runtime.part")).reduce((sum,r)=>sum+r.size,0),
      compilerWasmRequests:published ? null : requests.filter(r=>/\/(?:lift|coff|poll|gnu-link)\.wasm$/.test(r.name)), file, stages};
    fs.writeFileSync(path.join(run,"result.json"), JSON.stringify(report,null,2));
    console.log(JSON.stringify(report));
    if (expected) assert.equal(sha256,expected,"Download differs from reference");
    if (!published && linux) {
      assert.equal(report.runtimeBytes,0);
      assert.deepEqual(report.compilerWasmRequests,[]);
      assert.equal(await evaluate('document.querySelector("#install-linux").hidden'),false);
    }
    if (!published && siteVersion.windowsRuntime && !linux) {
      assert.ok(report.runtimeBytes > 0);
      assert.deepEqual(report.compilerWasmRequests,[]);
    }
    // Negative checks follow the timed run so they cannot prime it.
    let count = requests.length;
    await evaluate(`(async()=>{ const input=document.querySelector("#master"),f=input.files[0];
      const last=new Uint8Array(await f.slice(-1).arrayBuffer());last[0]^=1;
      const d=new DataTransfer();d.items.add(new File([f.slice(0,-1),last], f.name));
      input.files=d.files;input.dispatchEvent(new Event("change"));document.querySelector("#build").click(); })()`);
    await until(()=>evaluate(log+'.includes("master data differs")'),20000);
    if (!published) assert.equal(requests.slice(count).some(r=>r.name.includes("/runtime/")),false);
    if (linux) {
      await select("#master", master);
      await evaluate(`(async()=>{ const input=document.querySelector("#game"),f=input.files[0];
        const head=new Uint8Array(await f.slice(0,1).arrayBuffer());head[0]^=1;
        const d=new DataTransfer();d.items.add(new File([head,f.slice(1)],f.name));
        input.files=d.files;input.dispatchEvent(new Event("change"));document.querySelector("#build").click(); })()`);
      await until(()=>evaluate(log+'.includes("APK differs")'),30000);
    }
    if (!published && siteVersion.windowsRuntime && !linux) {
      corrupt=true;
      await select("#master",master);
      await evaluate('document.querySelector("#build").click()');
      await until(()=>evaluate(log+'.includes("Damaged browser kit file runtime/runtime.part001")'),90000);
    }
    console.log("Negative checks passed");
  } finally {
    if (call) call("Browser.close").catch(()=>{});
    if (ws) ws.close();
    browser.kill();
    if (server.listening) server.close();
  }
}
main().catch(e=>{console.error(e);process.exitCode=1});
