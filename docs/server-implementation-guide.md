# Server Implementation Guide — Full Flow cho firmware xiaozhi-esp32 (robo-worker)

**Đối tượng:** team server (robo-worker / Cloudflare Worker).
**Mục tiêu:** liệt kê **chính xác** mọi thứ server phải implement để firmware này chạy được **trọn vẹn luồng**, bao gồm 2 mode điều khiển bằng nút "+":

- **Bấm "+" 2 lần** → mode **trò chuyện bình thường** (free chat)
- **Bấm "+" 3 lần** → mode **listen-and-say**

Tài liệu này mô tả hợp đồng (contract) **đúng như firmware đang gửi/nhận** (trích từ `main/protocols/*` và `main/application.cc`). Phần xác thực (device JWT, session token) đã có trong `device-auth-hardware-guide.md` — ở đây chỉ tóm tắt phần server **bắt buộc** trả về.

> ⚠️ **Bất đối xứng audio (quan trọng nhất):** **upstream = PCM thô**, **downstream = Opus**. Sai chỗ này → kết nối thành công nhưng "không có gì xảy ra". Xem §4.

---

## 1. Tổng quan luồng

```text
[Boot]    Device → POST /xiaozhi/ota  (Authorization: Bearer <device-JWT>)
          Server → { websocket: { url, token }, ... }          (§2)
[Connect] Device → GET <url>  Upgrade: websocket
                   Authorization: Bearer <session-token>        (§3)
          Server → 101 Switching Protocols
[Hello]   Device → {"type":"hello", audio_params:{...}}
          Server → {"type":"hello", session_id, audio_params:{pcm 24k 40ms}}  (§4)
[Chat]    Device ══ binary PCM 1920B/frame ══►  (mic)            (§5)
          Device → {"type":"listen","state":"stop"}  (hết câu)
          Server → {"type":"tts","state":"start"} → Opus → {"type":"tts","state":"stop"}
[Mode]    "+"×2 → {"type":"free_chat","state":"start"}           (§6)
          "+"×3 → {"type":"listen_and_say","state":"start", course_id, lesson_id}
```

---

## 2. OTA endpoint — `POST /xiaozhi/ota`

Firmware gọi mỗi lần boot. Header firmware gửi:

```http
POST /xiaozhi/ota
Authorization: Bearer <device-identity-JWT>   # ES256, xem device-auth guide
Device-Id: <MAC address>
Client-Id: <board UUID>
Content-Type: application/json
```

**Server phải trả HTTP 200** với JSON chứa object `websocket`. Firmware (`main/ota.cc`) **lặp qua mọi key trong `websocket`** và lưu vào NVS, sau đó dùng:

| Key trong `websocket` | Bắt buộc | Firmware dùng để |
|---|---|---|
| `url` | ✅ | địa chỉ `wss://…/xiaozhi/ws` để mở WebSocket |
| `token` | ✅ | gửi trong header `Authorization: Bearer` khi mở WS |
| `version` | tùy chọn | chọn binary protocol version (mặc định 1 = text/JSON + binary thô; xem §5) |

Response tối thiểu:

```json
{
  "server_time": { "timestamp": 1717430400000, "timezone_offset": 420 },
  "firmware": { "version": "1.0.0", "url": "" },
  "websocket": {
    "url": "wss://robo-worker.taskfi.workers.dev/xiaozhi/ws",
    "token": "<session-token>"
  }
}
```

- `server_time.timestamp` (ms) + `timezone_offset` (phút): firmware **set đồng hồ hệ thống** từ đây (cần cho JWT/TLS). Nên trả.
- `firmware.url` rỗng = không OTA. Nếu có URL → firmware tải và update.
- Khi auth fail: trả `websocket.token: ""` (device sẽ không có token hợp lệ để mở WS). Xem `device-auth-hardware-guide.md §4.5`.

---

## 3. WebSocket endpoint — `GET /xiaozhi/ws`

Firmware mở WS với header:

```http
GET /xiaozhi/ws
Upgrade: websocket
Authorization: Bearer <session-token>     # token từ bước OTA
Protocol-Version: 1
Device-Id: <MAC address>
Client-Id: <board UUID>
```

