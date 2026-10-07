<script setup>
defineProps({
  text: { type: String, required: true },
  kind: { type: String, default: "neutral" }, // ok / warn / bad / neutral
});
</script>

<template>
  <span class="pill" :class="kind">{{ text }}</span>
</template>
