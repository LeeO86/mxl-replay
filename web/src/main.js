import { createApp } from "vue";
import App from "./App.vue";
import WidgetPage from "./components/WidgetPage.vue";
import "./style.css";

// /widget/<id> (operator screens) is the same page with one widget and no app around it.
createApp(location.pathname.startsWith("/widget/") ? WidgetPage : App).mount("#app");
