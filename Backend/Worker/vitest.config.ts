import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: {
          CALLS_APP_ID: "test-app",
          CALLS_APP_SECRET: "test-secret",
          CLIENT_ACCESS_KEY: "test-client-key",
          HCF_DEVELOPER_CREDENTIAL: "test-flow-developer-credential",
          FLOW_API_BASE_URL: "https://flow.example.test",
          HCF_CREDENTIAL_ENDPOINT: "https://management.example.test/access-credentials",
          FLOW_TOKEN_TTL_SECONDS: "300",
          TURN_KEY_ID: "test-turn-key-id",
          TURN_KEY_API_TOKEN: "test-turn-api-token"
        }
      }
    })
  ]
});