Server phải:
1. Verify session token (HS256) + re-check device active/binding.
2. Trả `101 Switching Protocols`.
3. Forward tới `Tutor` DO của child tương ứng (robo-worker đã làm qua `routeAgentRequest`, gắn marker `xz_device`/`xz_client` trên query string).

> Firmware gửi token trong **header** `Authorization`. Không dựa vào `?token=` (chỉ dùng cho test khi `E2E_ENABLED=1`).

---

## 4. Hello handshake (ngay sau khi WS mở)

**Device → server** (`main/protocols/websocket_protocol.cc::GetHelloMessage`):

```json
{
  "type": "hello",
  "version": 1,
  "features": { "aec": true, "mcp": true },
  "audio_params": { "format": "pcm", "sample_rate": 24000, "channels": 1, "frame_duration": 40 }
}
```

**Server → device** (bắt buộc, firmware chặn 10s chờ frame này, không có → đóng kết nối):

```json
{
  "type": "hello",
  "transport": "websocket",
  "session_id": "<uuid>",
  "audio_params": { "format": "pcm", "sample_rate": 24000, "channels": 1, "frame_duration": 40 }
}
```

Firmware đọc `audio_params.sample_rate` / `frame_duration` / `format` từ hello của server để cấu hình **đường nhận**. Lưu ý nghịch lý: firmware nhận **Opus** ở downstream nhưng hello khai báo PCM — firmware này đã được build để **giải mã Opus ở loa** bất kể (xem §5). Server cứ trả `audio_params` mô tả định dạng **upstream** như trên.

### Định dạng audio (BẤT ĐỐI XỨNG)

| Hướng | Codec | Sample rate | Channels | Frame | Bytes/frame |
|---|---|---|---|---|---|
| **Upstream** (mic → server) | **PCM int16 LE** (KHÔNG Opus) | 24000 | 1 | 40 ms | **1920** |
| **Downstream** (server → loa) | **Opus** | 24000 | 1 | 20 ms | thay đổi |

- Mỗi binary frame upstream = **đúng 1 frame PCM 1920 byte**, không header/length-prefix.
- Mỗi binary frame downstream = **1 gói Opus**; firmware ráp theo thứ tự đến.
- Server forward PCM thẳng vào STT, forward Opus của TTS bridge thẳng về device.

---

## 5. Binary framing & `Protocol-Version`

Firmware hỗ trợ 3 version (header `Protocol-Version`, và key `version` trong OTA `websocket`):

| version | Upstream/downstream binary | Ghi chú |
|---|---|---|
| **1** (mặc định, khuyến nghị) | payload audio **thô**, không header | Mỗi WS binary frame = 1 frame audio. Đơn giản nhất. robo-worker dùng cái này. |
| 2 | có header `BinaryProtocol2` (version, type, timestamp, payload_size) | timestamp cho AEC server-side |
| 3 | có header `BinaryProtocol3` (type, payload_size) | |

➡️ **Server chỉ cần hỗ trợ version 1**: nhận/gửi audio dưới dạng binary frame thô. Không cần đặt `version` trong OTA (mặc định = 1).

---

## 6. Frame device → server (server phải xử lý)

Tất cả là **text frame JSON** (trừ audio là binary). Mọi frame đều có `session_id` (server có thể bỏ qua).

| `type` | Payload | Khi nào device gửi | Server phải làm |
|---|---|---|---|
| `hello` | `{audio_params, features, version}` | ngay sau khi WS mở | trả hello (§4) |
| *(binary)* | PCM 1920B | khi đang nghe | đẩy vào STT |
| `listen` `state:"stop"` | `{"type":"listen","state":"stop"}` | VAD phát hiện im lặng / hết câu | **flush STT** → emit transcript ngay |
| `listen` `state:"start"` | `{..., "mode":"auto"\|"manual"\|"realtime"}` | bắt đầu nghe (một số luồng) | (tùy chọn) `listen.start` là implicit, có thể bỏ qua |
| `listen` `state:"detect"` | `{..., "text":"<wake word>"}` | wake-word phát hiện | (tùy chọn) log |
| `abort` | `{"type":"abort"[, "reason":"wake_word_detected"]}` | user ngắt lời / barge-in | best-effort dừng TTS hiện tại |
| `mcp` | `{"type":"mcp","payload":{...}}` | gọi/đáp tool MCP | xử lý theo MCP protocol (nếu dùng) |
| **`free_chat`** | `{"type":"free_chat","state":"start"}` | **"+"×2** | **xóa lesson/game đang chạy** → quay về free chat (xem §8) |
| **`listen_and_say`** | `{"type":"listen_and_say","state":"start","course_id":"…","lesson_id":"…"}` | **"+"×3** | **bắt đầu lesson listen-and-say** với course/lesson đó (xem §9) |

