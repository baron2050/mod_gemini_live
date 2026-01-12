# mod_gemini_live

Ultra-low-latency bidirectional audio streaming module for FreeSWITCH, designed to integrate with Google's Gemini Multimodal Live API.

## Overview

`mod_gemini_live` provides a "dumb pipe" for raw PCM audio between FreeSWITCH and an external TCP socket. It handles only audio transport and resampling - all call control, WebSocket handling, and Gemini protocol logic is delegated to an external sidecar application controlled via ESL (Event Socket Library).

### Design Philosophy

- **Minimal latency**: No buffering delays, immediate playback
- **Simple**: Audio only, all call control via ESL
- **Fast**: Pure raw PCM on socket, no parsing overhead

## Architecture

```
┌─────────────────┐     TCP (Raw PCM)         ┌──────────────────┐
│  FreeSWITCH     │◄────────────────────────►│  Sidecar App     │
│  mod_gemini_live│                           │  (Your code)     │
└────────┬────────┘                           └────────┬─────────┘
         │                                             │
    Media Bug                                   WebSocket + JSON
    (READ_REPLACE +                                    │
     WRITE_REPLACE)                            ┌───────▼────────┐
         │                                     │  Gemini Live   │
    ┌────▼────┐                                │  API           │
    │ SIP Call│                                └────────────────┘
    └─────────┘
```

## Prerequisites

- FreeSWITCH with development headers installed
- `pkg-config` (recommended) or FreeSWITCH include path configured
- GCC compiler

### Debian/Ubuntu

```bash
apt-get install freeswitch-dev pkg-config build-essential
```

### From Source

If FreeSWITCH was built from source, ensure the include path is available:

```bash
export FS_PREFIX=/usr/local/freeswitch
```

## Build & Install

```bash
# Clone the repository
git clone https://github.com/emaktel/mod_gemini_live.git
cd mod_gemini_live

# Build
make

# Install (requires root or appropriate permissions)
sudo make install

# Load in FreeSWITCH (via fs_cli)
fs_cli -x "load mod_gemini_live"
```

To automatically load on startup, add to `modules.conf.xml`:

```xml
<load module="mod_gemini_live"/>
```

## Usage

### Dialplan Configuration

Route calls to your sidecar application using outbound ESL:

```xml
<extension name="gemini_assistant">
  <condition field="destination_number" expression="^5000$">
    <action application="answer"/>
    <action application="socket" data="127.0.0.1:8084 async full"/>
  </condition>
</extension>
```

### Starting the Audio Pipe

From your sidecar via ESL, execute the `gemini_live` application:

```
execute gemini_live 127.0.0.1 9001
```

This connects to your sidecar's audio TCP server on the specified host and port.

### API Commands

#### `uuid_gemini_flush <uuid>`

Flushes the audio playback queue and enters a 500ms discard window to clear in-flight packets.

```bash
# Via fs_cli
uuid_gemini_flush <session-uuid>

# Via ESL
api uuid_gemini_flush <session-uuid>
```

**Use cases:**
- Gemini signals end-of-turn
- User interrupts (barge-in)
- Stop current playback immediately

**Response:** `+OK` on success, `-ERR <message>` on failure

#### `uuid_gemini_stop <uuid>`

Stops the Gemini audio pipe for a session.

```bash
# Via fs_cli
uuid_gemini_stop <session-uuid>

# Via ESL
api uuid_gemini_stop <session-uuid>
```

**Use cases:**
- Graceful shutdown before call transfer
- Switching to different audio source
- Manual cleanup (optional - hangup auto-cleans)

**Response:** `+OK` on success, `-ERR <message>` on failure

## Audio Format Specifications

### Socket Protocol

The module exchanges **pure raw PCM** with the sidecar - no headers, no framing, just audio bytes.

| Direction | Sample Rate | Format | Notes |
|-----------|-------------|--------|-------|
| To Sidecar (mic) | 16kHz | L16 LE mono | Module resamples from session rate |
| From Sidecar (speaker) | 24kHz | L16 LE mono | Module resamples to session rate |

**L16 LE** = Linear 16-bit signed little-endian PCM, mono channel

### Session Rate Handling

The module automatically resamples between the session's codec rate (8kHz, 16kHz, 48kHz, etc.) and the fixed Gemini rates:
- **Outbound**: Session rate → 16kHz
- **Inbound**: 24kHz → Session rate

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Audio latency (module) | < 1ms |
| Memory per call | ~200KB |
| CPU per call | Minimal (resampling only) |
| Socket protocol | Zero overhead (raw PCM) |
| Queue capacity | 90 seconds |

### Critical Implementation Details

- **TCP_NODELAY**: Enabled on both ends to disable Nagle's algorithm (~40-200ms latency reduction)
- **Non-blocking sends**: Packets dropped rather than blocking the media thread
- **Zero-latency playback**: No pre-buffering, immediate playback on data arrival
- **Automatic cleanup**: All resources freed on call hangup via session pool

