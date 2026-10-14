// REST client for the replay API (SPECIFICATION.md §8.5) and display helpers.

export class ApiError extends Error {
  constructor(status, body) {
    super(body?.error || body?.status || `HTTP ${status}`);
    this.status = status;
    this.body = body;
  }
}

async function request(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.body != null && typeof options.body === "string" && !headers["Content-Type"]) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, { ...options, headers });
  const type = resp.headers.get("content-type") || "";
  const body = type.includes("json") ? await resp.json() : await resp.text();
  if (!resp.ok) throw new ApiError(resp.status, typeof body === "object" ? body : { error: String(body).slice(0, 300) });
  return body;
}

export const api = {
  get: (path) => request(path),
  post: (path, body) => request(path, { method: "POST", body: typeof body === "string" ? body : JSON.stringify(body ?? {}) }),
  put: (path, body) => request(path, { method: "PUT", body: JSON.stringify(body ?? {}) }),
  patch: (path, body) => request(path, { method: "PATCH", body: JSON.stringify(body ?? {}) }),
  del: (path) => request(path, { method: "DELETE" }),
};

export function fmtBytes(n) {
  if (!n) return "0 B";
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    ++i;
  }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

/** A length in seconds: "4.20 s", "1:05.4", "1:02:03". */
export function fmtSeconds(s) {
  if (s == null || !Number.isFinite(s)) return "–";
  const sign = s < 0 ? "−" : "";
  s = Math.abs(s);
  if (s < 60) return `${sign}${s.toFixed(s < 10 ? 2 : 1)} s`;
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h) return `${sign}${h}:${String(m).padStart(2, "0")}:${String(Math.floor(sec)).padStart(2, "0")}`;
  return `${sign}${m}:${sec.toFixed(1).padStart(4, "0")}`;
}

export function fmtNs(ns) {
  return fmtSeconds(ns / 1e9);
}

export function fmtSpeed(speed) {
  return `${Math.round((speed ?? 0) * 100)}%`;
}

/** Playing time of a clip at its speed (or the speed given). */
export function playNs(clip, speed = clip.speed) {
  const s = Math.abs(speed || 0);
  return (clip.out_ns - clip.in_ns) / (s > 0.001 ? s : 1);
}

/** Serial number of an id ("clip-12" → 12): creation order. */
export function serial(id) {
  const m = /-(\d+)$/.exec(id || "");
  return m ? Number(m[1]) : 0;
}

export const END_ACTIONS = [
  { value: "freeze", label: "Freeze" },
  { value: "black", label: "Black" },
  { value: "loop", label: "Loop" },
  { value: "next", label: "Next" },
  { value: "return-to-live", label: "Return to live" },
];
// Playlist transitions (1.7.0): the entry's `transition` and `transition_frames` as one choice for compact editors.
export const TRANSITION_PRESETS = [
  { value: "cut", label: "Cut", transition: "cut", frames: 0 },
  { value: "mix12", label: "Mix 12", transition: "mix", frames: 12 },
  { value: "mix25", label: "Mix 25", transition: "mix", frames: 25 },
  { value: "mix50", label: "Mix 50", transition: "mix", frames: 50 },
];
/** The preset of an entry; a mix of another length is offered as its own choice. */
export function transitionPreset(entry) {
  if (entry.transition !== "mix") return "cut";
  const preset = TRANSITION_PRESETS.find((p) => p.transition === "mix" && p.frames === entry.transition_frames);
  return preset ? preset.value : `mix${entry.transition_frames}`;
}
/** The default aux track of a playlist (the server's defaults). */
export const AUX_DEFAULTS = { source: "", level_db: 0, loop: true, fade_out_frames: 50, duck_db: -12, attack_ms: 200, release_ms: 500 };
/** The transition fields an editor keeps per entry, from a playlist answer. */
export const entryTransition = (e) => ({
  transition: e.transition || "cut",
  transition_frames: e.transition_frames || 0,
  audio_frames: e.audio_frames ?? -1,
  own_audio: e.own_audio ?? true,
});

export const MOTION_MODES = [
  { value: "repeat", label: "Repeat" },
  { value: "blend", label: "Blend" },
  { value: "interpolate", label: "Interpolate" },
];
export const AUDIO_MODES = [
  { value: "mute", label: "Mute" },
  { value: "stretch", label: "Stretch" },
  { value: "follow", label: "Follow" },
];
export const SPEED_PRESETS = [0.25, 0.33, 0.5, 0.66, 0.75, 1];
export const CLIP_COLOURS = ["#3b82f6", "#ef4444", "#a855f7", "#f97316", "#06b6d4", "#ec4899", "#94a3b8", "#14b8a6"];