> `course_id` / `lesson_id` được **hardcode trong firmware** qua Kconfig
> (`CONFIG_LISTEN_AND_SAY_COURSE_ID`, `CONFIG_LISTEN_AND_SAY_LESSON_ID`), mặc định trỏ
> tới course demo. Nếu `course_id` vắng → server fallback `DEFAULT_COURSE`.

---

## 7. Frame server → device (device hiểu được)

| `type` | Payload | Tác dụng trên device |
|---|---|---|
| `tts` `state:"start"` | `{"type":"tts","state":"start"}` | reset decoder, chuyển sang **Speaking**, bắt đầu phát Opus theo sau |
| `tts` `state:"sentence_start"` | `{..., "text":"…"}` | (log/caption) text đang được đọc |
| `tts` `state:"stop"` | `{"type":"tts","state":"stop"}` | hết reply → nếu mode ≠ manual thì chuyển sang **Listening** (nghe câu tiếp) |
| *(binary)* | gói Opus 20ms | giải mã + phát ở loa |
| `stt` | `{"type":"stt","text":"…"}` | (log) transcript của trẻ |
| `llm` | `{"type":"llm","emotion":"happy"}` | đổi biểu cảm/emote trên màn hình |
| `mcp` | `{"type":"mcp","payload":{...}}` | gọi tool device-side (volume, GPIO…) |
| `system` | `{"type":"system","command":"reboot"}` | reboot device (dùng cho OTA) |
| `alert` | `{"type":"alert","status":..,"message":..,"emotion":..}` | hiện cảnh báo + chuông |

> **Bắt buộc đúng shape `{type:"tts", state:...}`** cho device. KHÔNG gửi shape FE
> `{event:"chunk"|"done"}` — device này bỏ qua chúng, sẽ kẹt ở Speaking/Connecting.
> Trong robo-worker đó là các helper `emitTts*` (nhánh `isXiaoziConnection`).

---

## 8. Sequence: trò chuyện bình thường ("+"×2)

```text
Device                                   Server (Tutor DO)
  │ "+"×2 → EnterFreeChatMode()
  │ (mở WS + hello nếu chưa)
  │ ── {"type":"free_chat","state":"start"} ─────────►│ xóa listeningSession/activeGame/
  │                                                    │ pendingLessonStart
  │ ══ binary PCM (trẻ nói) ═══════════════════════►  │ → STT
  │ ── {"type":"listen","state":"stop"} ─────────────►│ flush STT → transcript → LLM → TTS
  │ ◄── {"type":"tts","state":"start"} ───────────────┤
  │ ◄══ Opus packets ═════════════════════════════════┤
  │ ◄── {"type":"tts","state":"stop"} ────────────────┤  (device → Listening, nghe tiếp)
```

**Server phải implement:** khi nhận `free_chat`, clear toàn bộ state lesson/game (nếu không, lần reconnect `onConnect` sẽ resume lesson cũ và utterance kế tiếp bị hiểu là câu trả lời bài học, không phải chat).

---

## 9. Sequence: listen-and-say ("+"×3)

```text
Device                                   Server (Tutor DO)
  │ "+"×3 → StartListenAndSay(course_id, lesson_id)
  │ (mở WS + hello nếu chưa; listening_mode = auto)
  │ ── {"type":"listen_and_say","state":"start",       │
  │     course_id, lesson_id} ────────────────────────►│ startLearning(courseId, lessonId)
  │                                                     │ → startListenAndSay → sendQuestion
  │ ◄── {"type":"tts","state":"start"} ────────────────┤  (đọc câu hỏi đầu tiên)
  │ ◄══ Opus packets ══════════════════════════════════┤
  │ ◄── {"type":"tts","state":"stop"} ─────────────────┤  device → Listening
  │ ══ binary PCM (trẻ trả lời) ══════════════════════► │ → STT
  │ ── {"type":"listen","state":"stop"} ───────────────►│ chấm điểm → câu kế / kết thúc
  │ ◄── {"type":"tts",...} (feedback + câu tiếp) ───────┤
```

