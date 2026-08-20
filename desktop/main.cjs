const { app, BrowserWindow, dialog, session } = require("electron");
const { join } = require("node:path");
const { pathToFileURL } = require("node:url");

let apexServer = null;
let mainWindow = null;
let quitting = false;

async function ensureServer() {
  if (apexServer) return apexServer;
  const serverModule = await import(pathToFileURL(join(__dirname, "../src/server.js")).href);
  apexServer = await serverModule.startApexServer({ port: 0, host: "127.0.0.1", log: false });
  apexServer.on("error", (error) => {
    console.error(`Apex Editor server error: ${error.message}`);
  });
  return apexServer;
}

async function createWindow() {
  const server = await ensureServer();
  const allowedOrigin = new URL(server.apexUrl).origin;
  mainWindow = new BrowserWindow({
    width: 1600,
    height: 1000,
    minWidth: 960,
    minHeight: 640,
    show: false,
    backgroundColor: "#111315",
    title: "Apex Editor",
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
      allowRunningInsecureContent: false,
      webviewTag: false
    }
  });
  mainWindow.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
  mainWindow.webContents.on("will-navigate", (event, destination) => {
    if (new URL(destination).origin !== allowedOrigin) event.preventDefault();
  });
  mainWindow.once("ready-to-show", () => mainWindow?.show());
  mainWindow.on("closed", () => { mainWindow = null; });
  await mainWindow.loadURL(server.apexUrl);
}

app.setName("Apex Editor");
app.whenReady().then(() => {
  session.defaultSession.setPermissionRequestHandler((_webContents, _permission, callback) => callback(false));
  return createWindow();
}).catch((error) => {
  console.error(error);
  dialog.showErrorBox("Apex Editor could not start", error.stack || error.message);
  app.quit();
});

app.on("activate", () => {
  if (!mainWindow) createWindow().catch((error) => dialog.showErrorBox("Apex Editor could not open", error.stack || error.message));
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});

app.on("before-quit", (event) => {
  if (quitting || !apexServer) return;
  event.preventDefault();
  quitting = true;
  apexServer.close(() => {
    apexServer = null;
    app.quit();
  });
});
