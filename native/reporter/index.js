"use strict";

const http = require("http");
const https = require("https");

const PORT = 8080;
const EDGE_NODE_API_PORT = parseInt(process.env.EDGE_NODE_API_PORT || "8081", 10);
const EDGE_NODE_API_TOKEN = process.env.EDGE_NODE_API_TOKEN || "edge-node-api-token";
const POLL_INTERVAL_MS = parseInt(process.env.REPORTER_STATS_INTERVAL || "30", 10) * 1000;
const PUSH_INTERVAL_MS = parseInt(process.env.REPORTER_INTERVAL || "60", 10) * 1000;
const ENDPOINT = process.env.REPORTER_ENDPOINT || "";
const INSTANCE_ID = process.env.EDGE_INSTANCE_ID || process.env.JOB_INSTANCE_ID || process.env.HOSTNAME || "unknown";
const STALL_THRESHOLD = parseInt(process.env.REPORTER_STALL_THRESHOLD || "10", 10);
const LIVENESS_GRACE_SEC = parseInt(process.env.REPORTER_LIVENESS_GRACE_SEC || "300", 10);

let zeroSamples = 0;
let stats = {
  hashrate: 0,
  sharesAccepted: 0,
  sharesRejected: 0,
  pool: "",
  uptime: 0,
  connectionStatus: "unknown",
  lastUpdate: 0,
};

process.on("uncaughtException", (e) => console.error("[reporter] uncaught:", e && e.message));
process.on("unhandledRejection", (r) => console.error("[reporter] unhandled:", r));

function nodeSummary(cb){
  let done = false;
  const finish = (err, val) => {
	if(!done){
	  done = true;
	  cb(err, val);
	}
  };
  const req = http.request(
	{
	  hostname: "127.0.0.1",
		port: EDGE_NODE_API_PORT,
	  path: "/1/summary",
	  method: "GET",
		headers: { Authorization: "Bearer " + EDGE_NODE_API_TOKEN, Accept: "application/json" },
	  timeout: 2000,
	},
	(res) => {
	  let data = "";
	  res.on("data", (chunk) => (data += chunk));
	  res.on("end", () => {
		try{
		  finish(null, JSON.parse(data));
		}catch(e){
		  finish(e, null);
		}
	  });
	}
  );
  req.on("error", (e) => finish(e, null));
  req.on("timeout", () => {
	req.destroy();
	finish(new Error("timeout"), null);
  });
  req.end();
}

function poll(){
	nodeSummary((err, summary) => {
	const processUptime = process.uptime();
	if(err || !summary){
	  stats.hashrate = 0;
	  stats.connectionStatus = "api_unavailable";
	  stats.lastUpdate = Date.now();
	  if(processUptime > LIVENESS_GRACE_SEC && ++zeroSamples >= STALL_THRESHOLD){
			console.error("[reporter] runtime API unreachable x" + zeroSamples + "; exiting");
		process.exit(2);
	  }
	  return;
	}

	const hr = (summary.hashrate && summary.hashrate.total && summary.hashrate.total[0]) || 0;
	const good = (summary.results && summary.results.shares_good) || 0;
	const total = (summary.results && summary.results.shares_total) || 0;
	const uptime = (summary.connection && summary.connection.uptime) || 0;
	stats = {
	  hashrate: hr,
	  sharesAccepted: good,
	  sharesRejected: total - good,
	  pool: (summary.connection && summary.connection.pool) || "",
	  uptime,
	  connectionStatus: uptime > 0 ? "connected" : "disconnected",
	  lastUpdate: Date.now(),
	};
	if(processUptime > LIVENESS_GRACE_SEC && hr <= 0){
	  if(++zeroSamples >= STALL_THRESHOLD){
		console.error("[reporter] zero hashrate x" + zeroSamples + "; exiting");
		process.exit(2);
	  }
	}else if(hr > 0){
	  zeroSamples = 0;
	}
  });
}

function push(){
  if(!ENDPOINT) return;
  const body = JSON.stringify({
	instanceId: INSTANCE_ID,
	hashrate: stats.hashrate,
	sharesAccepted: stats.sharesAccepted,
	sharesRejected: stats.sharesRejected,
	pool: stats.pool,
	connectionStatus: stats.connectionStatus,
	algorithm: "rx/0",
	colo: "unknown",
	cpuPercent: 0,
	memoryUsage: 0,
	uptime: stats.uptime,
	timestamp: Date.now(),
  });
  const lib = ENDPOINT.startsWith("https:") ? https : http;
  const req = lib.request(
	ENDPOINT,
	{ method: "POST", headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) }, timeout: 5000 },
	(res) => res.resume()
  );
  req.on("error", (e) => console.error("[reporter] push error:", e.message));
  req.on("timeout", () => req.destroy());
  req.write(body);
  req.end();
}

http
  .createServer((req, res) => {
	if(req.url === "/health"){
	  res.writeHead(200, { "Content-Type": "application/json" });
	  res.end(JSON.stringify({ ok: true, instance: INSTANCE_ID }));
	  return;
	}
	if(req.url === "/stats"){
	  res.writeHead(200, { "Content-Type": "application/json" });
	  res.end(JSON.stringify(Object.assign({}, stats, { stallCounter: zeroSamples, stallThreshold: STALL_THRESHOLD })));
	  return;
	}
	res.writeHead(404);
	res.end("Not Found");
  })
  .listen(PORT, () => console.log("[reporter] listening on :" + PORT + " (instance " + INSTANCE_ID + ")"));

setTimeout(() => {
  poll();
  setInterval(poll, POLL_INTERVAL_MS);
}, 3000);
if(ENDPOINT){
  setTimeout(() => {
	push();
	setInterval(push, PUSH_INTERVAL_MS);
  }, 10000);
}