**Server phải implement, và phải kiểm tra:**
1. `onMessage` nhận `listen_and_say` → resolve `courseId`/`lessonId` → `startListenAndSay`.
2. `startListenAndSay` phải tìm được **connection của device**. Trong robo-worker, `this.xiaozhiConnection` được gán trong `onConnect` (kết nối có marker `xz_device`). Nếu connection chưa được gán → target = null → câu hỏi bị **drop âm thầm**.
3. Câu hỏi gửi qua `speak()` → **`emitTts*`** → frame `{type:"tts"}` + Opus (KHÔNG phải `{event:…}`).
4. `lesson.skill` phải là `"listen-and-say"`, nếu không `startListenAndSay` throw → bọc try/catch, đừng để chết connection.

---

## 10. Checklist implement phía robo-worker

Trạng thái với nhánh hiện tại (các sửa đổi đã thực hiện trong `src/agents/tutor.ts`):

- [x] `onXiaoziMessage` xử lý `hello` / `listen` / `abort`
- [x] `onXiaoziMessage` thêm case **`listen_and_say`** → `startLearning(courseId, lessonId)` (fallback `DEFAULT_COURSE`, có try/catch)
- [x] `onXiaoziMessage` thêm case **`free_chat`** → clear `listeningSession`/`activeGame`/`pendingLessonStart`
- [x] `parsed` type thêm `course_id`/`lesson_id`
- [ ] **Verify end-to-end** với device thật hoặc harness `/device-auth-test` (mode B): register → claim → ota → ws → hello → "+"×3 → nghe câu hỏi
- [ ] (Đề xuất) Khi `startLearning` throw hoặc không có target, gửi 1 frame báo lỗi (vd `{"type":"tts",...}` câu "chưa sẵn sàng") để device không kẹt ở Connecting tới khi timeout (~120s)

Phía firmware (repo này) **đã xong**: protocol senders, Application methods, Kconfig IDs, binding nút "+" trên board `xingzhi-cube-0.96oled-wifi`.

---

## 11. Lưu ý / edge cases

- **Device kẹt Connecting:** sau "+"×3 device mở channel, gửi frame, rồi **chờ `tts.start`** của server để rời Connecting. Nếu server throw (đã catch+log, không gửi gì) device nằm Connecting tới khi channel timeout (~120s, `Protocol::IsTimeout`) rồi mới tự phục hồi. Với đồ chơi trẻ em nên gửi phản hồi audio khi không start được.
- **`session_id`:** device gắn vào mọi frame gửi lên; server có thể bỏ qua (robo-worker tự sinh `session_id` riêng trong hello).
- **Half-duplex:** trong lúc server đang TTS, server **bỏ qua audio đến** (tránh thu tiếng loa của chính nó). Device được phép gửi audio trở lại sau khi nhận `tts.stop`.
- **Reconnect:** session token sống 1 ngày. WS rớt → device reconnect bằng token cũ; nếu 401 → device chạy lại OTA. `onConnect` nên resume `listeningSession` nếu còn (đã có trong robo-worker).
- **Đóng kết nối khi revoke:** dùng close code `4001` (xem device-auth guide §7.6).

---

## 12. Tham chiếu

- Xác thực device + OTA + session token: `device-auth-hardware-guide.md` (repo robo-worker)
- Protocol WS gốc của xiaozhi: `docs/websocket.md`
- STT/TTS bridge format: `websocket-api.md` (repo robo-worker)
- Code firmware liên quan:
  - `main/protocols/protocol.cc` — `SendListenAndSay`, `SendFreeChat`, `SendStartListening`, …
  - `main/protocols/websocket_protocol.cc` — hello, headers, binary framing
  - `main/application.cc` — `EnterFreeChatMode`, `StartListenAndSay`, `OnIncomingJson`
  - `main/boards/xingzhi-cube-0.96oled-wifi/xingzhi-cube-0.96oled-wifi.cc` — binding nút "+"
