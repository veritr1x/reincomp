// Run the release LLVM toolchain locally. The input directory contains the
// compiler, native host objects and notices; it has no translated runtime.
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const crypto = require("node:crypto");
const {spawn} = require("node:child_process");
const {pipeline} = require("node:stream/promises");

async function build(site, apk, master, output, edge, jobs, expected) {
  const token = crypto.randomBytes(24).toString("hex");
  const prefix = "/" + token + "/";
  const profile = await fs.promises.mkdtemp(path.join(path.dirname(output), "edge-"));
  const partial = path.join(profile, "player.partial");
  let browser, resolve, reject;
  const completed = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  const inputs = new Map([["apk", apk], ["master", master]]);
  const types = {".js":"application/javascript", ".mjs":"application/javascript", ".wasm":"application/wasm", ".json":"application/json"};
  const worker = `
    import {compileWindows} from "./windows/compiler.mjs";
    const read = async name => {
      const response = await fetch(name);
      if (!response.ok) throw Error("Unable to read " + name);
      return response.blob();
    };
    try {
      const result = await compileWindows({apk:await read("apk"),master:await read("master"),
        workers:${jobs},log:s=>fetch("progress",{method:"POST",body:s})});
      const saved = await fetch("result",{method:"POST",body:result});
      if (!saved.ok) throw Error(await saved.text());
    } catch (e) {await fetch("failed",{method:"POST",body:String(e.stack || e)});}
  `;
  const server = http.createServer(async (req, res) => {
    try {
      if (!req.url.startsWith(prefix)) {res.writeHead(404).end(); return;}
      const name = req.url.slice(prefix.length);
      if (req.method === "GET" && name === "") {
        res.writeHead(200, {"Content-Type":"text/html"}).end('<script>new Worker("./build.mjs",{type:"module"}).onerror=e=>fetch("failed",{method:"POST",body:e.message})</script>');
      } else if (req.method === "GET" && name === "build.mjs") {
        res.writeHead(200, {"Content-Type":"application/javascript"}).end(worker);
      } else if (req.method === "GET") {
        if (!inputs.has(name) && (!/^(windows|bundles)\/[a-zA-Z0-9_./+-]+$/.test(name) || name.split("/").some(p=>p===".." || !p) || name.startsWith("windows/runtime/"))) {
          res.writeHead(404).end(); return;
        }
        const file = inputs.get(name) || path.join(site,name);
        const stat = await fs.promises.stat(file);
        res.writeHead(200, {"Content-Type":types[path.extname(file)] || "application/octet-stream","Content-Length":stat.size});
        await pipeline(fs.createReadStream(file),res);
      } else if (req.method === "POST" && ["progress","failed"].includes(name)) {
        let message = "";
        for await (const chunk of req) {message += chunk.toString(); if(message.length > 32768) throw Error("Invalid build log");}
        if (name === "progress") console.log(message); else reject(Error(message));
        res.writeHead(204).end();
      } else if (req.method === "POST" && name === "result") {
        await pipeline(req,fs.createWriteStream(partial,{flags:"wx"}));
        const hash = crypto.createHash("sha256");
        for await (const chunk of fs.createReadStream(partial)) hash.update(chunk);
        const result = hash.digest("hex");
        if(result !== expected) throw Error("Source-built EXE differs from the release. SHA-256 " + result + ". Candidate retained at " + partial);
        await fs.promises.link(partial,output);
        await fs.promises.unlink(partial);
        res.writeHead(200).end("OK");
        resolve(result);
      } else res.writeHead(404).end();
    } catch (e) {
      if (!res.headersSent) res.writeHead(500);
      res.end(String(e));
      reject(e);
    }
  });
  server.requestTimeout = 0;
  try {
    await new Promise((r,j)=>server.once("error",j).listen(0,"127.0.0.1",r));
    const url = "http://127.0.0.1:" + server.address().port + prefix;
    browser = spawn(edge,["--headless=new","--no-first-run","--disable-sync","--disable-extensions",
      "--disable-background-networking","--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
      "--user-data-dir="+profile,url],{windowsHide:true,stdio:"ignore"});
    browser.once("error",reject);
    browser.once("exit",code=>reject(Error("Browser closed before the build completed ("+code+")")));
    console.log("Verified EXE SHA-256",await completed);
  } finally {
    if(browser) browser.kill();
    server.closeAllConnections();
    server.close();
  }
}

const [site, apk, master, out, edge, count, digest] = process.argv.slice(2);
const jobs = Number(count);
if (!Number.isInteger(jobs) || jobs < 1 || !/^[a-f0-9]{64}$/.test(digest || "")) throw Error("Invalid build arguments");
build(site,apk,master,out,edge,jobs,digest).catch(e=>{console.error(e.message);process.exitCode=1;});
