import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 3101,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:3100", // the API binds 127.0.0.1 (IPv4 only)
        changeOrigin: true,
      },
    },
  },
});