---

## Sidecar Requirements

The module requires an external sidecar application to handle:
1. ESL connection from FreeSWITCH
2. TCP audio server for raw PCM exchange
3. WebSocket connection to Gemini Live API
4. Base64 encoding/decoding for Gemini protocol
5. Turn detection and flush commands

### Sidecar Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Sidecar Application                              │
│  ┌──────────────────┐  ┌───────────────────────────────────────────┐│
│  │ Outbound ESL     │  │ Per-Call Handler                          ││
│  │ Server (:8084)   │  │  - TCP Audio Server (ephemeral port)      ││
│  │                  │  │  - Gemini WebSocket Client                 ││
│  │ - Call control   │  │  - Audio format conversion (base64)        ││
│  │ - Execute apps   │  │  - Turn detection → flush command          ││
│  │ - Hangup         │  │                                            ││
│  └──────────────────┘  └───────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
```

### Sidecar Responsibilities

#### 1. ESL Server

Listen for outbound ESL connections from FreeSWITCH:

```javascript
const net = require('net');
const ESL_PORT = 8084;

const server = net.createServer((socket) => {
  // Handle ESL connection
  // Send 'connect' to receive channel data
  socket.write('connect\n\n');
});

server.listen(ESL_PORT, '127.0.0.1');
```

#### 2. Per-Call Audio Server

Create a TCP server on an ephemeral port for each call:

```javascript
const audioServer = net.createServer((socket) => {
  socket.setNoDelay(true);  // CRITICAL: Disable Nagle's algorithm

  // Receive 16kHz PCM from FreeSWITCH
  socket.on('data', (pcmData) => {
    // Forward to Gemini (base64 encoded)
    sendToGemini(pcmData);
  });
});

audioServer.listen(0, '127.0.0.1', () => {
  const port = audioServer.address().port;
  // Execute: gemini_live 127.0.0.1 <port>
});
```

#### 3. Gemini WebSocket Connection

Connect to Gemini Live API and handle audio:

```javascript
// Send audio to Gemini (16kHz PCM → base64)
function sendToGemini(pcmData) {
  const message = {
    realtimeInput: {
      mediaChunks: [{
        mimeType: 'audio/pcm;rate=16000',
        data: pcmData.toString('base64')
      }]
    }
  };
  geminiWs.send(JSON.stringify(message));
}

// Receive audio from Gemini (base64 → 24kHz PCM)
function handleGeminiMessage(message) {
  if (message.serverContent?.modelTurn?.parts) {
    for (const part of message.serverContent.modelTurn.parts) {
      if (part.inlineData?.data) {
        const pcmData = Buffer.from(part.inlineData.data, 'base64');
        audioSocket.write(pcmData);  // Send to FreeSWITCH
      }
    }
  }

  // Handle interruption
  if (message.serverContent?.interrupted) {
    eslConn.api(`uuid_gemini_flush ${uuid}`);
  }
}
```

#### 4. ESL Commands

Start the audio pipe after setting up the audio server:

```javascript
// Answer the call
await sendEslCommand('sendmsg\ncall-command: execute\nexecute-app-name: answer');

// Start audio pipe
await sendEslCommand(
  `sendmsg\ncall-command: execute\nexecute-app-name: gemini_live\nexecute-app-arg: 127.0.0.1 ${audioPort}`
);

// Flush audio queue (on interruption)
await sendEslCommand(`api uuid_gemini_flush ${uuid}`);
```

### Node.js Dependencies

A typical Node.js sidecar requires:

```json
{
  "dependencies": {
    "@google/genai": "^1.x"
  }
}
```

Or using raw WebSocket:

```json
{
  "dependencies": {
    "ws": "^8.x"
  }
}
```

### Gemini API Configuration

```javascript
const GEMINI_MODEL = 'gemini-2.0-flash-exp';  // Or latest model
const GEMINI_CONFIG = {
  responseModalities: ['AUDIO'],
  speechConfig: {
    voiceConfig: {
      prebuiltVoiceConfig: {
        voiceName: 'Aoede'  // Or other voice
      }
    }
  }
};
```

---

## Troubleshooting

### No audio reaching Gemini

1. Verify module is loaded:
   ```bash
   fs_cli -x "module_exists mod_gemini_live"
   ```

2. Check socket connection in logs:
   ```bash
   fs_cli -x "console loglevel debug"
   ```

3. Ensure TCP_NODELAY is set on both ends

### Audio glitches/stuttering

1. Check system load - resampling is CPU-intensive
2. Verify network latency between FreeSWITCH and sidecar
3. Ensure no packet loss on loopback

### Flush not working

1. Verify UUID is correct
2. Check ESL connection is active
3. Confirm `uuid_gemini_flush` returns `+OK`

---

## License

See LICENSE file.
