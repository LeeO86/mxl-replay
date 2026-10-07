// Keyboard map (SPECIFICATION.md §8.4): action → key combination, kept in this browser
// (localStorage). Digits are fixed: on the Shotbox they select a button, on the LSM a camera.
import { reactive } from "vue";

export const ACTIONS = [
  { id: "playPause", label: "Play / pause", key: "Space" },
  { id: "live", label: "Back to live", key: "l" },
  { id: "markIn", label: "Mark IN", key: "i" },
  { id: "markOut", label: "Mark OUT", key: "o" },
  { id: "gotoIn", label: "Go to IN", key: "Shift+i" },
  { id: "gotoOut", label: "Go to OUT", key: "Shift+o" },
  { id: "frameBack", label: "Back 1 frame", key: "ArrowLeft" },
  { id: "frameForward", label: "Forward 1 frame", key: "ArrowRight" },
  { id: "secondBack", label: "Back 1 second", key: "Shift+ArrowLeft" },
  { id: "secondForward", label: "Forward 1 second", key: "Shift+ArrowRight" },
  { id: "speedUp", label: "Speed +5 %", key: "ArrowUp" },
  { id: "speedDown", label: "Speed −5 %", key: "ArrowDown" },
  { id: "createClip", label: "Create clip (LSM)", key: "c" },
  { id: "cue", label: "Cue / play the selected button (Shotbox)", key: "Enter" },
  { id: "nextBank", label: "Next bank (Shotbox)", key: "PageDown" },
  { id: "prevBank", label: "Previous bank (Shotbox)", key: "PageUp" },
];

const STORAGE = "mxl-replay.keys";

function defaults() {
  return Object.fromEntries(ACTIONS.map((a) => [a.id, a.key]));
}

function load() {
  try {
    return { ...defaults(), ...JSON.parse(localStorage.getItem(STORAGE) || "{}") };
  } catch {
    return defaults();
  }
}

export const keymap = reactive(load());

function save() {
  try {
    localStorage.setItem(STORAGE, JSON.stringify(keymap));
  } catch {
    /* kept for this tab only */
  }
}

/** "Shift+ArrowLeft", "Space", "i" … from a keydown event; empty for a bare modifier. */
export function comboOf(event) {
  if (["Shift", "Control", "Alt", "Meta"].includes(event.key)) return "";
  let key = event.key === " " ? "Space" : event.key;
  if (key.length === 1) key = key.toLowerCase();
  const mods = [event.ctrlKey && "Ctrl", event.altKey && "Alt", event.metaKey && "Meta", event.shiftKey && "Shift"].filter(Boolean);
  return [...mods, key].join("+");
}

const NAMES = { ArrowLeft: "←", ArrowRight: "→", ArrowUp: "↑", ArrowDown: "↓", PageUp: "PgUp", PageDown: "PgDn", Escape: "Esc" };
/** "Shift+ArrowLeft" → "Shift+←", "i" → "I": for display. */
export function keyLabel(combo) {
  if (!combo) return "–";
  return combo
    .split("+")
    .map((part) => NAMES[part] || (part.length === 1 ? part.toUpperCase() : part))
    .join("+");
}

export function actionFor(combo) {
  return ACTIONS.find((a) => keymap[a.id] === combo)?.id || "";
}

/** Assigns a key; an action that had it loses it. */
export function setKey(id, combo) {
  for (const a of ACTIONS) if (keymap[a.id] === combo) keymap[a.id] = "";
  keymap[id] = combo;
  save();
}

export function resetKeys() {
  Object.assign(keymap, defaults());
  save();
}
