# mod_socket_audio

Ultra-low-latency bidirectional audio streaming module for FreeSWITCH. Provides a raw PCM audio pipe over TCP for integration with external audio processing services.

## Overview

`mod_socket_audio` provides a "dumb pipe" for raw PCM audio between FreeSWITCH and an external TCP socket. It handles only audio transport and resampling - all call control and application logic is delegated to an external sidecar application controlled via ESL (Event Socket Library).

### Use Cases

- **AI Voice Assistants**: Integrate with Google Gemini, OpenAI Realtime API, or other AI voice services
- **Speech Processing**: Connect to external ASR/TTS engines
- **Audio Analysis**: Stream audio to analysis services in real-time
- **Custom IVR**: Build sophisticated voice applications with external logic

### Design Philosophy

- **Minimal latency**: No buffering delays, immediate playback
- **Simple**: Audio only, all call control via ESL
- **Fast**: Pure raw PCM on socket, no parsing overhead
- **Generic**: Works with any audio processing backend

## Architecture

```
┌───────────────────┐        TCP (Raw PCM)      ┌─────────────────┐
│  FreeSWITCH       │◄───────────────────────-─►│   Sidecar App   │
│  mod_socket_audio │                           │   (Your code)   │
└────────┬──────────┘                           └────────┬────────┘
         │                                               │
     Media Bug                                     Your Protocol
   (READ_REPLACE +                                (WebSocket, gRPC,
    WRITE_REPLACE)                                  HTTP, etc.)
         │                                               │
    ┌────▼─────┐                                 ┌───────▼────────┐
    │ SIP Call │                                 │    External    │
    └──────────┘                                 │    Service     │
                                                 └────────────────┘
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
git clone https://github.com/emaktel/mod_socket_audio.git
cd mod_socket_audio

# Build
make

# Install (requires root or appropriate permissions)
sudo make install

# Load in FreeSWITCH (via fs_cli)
fs_cli -x "load mod_socket_audio"
```

To automatically load on startup, add to `modules.conf.xml`:

```xml
<load module="mod_socket_audio"/>
```

## Usage

### Dialplan Configuration

Route calls to your sidecar application using outbound ESL:

```xml
<extension name="voice_assistant">
  <condition field="destination_number" expression="^5000$">
    <action application="answer"/>
    <action application="socket" data="127.0.0.1:8084 async full"/>
  </condition>
</extension>
```

### Starting the Audio Pipe

From your sidecar via ESL, execute the `socket_audio` application:

```
execute socket_audio 127.0.0.1 9001
```

This connects to your sidecar's audio TCP server on the specified host and port.

### API Commands

#### `uuid_socket_audio_flush <uuid>`

Flushes the audio playback queue and enters a 50ms discard window to clear in-flight packets.

```bash
# Via fs_cli
uuid_socket_audio_flush <session-uuid>

# Via ESL
api uuid_socket_audio_flush <session-uuid>
```

**Use cases:**
- End-of-turn signaling from AI service
- User interrupts (barge-in)
- Stop current playback immediately

**Response:** `+OK` on success, `-ERR <message>` on failure

#### `uuid_socket_audio_stop <uuid>`

Stops the socket audio pipe for a session.

```bash
# Via fs_cli
uuid_socket_audio_stop <session-uuid>

# Via ESL
api uuid_socket_audio_stop <session-uuid>
```

**Use cases:**
- Graceful shutdown before call transfer
- Switching to different audio source
- Manual cleanup (optional - hangup auto-cleans)

**Response:** `+OK` on success, `-ERR <message>` on failure

### Events

The module emits custom events that can be subscribed to via ESL:

#### `socket_audio::playback_start`

Fired when audio playback begins (first frame written after silence or flush).

#### `socket_audio::playback_stop`

Fired when audio playback stops. Includes a `Playback-Stop-Reason` header:
- `complete` - Queue emptied naturally (end of audio)
- `flush` - Playback interrupted by flush command

**ESL subscription:**
```
event plain CUSTOM socket_audio::playback_start socket_audio::playback_stop
```

## Audio Format Specifications

### Socket Protocol

The module exchanges **pure raw PCM** with the sidecar - no headers, no framing, just audio bytes.

| Direction | Sample Rate | Format | Notes |
|-----------|-------------|--------|-------|
| To Sidecar (mic) | 16kHz | L16 LE mono | Module resamples from session rate |
| From Sidecar (speaker) | 24kHz | L16 LE mono | Module resamples to session rate |

**L16 LE** = Linear 16-bit signed little-endian PCM, mono channel

### Session Rate Handling

The module automatically resamples between the session's codec rate (8kHz, 16kHz, 48kHz, etc.) and the fixed socket rates:
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
| Discard window | 50ms |

### Critical Implementation Details

- **TCP_NODELAY**: Enabled to disable Nagle's algorithm (~40-200ms latency reduction)
- **Non-blocking sends**: Packets dropped rather than blocking the media thread
- **Zero-latency playback**: No pre-buffering, immediate playback on data arrival
- **Automatic cleanup**: All resources freed on call hangup via session pool

---

## Sidecar Implementation

The module requires an external sidecar application to handle:
1. ESL connection from FreeSWITCH
2. TCP audio server for raw PCM exchange
3. Connection to your external audio service
4. Audio format conversion as needed
5. Turn detection and flush commands

### Sidecar Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Sidecar Application                              │
│  ┌──────────────────┐  ┌───────────────────────────────────────────┐│
│  │ Outbound ESL     │  │ Per-Call Handler                          ││
│  │ Server (:8084)   │  │  - TCP Audio Server (ephemeral port)      ││
│  │                  │  │  - External Service Client                 ││
│  │ - Call control   │  │  - Audio format conversion                 ││
│  │ - Execute apps   │  │  - Turn detection → flush command          ││
│  │ - Hangup         │  │                                            ││
│  └──────────────────┘  └───────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
```

### Example: Basic Sidecar

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
    // Forward to your external service
    sendToExternalService(pcmData);
  });
});

audioServer.listen(0, '127.0.0.1', () => {
  const port = audioServer.address().port;
  // Execute: socket_audio 127.0.0.1 <port>
});
```

#### 3. ESL Commands

Start the audio pipe after setting up the audio server:

```javascript
// Answer the call
await sendEslCommand('sendmsg\ncall-command: execute\nexecute-app-name: answer');

// Start audio pipe
await sendEslCommand(
  `sendmsg\ncall-command: execute\nexecute-app-name: socket_audio\nexecute-app-arg: 127.0.0.1 ${audioPort}`
);

// Flush audio queue (on interruption or end-of-turn)
await sendEslCommand(`api uuid_socket_audio_flush ${uuid}`);
```

---

## Example: Google Gemini Integration

Here's an example of integrating with Google's Gemini Multimodal Live API:

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
    eslConn.api(`uuid_socket_audio_flush ${uuid}`);
  }
}
```

---

## Troubleshooting

### No audio reaching sidecar

1. Verify module is loaded:
   ```bash
   fs_cli -x "module_exists mod_socket_audio"
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
3. Confirm `uuid_socket_audio_flush` returns `+OK`

---

## License

See LICENSE file.
