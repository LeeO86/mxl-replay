<script setup>
// Operator-screen widget (SPECIFICATION.md §8.7): /widget/<id>?<params>[&theme=dark|light|transparent], without
// the app around it, on this replay's own API (same origin). The server checked the id and the params against its
// one widget list (GET /widgets). It posts {type: "widget-ready"} and {type: "widget-size", w, h} to the page that
// frames it.
import { onMounted, onUnmounted, ref, watch } from "vue";
import ClipListWidget from "./ClipListWidget.vue";
import PlaylistEditorWidget from "./PlaylistEditorWidget.vue";
import PlaylistPlayerWidget from "./PlaylistPlayerWidget.vue";
import TimelineWidget from "./TimelineWidget.vue";
import TransportWidget from "./TransportWidget.vue";
import { start, state, stop } from "../store.js";

// The widget id (the same ids as GET /widgets) to its component.
const WIDGETS = {
  transport: TransportWidget,
  "clip-list": ClipListWidget,
  "playlist-editor": PlaylistEditorWidget,
  "playlist-player": PlaylistPlayerWidget,
  timeline: TimelineWidget,
};

const params = new URLSearchParams(location.search);
const id = decodeURIComponent(location.pathname.slice("/widget/".length));
const widget = WIDGETS[id] || null;
const channel = Number(params.get("channel")) || 0;
const flag = (name) => (params.has(name) ? ["true", "1"].includes(params.get(name)) : undefined);
// Only the params the widget declares (the others would land on its root element as attributes).
const all = { channel, playlist: params.get("playlist") || "", clip: flag("clip"), presets: flag("presets") };
const widgetProps = Object.fromEntries(Object.entries(all).filter(([k, v]) => v !== undefined && widget?.props && k in widget.props));
// A widget with a channel works on that channel (the timeline, scrubbing and marks use the selected one); not kept.
if (channel) state.channel = channel;
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its color scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  () => state.status,
  (status) => {
    if (!status || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(() => {
  start();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stop();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <div v-if="!widget" class="empty">No widget {{ id }}</div>
    <component :is="widget" v-else-if="state.status" v-bind="widgetProps" />
    <div v-else class="empty">{{ state.statusError || (state.indexing ? "Indexing the buffer…" : "Waiting for the replay…") }}</div>
    <div v-if="state.error" class="widget-error" title="Dismiss" @click="state.error = ''">{{ state.error }}</div>
  </div>
</template>
