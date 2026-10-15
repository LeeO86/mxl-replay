<script setup>
// The clip list widget (SPECIFICATION.md §8.7): the shotbox's buttons for one channel, clips and playlists in
// shotbox order with the same filter (all, clips, playlists; kept in the browser). A click is a shotbox click:
// it cues the clip or the playlist's first clip, the next plays, the next pauses. Without `channel` the widget
// has a channel picker.
import { computed, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import Segmented from "./Segmented.vue";
import { api, fmtNs, fmtSpeed } from "../api.js";
import { act, channels, channelState, refreshStatus, state } from "../store.js";
import { FILTERS, progressOn, shotItems, shotStateOn, STATE_TEXT, storedFilter, storeFilter } from "../shots.js";

const props = defineProps({ channel: { type: Number, default: 0 } });
const target = computed(() => channels.value.find((c) => c.index === (props.channel || state.channel)) || null);

const FILTER_KEY = "mxl-replay.widget-clips-filter";
const filter = ref(storedFilter(FILTER_KEY));
watch(filter, (v) => storeFilter(FILTER_KEY, v));
const items = shotItems(filter);

const shotState = (item) => shotStateOn(target.value, item);
const progress = (item) => progressOn(target.value, item);

async function click(item) {
  if (!target.value) return;
  await act(() => api.post(`/api/v1/shotbox/${encodeURIComponent(item.id)}`, { channel: target.value.index }));
  refreshStatus();
}
</script>

<template>
  <div class="widget-clips">
    <div class="widget-head">
      <ChannelPicker v-if="!channel" />
      <template v-else-if="target">
        <span class="name">{{ target.label }}</span>
        <span class="state-tag" :class="channelState(target)">{{ channelState(target).toUpperCase() }}</span>
      </template>
      <span class="spacer"></span>
      <Segmented v-model="filter" :options="FILTERS" label="Show" />
    </div>
    <div v-if="!items.length" class="empty">{{ filter === "playlist" ? "No playlists yet." : "No clips yet." }}</div>
    <div v-else class="widget-shots">
      <button
        v-for="item in items"
        :key="item.id"
        class="shot"
        :class="shotState(item)"
        :disabled="!target"
        :aria-pressed="shotState(item) === 'playing'"
        :title="`${item.name} (${item.id})`"
        @click="click(item)"
      >
        <div class="thumb">
          <img v-if="item.kind === 'clip'" :src="`/api/v1/clips/${encodeURIComponent(item.id)}/thumbnail.jpg?in=${item.clip.in_ns}`" alt="" loading="lazy" @error="$event.target.style.visibility = 'hidden'" />
          <div v-else-if="item.stack.length" class="stack" :class="`n${item.stack.length}`">
            <img
              v-for="(c, k) in [...item.stack].reverse()"
              :key="c.id"
              :class="`s${item.stack.length - 1 - k}`"
              :src="`/api/v1/clips/${encodeURIComponent(c.id)}/thumbnail.jpg?in=${c.in_ns}`"
              :style="{ borderColor: c.colour }"
              alt=""
              loading="lazy"
              @error="$event.target.style.visibility = 'hidden'"
            />
            <span class="stack-tag">PLAYLIST{{ item.loop ? " ↻" : "" }}</span>
          </div>
          <div v-else class="playlist-mark">PLAYLIST</div>
          <span v-if="STATE_TEXT[shotState(item)]" class="badge-state state-tag" :class="shotState(item)">{{ STATE_TEXT[shotState(item)] }}</span>
        </div>
        <div class="label">
          <div class="name">{{ item.name }}</div>
          <div class="meta">
            <span class="num">{{ fmtNs(item.ns) }}</span>
            <span>{{ item.sub }}</span>
            <span v-if="item.speed != null" class="speed">{{ fmtSpeed(item.speed) }}</span>
            <span v-if="item.kind === 'playlist' && target?.playlist === item.id && !target.live">entry {{ target.playlist_index + 1 }}</span>
          </div>
        </div>
        <span class="stripe" :style="{ background: item.colour }"></span>
        <span v-if="shotState(item) === 'playing' || shotState(item) === 'paused'" class="progress" :style="{ width: `${progress(item) * 100}%` }"></span>
      </button>
    </div>
  </div>
</template>
