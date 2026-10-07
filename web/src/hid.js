// Jog/shuttle controllers over WebHID (SPECIFICATION.md §8.4): Contour ShuttleXpress
// (0b33:0020), ShuttlePRO (0b33:0010) and ShuttlePRO v2 (0b33:0030).
//
// Input report (no report id, 5 bytes):
//   byte 0    shuttle ring, signed: −7 (full left) … 0 (centre, spring return) … +7 (full right)
//   byte 1    jog wheel: a counter 0–255 that wraps, +1 per detent clockwise
//   byte 2    unused
//   byte 3–4  buttons, a little-endian bit mask (bit 0 = button 1). The ShuttleXpress reports
//             its five buttons as bits 4–8; the ShuttlePRO v2 its fifteen as bits 0–14.
//
// The ring sets the speed (and plays; centre pauses), the jog wheel steps frames, buttons run
// actions. WebHID needs a secure context (https or localhost) and Chrome or Edge.
import { reactive } from "vue";

export const SHUTTLE_SPEEDS = [0, 0.1, 0.25, 0.33, 0.5, 0.75, 1, 2]; // by ring position 0–7

const XPRESS = { 4: "markIn", 5: "markOut", 6: "playPause", 7: "gotoIn", 8: "live" };
const PRO = {
  0: "markIn",
  1: "markOut",
  2: "gotoIn",
  3: "gotoOut",
  4: "camera1",
  5: "camera2",
  6: "camera3",
  7: "camera4",
  8: "live",
  9: "playPause",
  10: "createClip",
  11: "speed50",
  12: "speed100",
  13: "secondBack",
  14: "secondForward",
};
export const BUTTON_LAYOUTS = { "ShuttleXpress (5 buttons)": XPRESS, "ShuttlePRO / ShuttlePRO v2": PRO };

export const hid = reactive({
  supported: typeof navigator !== "undefined" && "hid" in navigator,
  secure: typeof window !== "undefined" && window.isSecureContext,
  device: "",
  shuttle: 0,
  jog: null,
  buttons: 0,
  error: "",
});

let handler = () => {};
let device = null;
let lastJog = null;
let lastButtons = 0;
let lastShuttle = 0;

/** fn(action, value): "jog" (frames), "shuttle" (ring position), or a button action id. */
export function onHidAction(fn) {
  handler = fn;
}

function onReport(event) {
  const data = event.data;
  if (data.byteLength < 5) return;
  const shuttle = data.getInt8(0);
  const jog = data.getUint8(1);
  const buttons = data.getUint16(3, true);
  if (lastJog !== null && jog !== lastJog) handler("jog", ((jog - lastJog + 384) % 256) - 128);
  lastJog = jog;
  if (shuttle !== lastShuttle) handler("shuttle", shuttle);
  lastShuttle = shuttle;
  const pressed = buttons & ~lastButtons;
  const layout = event.device.productId === 0x0020 ? XPRESS : PRO;
  for (const [bit, action] of Object.entries(layout)) if (pressed & (1 << Number(bit))) handler(action);
  lastButtons = buttons;
  Object.assign(hid, { shuttle, jog, buttons });
}

async function open(dev) {
  if (!dev.opened) await dev.open();
  dev.addEventListener("inputreport", onReport);
  device = dev;
  lastJog = null;
  lastButtons = 0;
  lastShuttle = 0;
  hid.device = dev.productName || "HID device";
  hid.error = "";
}

export async function connectHid() {
  try {
    const [dev] = await navigator.hid.requestDevice({ filters: [{ vendorId: 0x0b33 }] });
    if (dev) await open(dev);
  } catch (e) {
    hid.error = e.message;
  }
}

export async function disconnectHid() {
  if (!device) return;
  device.removeEventListener("inputreport", onReport);
  try {
    await device.close();
  } catch {
    /* already gone */
  }
  device = null;
  hid.device = "";
}

/** Reopens a device this site was allowed to use before (no prompt). */
export async function restoreHid() {
  if (!hid.supported) return;
  navigator.hid.addEventListener("disconnect", (event) => {
    if (event.device === device) {
      device = null;
      hid.device = "";
    }
  });
  try {
    const dev = (await navigator.hid.getDevices()).find((d) => d.vendorId === 0x0b33);
    if (dev) await open(dev);
  } catch (e) {
    hid.error = e.message;
  }
}
