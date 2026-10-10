<script setup>
// Operator-screen widget (SPECIFICATION.md §8.7): /widget/transport?channel=<n> and /widget/clip-list[?channel=<n>]
// with [&theme=dark|light|transparent], without the app around it, on this replay's own API (same origin).
// It posts {type: "widget-ready"} and {type: "widget-size", w, h} to the page that frames it.
import { onMounted, onUnmounted, ref, watch } from "vue";
import ClipListWidget from "./ClipListWidget.vue";
import TransportWidget from "./TransportWidget.vue";
import { start, state, stop } from "../store.js";

const params = new URLSearchParams(location.search);
const widget = location.pathname === "/widget/transport" ? TransportWidget : ClipListWidget;
const channel = Number(params.get("channel")) || 0;
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
    <component :is="widget" v-if="state.status" :channel="channel" />
    <div v-else class="empty">{{ state.statusError || (state.indexing ? "Indexing the buffer…" : "Waiting for the replay…") }}</div>
    <div v-if="state.error" class="widget-error" title="Dismiss" @click="state.error = ''">{{ state.error }}</div>
  </div>
</template>
