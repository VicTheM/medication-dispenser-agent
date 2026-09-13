import { defineRailway, project, service, empty } from "railway/iac";

export default defineRailway(() => {
  // empty() leaves the source unlinked so you can push local code via CLI.
  // To auto-deploy from GitHub instead, use: github("username/repo-name")
  const appSource = empty();

  const videoServer = service("video-server", {
    source: appSource,
    env: {
      NODE_ENV: "production",
      S3_ENDPOINT:
        "https://dc63d7e1f34a4437a67e242e912fda34.r2.cloudflarestorage.com",
      S3_ACCESS_KEY_ID: "50929696d185dd613c0d3a94daeeac7a",
      S3_SECRET_ACCESS_KEY:
        "00c3524b928e0270fbd3fea11b30285a5f1c3a9162f06966258209fba9c4d451",
      AI_VOICE_URL:
        "https://ally-project-c2h6bgdgf3hngzdv.spaincentral-01.azurewebsites.net/voice/ask",
      S3_PUBLIC_ENDPOINT: "https://pub-6772b7467ed4476aa24e606e3fb45e3d.r2.dev",
      API_BASE_URL: "https://medication-dispenser-agent.onrender.com",
      SERVER_TYPE: "video",
    },
    build: "pip install -r requirements.txt",
    start: "python video-audio-server.py",
    tcpProxy: true,
  });

  const audioServer = service("audio-server", {
    source: appSource,
    env: {
      NODE_ENV: "production",
      S3_ENDPOINT:
        "https://dc63d7e1f34a4437a67e242e912fda34.r2.cloudflarestorage.com",
      S3_ACCESS_KEY_ID: "50929696d185dd613c0d3a94daeeac7a",
      S3_SECRET_ACCESS_KEY:
        "00c3524b928e0270fbd3fea11b30285a5f1c3a9162f06966258209fba9c4d451",
      AI_VOICE_URL:
        "https://ally-project-c2h6bgdgf3hngzdv.spaincentral-01.azurewebsites.net/voice/ask",
      S3_PUBLIC_ENDPOINT: "https://pub-6772b7467ed4476aa24e606e3fb45e3d.r2.dev",
      API_BASE_URL: "https://medication-dispenser-agent.onrender.com",
      SERVER_TYPE: "audio",
    },
    build: "pip install -r requirements.txt",
    start: "python video-audio-server.py",
    tcpProxy: true,
  });

  return project("esp32-backend", {
    resources: [videoServer, audioServer],
  });
});
