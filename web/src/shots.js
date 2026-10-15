// The shotbox's buttons (SPECIFICATION.md §8.1), shared by the Shotbox tab and the clip-list widget (§8.7):
// clips and playlists in creation order, a filter (all, clips, playlists), and a playlist's clips' thumbnails.
import { computed } from "vue";
import { playNs, serial } from "./api.js";
import { cameraLabel, clipsById, state } from "./store.js";

export const FILTERS = [
  { value: "all", label: "All" },
  { value: "clip", label: "Clips" },
  { value: "playlist", label: "Playlists" },
];

/** The filter kept in the browser under `key` ("all" when none or unknown). */
export function storedFilter(key) {
  try {
    const v = localStorage.getItem(key);
    return FILTERS.some((f) => f.value === v) ? v : "all";
  } catch {
    return "all";
  }
}
export function storeFilter(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    /* only for this page */
  }
}

/** Up to three of a playlist's clips (first entries, each clip once) for its stacked thumbnails. */
export function stackOf(p) {
  const seen = new Set();
  const out = [];
  for (const id of p.clip_ids || []) {
    const c = clipsById.value[id];
    if (c && !seen.has(id)) {
      seen.add(id);
      out.push(c);
      if (out.length === 3) break;
    }
  }
  return out;
}

/** The buttons for `filter` (a ref or getter of "all", "clip" or "playlist"), oldest first. */
export function shotItems(filter) {
  return computed(() => {
    const clips = state.clips.map((c) => ({ kind: "clip", id: c.id, name: c.name, colour: c.colour, speed: c.speed, ns: playNs(c), sub: cameraLabel(c.camera), clip: c }));
    const lists = state.playlists.map((p) => ({
      kind: "playlist",
      id: p.id,
      name: p.name,
      colour: "#94a3b8",
      speed: null,
      ns: p.duration_ns,
      sub: `${p.entries} clips`,
      loop: p.loop,
      stack: stackOf(p),
    }));
    const all = [...clips, ...lists].sort((a, b) => serial(a.id) - serial(b.id));
    const f = typeof filter === "function" ? filter() : filter.value;
    return f === "all" ? all : all.filter((i) => i.kind === f);
  });
}

/** The shot's state on channel `c` ("idle" when the channel is on something else or live). */
export function shotStateOn(c, item) {
  return c && c.shot_id === item.id && !c.live ? c.shot : "idle";
}
/** How far channel `c` is through the shot (0–1). */
export function progressOn(c, item) {
  if (!c || c.shot_id !== item.id || c.out_ns <= c.in_ns) return 0;
  return Math.min(1, Math.max(0, (c.position_ns - c.in_ns) / (c.out_ns - c.in_ns)));
}
export const STATE_TEXT = { cued: "CUED", playing: "PLAYING", paused: "PAUSED", ended: "ENDED" };
