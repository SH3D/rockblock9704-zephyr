# JSPR as Ground Control's library speaks it

JSPR is the RockBLOCK 9704's serial protocol: JSON lines at 230400 8N1.
This is what Ground Control's C library (v1.0.1, commit `ab37e8a`, the pin
in `../west.yml`) sends and what it expects back, taken from its source
with a file and line for each claim. Paths are relative to that library's
repository. It is what `python/rb9704tools/fakemodem.py` implements, and
section 8 is the list of the library's surprises, several of which this
module works around (README, "Ground Control's library").

It describes the library, not the modem: where the two might differ (what
a real modem sends unprompted, how large its MT segments are) the source
cannot say, and the text says so. Field names the library never reads are
marked as placeholders.

"Library" means the C library. The Python binding (`rockblock9704/rockblock.c`,
`rockblock9704/rockblock9704.py`) is a thin wrapper over the same C files; it
is built without `KERMIT` (`CMakeLists.txt:88-109`), so firmware update is not
in it.

Notation: `>>` is a line the library writes to the modem, `<<` a line the modem
(or a fake) writes. `\r` is shown where it matters.

---

## 1. Line framing

### 1.1 Requests (library to modem)

- Format: `<VERB> <target> <json>\r`. VERB is `GET` or `PUT`, one space each
  side of the target, JSON always present (`{}` for a bare GET).
- Terminator is a single CR, no LF. Every format string ends `\r`
  (`src/jspr_command.c:24,40,89,106,166,184,207,236,257,272,286,325,350,370,386,407`).
- Built with `snprintf` into a 2048-byte buffer (`src/jspr_command.h:12`,
  `src/jspr_command.c:8`). The spacing inside the JSON is not uniform between
  commands (some `"k": v`, some `"k":v`), so parse the JSON, do not string-match it.
- Written with one `write()` loop until the whole line has gone
  (`src/serial_presets/serial_linux/serial_linux.c:188-228`).

### 1.2 Responses and unsolicited lines (modem to library)

Format: `<3-digit code> <target> <json>\r`

Reader: `receiveJspr()` (`src/jspr.c:35-160`).

- Reads one byte at a time (`src/jspr.c:59`). Each byte read is a `select()`
  with a 0.5 s timeout (`serial_linux.c:153-166`). If no byte arrives within
  0.5 s the read returns -1 and `receiveJspr` gives up for this call
  (`src/jspr.c:60-64`). So:
  - the first byte of a reply must arrive within 0.5 s of the request;
  - a pause of 0.5 s or more inside a line loses the bytes already read
    (they were consumed and the call returns false). Write each line in one go.
- **Terminator is CR** (`'\r'`), and only when it is at index > 2 of the
  line (`src/jspr.c:65`). LF is not a terminator and is not stripped at the
  end; a trailing LF becomes the first byte of the *next* line, where the
  code-scan below skips it (atoi skips leading whitespace). Safest: end every
  line with `\r` only. `\r\n` also works in practice.
- Lines shorter than 9 bytes before the CR (`JSPR_MIN_RESPONSE`,
  `src/jspr.h:18`, test at `src/jspr.c:85`) are not parsed and not discarded:
  `pos` is not reset, so the next byte overwrites the NUL and the short line
  is glued onto the front of the next one. Do not send short lines.
- Maximum line: 8191 bytes (`RX_BUFFER_SIZE 8192`, `src/jspr.h:13`,
  `src/jspr.c:57,74`). A line that fills the buffer without CR ends the call.
- **Code**: the reader scans 3-character windows from the start of the line,
  `atoi()` on each, until it finds a value in 200..500 inclusive
  (`src/jspr.c:89-108`); leading junk (the source mentions a DC1 before
  `bootInfo`, `src/jspr.c:87-88`) is then cut off with `memmove`
  (`src/jspr.c:110-113`). The code window is the first 3 bytes after that.
- **Target**: from byte 4 (after `NNN `) up to the next space
  (`src/jspr.c:115-127`). No space after the target means the call returns
  false (`src/jspr.c:117-121`). The target is copied only if shorter than 30
  characters (`JSPR_MAX_TARGET_LENGTH`, `src/jspr.h:16`, `src/jspr.c:123`).
- **JSON**: from the first `{` after the code to the end of the line
  (`src/jspr.c:140-151`). No `{` means the call returns false. JSON of 3500
  characters or more (`JSPR_MAX_JSON_LENGTH`, `src/jspr.h:20`) is left empty
  while the call still returns true, so the parse that follows fails.
  Always send a JSON object, `{}` at minimum.

### 1.3 Matching a response to a request

There is no request id on the wire. Matching is by target name only:

- `receiveJspr(&response, "target")`: every complete line whose target is
  not exactly `target` is **thrown away** and reading continues
  (`src/jspr.c:129-138`). The first line with the right target is returned,
  whatever its code. The caller then checks the code (almost always `== 200`).
- `waitForJsprMessage(&response, target, code, seconds)`
  (`src/jspr.c:162-188`): calls `receiveJspr` with the target filter in a loop
  until a line has both the target and the code, or the timeout passes
  (checked after each `receiveJspr`, then `delay(10)`). Lines with the right
  target and the wrong code are consumed and lost.
- `receiveJspr(&response, NULL)` (only from `rbPoll`, `src/rockblock_9704.c:716`):
  accepts any target, returns one line.

Consequence for a fake: while the library is waiting on a reply to a request,
**any other line you send (including `299 messageTerminate`, `299
messageOriginateSegment`, `299 constellationState`) is silently discarded.**
Send the solicited `200` line first and unsolicited lines after it.

### 1.4 Unsolicited lines

Recognised by code `299` (`JSPR_RC_UNSOLICITED_MESSAGE`, `src/jspr.h:40`)
and the target, in `rbPoll()` (`src/rockblock_9704.c:704-906`). `rbPoll` does
anything only if `serialPeek()` (`FIONREAD`, `serial_linux.c:230-241`)
reports bytes waiting (`src/rockblock_9704.c:714`), and then reads **one
line per call**. Targets it acts on:

| Code | Target | Condition | Section |
| --- | --- | --- | --- |
| 299 | `messageOriginateSegment` | an MO is at the head of the queue | 4 |
| not 200, not 299 | `messageOriginateSegment` | an MO is at the head | 4 |
| 299 | `messageOriginateStatus` | an MO is at the head | 4 |
| 299 | `messageTerminate` | always | 5 |
| 299 | `messageTerminateSegment` | always | 5 |
| 299 | `messageTerminateStatus` | always | 5 |
| 299 | `constellationState` | always | 6 |

Every other line read by `rbPoll` (any `200` reply, `299 operationalState`,
`299 simStatus`, `299 messageProvisioning`, `299 bootInfo`, ...) is consumed
and ignored.

### 1.5 Response codes

`src/jspr.h:37-63`. Only 200 means success anywhere in the library; 299 is
unsolicited. Error codes the enum names: 400 api version not selected,
401 unsupported request type, 402 configuration already set, 403 command too
long, 404 unknown target, 405 command malformed, 406 operation not allowed,
407 bad JSON, 408 request failed, 409 unauthorised, 410 SIM not configured,
411 wake xcvr in invalid, 412 invalid channel, 413 invalid action,
414 hardware not configured, 415 invalid radio path, 416 crash dump not
available, 417 feature not supported by hardware, 418 not provisioned,
419 invalid transmit power, 420 invalid burst type, 500 serial port error.
None of them is handled individually; any non-200 is a failure.

### 1.6 Serial setup (matters for a pty)

- 230400 8N1 (`src/rockblock_9704.h:119`). A pty ignores the speed.
- `setContextLinux` opens and immediately closes the port as a test
  (`serial_linux.c:27-33`), then `rbBegin` opens it again
  (`src/rockblock_9704.c:277-281`). On a pty the master will see `EIO` on
  read while no slave fd is open; the fake must tolerate `EIO` and keep the
  master open.
- Opened `O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK` (`serial_linux.c:41`).
  Termios: CS8, no parity, 1 stop, `ICRNL`/`IXON`/`IXOFF`/`IXANY` off,
  `ICANON`/`ECHO`/`ECHOE`/`ISIG` off (`serial_linux.c:90-98`). `c_oflag` is
  untouched (the library never writes LF, so `ONLCR` does not matter).
- Immediately after opening, `clearLeftoverData()` drains whatever is already
  waiting (`src/rockblock_9704.c:135-151,283`). Lines sent before `rbBegin`
  are lost.

---

## 2. Every request, and the reply the library expects

All replies are `200 <same target> {...}` unless noted. Fields not listed are
ignored. "valueint" means a JSON number read through cJSON's int.

### 2.1 GET apiVersion

```
>> GET apiVersion {}\r
<< 200 apiVersion {"supported_versions":[{"major":1,"minor":0,"patch":0}],"active_version":{"major":1,"minor":0,"patch":0}}\r
```
Source: `src/jspr_command.c:21-33`, parse `src/jspr.c:281-338`.

- `supported_versions`: array of objects with numeric `major`, `minor`,
  `patch`. At most 2 are kept, **taken from the end of the array backwards**:
  `supportedVersions[0]` is the last element (`src/jspr.c:294-312`).
- `active_version`: object with numeric `major`, `minor`, `patch`. Present
  and complete means `activeVersionSet = true`; absent (not an object) means
  false (`src/jspr.c:314-331`).
- The version values are not checked against anything.

### 2.2 PUT apiVersion

Sent only if the GET reply had no `active_version` (`src/rockblock_9704.c:171-175`).
```
>> PUT apiVersion {"active_version": {"major": 1, "minor": 0, "patch": 0}}\r
<< 200 apiVersion {"active_version":{"major":1,"minor":0,"patch":0}}\r
```
Source: `src/jspr_command.c:35-56`. The version sent is the last element of
the GET's `supported_versions`. Only the code 200 and target are checked;
the JSON is not parsed.

### 2.3 GET simConfig (the "simInterface" getter)

```
>> GET simConfig {}\r
<< 200 simConfig {"interface":"internal"}\r
```
Source: `src/jspr_command.c:86-98`, parse `src/jspr.c:430-473`.
`interface` string, one of `none`, `local`, `remote`, `internal`.

### 2.4 PUT simConfig

Sent if the GET did not say `internal` (`src/rockblock_9704.c:200-215`).
The library always asks for `internal` (`src/rockblock_9704.c:202`,
`src/jspr_command.c:58-84`).
```
>> PUT simConfig {"interface": "internal"}\r
<< 200 simConfig {"interface":"internal"}\r
<< 299 simStatus {"card_present":true,"sim_connected":true,"iccid":"8988169712345678901"}\r
```
The library needs the `200 simConfig` and then, **within 1 s**, a
`299 simStatus` line (`src/rockblock_9704.c:203-213`). Order matters: a
`299 simStatus` sent before the `200 simConfig` is discarded by the target
filter. The JSON of the `299 simStatus` is not parsed here.

### 2.5 GET operationalState

```
>> GET operationalState {}\r
<< 200 operationalState {"state":"active","reason":0}\r
```
Source: `src/jspr_command.c:163-176`, parse `src/jspr.c:475-541`.

- `state` string: `inactive`, `active`, `cal_test`, `hw_self_test`,
  `rf_scan`, `loopback`, `fault`.
- `reason` number 0..7 (normal, hw self test failure, temperature fault,
  RF power protection fault, VAM app failed ack, invalid hardware, low supply
  voltage, mfrtest used incorrectly; `src/jspr.h:141-151`). Optional.

### 2.6 PUT operationalState

```
>> PUT operationalState {"state": "active"}\r
<< 200 operationalState {"state":"active"}\r
```
Source: `src/jspr_command.c:123-200`. States written: `inactive`, `active`
(others exist in `putOperationalState` but the library only uses these two).
In `rbBegin` only the 200 is needed. In `rbResyncServiceConfig` the library
instead waits for a `299 operationalState` carrying the new state (2.12).

### 2.7 GET simStatus (card present, SIM connected, ICCID)

```
>> GET simStatus {}\r
<< 200 simStatus {"card_present":true,"sim_connected":true,"iccid":"8988169712345678901"}\r
```
Source: `src/jspr_command.c:367-379`, parse `src/jspr.c:1048-1078`, callers
`src/rockblock_9704.c:983-1026` (`rbGetCardPresent`, `rbGetSimConnected`,
`rbGetIccid`; each sends its own GET).

- `card_present` bool, `sim_connected` bool (JSON `true`/`false`; numbers are
  ignored).
- `iccid` string, **truncated to 19 characters** (`JSPR_ICCID_MAX_LENGTH 20`
  including the NUL, `src/jspr.h:35`, `src/jspr.c:1071`).

### 2.8 GET hwInfo (IMEI, serial number, hardware version, board temperature)

```
>> GET hwInfo {}\r
<< 200 hwInfo {"hw_version":"1.0","serial_number":"123456","imei":"300434030000000","board_temp":27}\r
```
Source: `src/jspr_command.c:283-295`, parse `src/jspr.c:1009-1046`, callers
`src/rockblock_9704.c:927-981` (`rbGetImei`, `rbGetHwVersion`,
`rbGetSerialNumber`, `rbGetBoardTemp`; each sends its own GET).

| Field | Type | Kept |
| --- | --- | --- |
| `hw_version` | string | first 6 chars (`src/jspr.h:31`) |
| `serial_number` | string | first 6 chars (`src/jspr.h:32`) |
| `imei` | string | first 15 chars (`src/jspr.h:33`) |
| `board_temp` | number | cast to int8 (-128..127) |

`rbGetBoardTemp` returns -100 on failure (`src/rockblock_9704.c:974`).

### 2.9 GET firmware (firmware version)

```
>> GET firmware {"slot": "primary"}\r
<< 200 firmware {"slot":"primary","validity":1,"version":{"major":1,"minor":2,"patch":3,"build_info":"fake"},"hash":"0000000000000000000000000000000000000000000000000000000000000000"}\r
```
Source: `src/jspr_command.c:317-340`, parse `src/jspr.c:340-428`, caller
`src/rockblock_9704.c:1028-1062`. `rbGetFirmwareVersion` returns
`"v<major>.<minor>.<patch>"` (`src/rockblock_9704.c:1051`).

- `slot` must be a **string** if present: the parser calls `strcmp` on
  `valuestring` without a type check (`src/jspr.c:366-377`), so a non-string
  crashes the host. The same holds for `version.build_info` and `hash`
  (`stpncpy` on `valuestring`, `src/jspr.c:409,417`).
- `validity` is read as `valueint > 0` (`src/jspr.c:382`). cJSON does not set
  `valueint` for `true`, so JSON `true` reads as false; send `1` if it matters.
  Nothing in the public API reads `validity`.
- `build_info` kept to 49 chars, `hash` to 64.
- On a non-200 the library prints `Failed\n` to stdout (`src/rockblock_9704.c:1042`).

### 2.10 GET constellationState (signal)

```
>> GET constellationState {}\r
<< 200 constellationState {"constellation_visible":true,"signal_bars":4,"signal_level":-105}\r
```
Source: `src/jspr_command.c:254-266`, parse `src/jspr.c:753-788`, caller
`src/rockblock_9704.c:908-925` (`rbGetSignal`).

- Waits up to 1 s for code 200 with this target (`src/rockblock_9704.c:912`);
  `299 constellationState` lines arriving meanwhile are consumed and ignored.
- `constellation_visible` bool; `signal_level` number, read only when
  visible is true; `signal_bars` number 0..5 (out of range ignored).
- Returns `signal_bars`, or -1 on failure.

### 2.11 GET messageProvisioning

Sent by `checkProvisioning()` the first time any send is attempted, then
never again in the life of the process (cached in a global,
`src/rockblock_9704.c:40,1150-1206`). `rbBegin` does not send it and does not
clear the cache.
```
>> GET messageProvisioning {}\r
<< 200 messageProvisioning {"provisioning":[{"topic_id":244,"topic_name":"RAW","priority":"Low","discard_time_seconds":604800,"max_queue_depth":8}]}\r
```
Source: `src/jspr_command.c:269-281`, parse `src/jspr.c:934-1007`.

- `provisioning`: array, at most 20 entries used (`JSPR_MAX_TOPICS`,
  `src/jspr.h:29`). `topicCount` is set to the array size even above 20.
- Per entry: `topic_id` number 64..65535 (below 64 is ignored and the entry's
  id is left as stack garbage); `topic_name` string shorter than 57 chars;
  `priority` string, one of `Safety-1`, `Safety-2`, `Safety-3`, `High`,
  `Medium`, `Low`.
- The struct has `discardTimeSeconds` and `maxQueueDepth` (`src/jspr.h:259-266`)
  but **the parser never reads them**; the field names above are placeholders,
  not known from the source.
- `provisioningSet` becomes true whenever the JSON parses at all
  (`src/jspr.c:1001`), with or without the array. The library caches the
  result even when the requested topic is missing, so a fake cannot change
  provisioning later without restarting the host process.
- The `messageProvisioning` callback (Python: `set_message_provisioning_callback`)
  fires only here, after the GET (`src/rockblock_9704.c:1181-1187`).

### 2.12 PUT serviceConfig (resync), with its state dance

`rbResyncServiceConfig()` (`src/rockblock_9704.c:1064-1120`):

```
>> GET operationalState {}\r
<< 200 operationalState {"state":"active","reason":0}\r          (within 1 s)
>> PUT operationalState {"state": "inactive"}\r
<< 200 operationalState {"state":"inactive"}\r                    (consumed and ignored)
<< 299 operationalState {"state":"inactive","reason":0}\r         (required, within 1 s)
>> PUT serviceConfig {"resync": true}\r
<< 200 serviceConfig {}\r                                         (required, within 1 s)
>> PUT operationalState {"state": "active"}\r
<< 200 operationalState {"state":"active"}\r                      (consumed and ignored)
<< 299 operationalState {"state":"active","reason":0}\r           (required, within 1 s)
```
If the modem was already inactive, the first PUT and the last two exchanges
are skipped and the call succeeds on `200 serviceConfig`. Any other state
fails without sending anything more. Request text: `src/jspr_command.c:381-400`.

### 2.13 PUT messageOriginate, PUT messageOriginateSegment, PUT messageOriginateStatus

See section 4. Request text: `src/jspr_command.c:201-252,402-421`.

### 2.14 GET/PUT firmware for update (C only, `KERMIT` builds)

`rbUpdateFirmware` (`src/rockblock_9704.c:1208-1372`) does the
`GET operationalState` / `PUT operationalState inactive` / wait for
`299 operationalState` sequence of 2.12, then
`>> PUT firmware {"slot": "primary"}\r`, needs `200 firmware {...}` and
then switches the line to Kermit. Not in the Python binding; out of scope
for the fake.

### 2.15 What the library never sends

There is no request for messageTerminate, messageTerminateSegment or
messageTerminateStatus, no MT acknowledgement of any kind, and no
`GET bootInfo`. `parseJsprBootInfo` exists (`src/jspr.c:198-279`) and is
never called.

---

## 3. rbBegin

`rbBegin(port)` (`src/rockblock_9704.c:273-300`); the Arduino one is the same
(`src/rockblock_9704_arduino.cpp:9-26`):

1. Open, close, open the port (1.6). Drain input.
2. `setApi()` (`src/rockblock_9704.c:153-186`), up to 2 attempts, each after
   a 5 ms sleep:
   ```
   >> GET apiVersion {}\r
   << 200 apiVersion {"supported_versions":[{"major":1,"minor":0,"patch":0}],"active_version":{"major":1,"minor":0,"patch":0}}\r
   ```
   Succeeds if the reply is 200 and `active_version` is present. If
   `active_version` is absent it sends `PUT apiVersion` (2.2) and succeeds on
   a `200 apiVersion`.
3. `setSim()` (`src/rockblock_9704.c:188-224`):
   ```
   >> GET simConfig {}\r
   << 200 simConfig {"interface":"internal"}\r
   ```
   Succeeds at once if `internal`. Otherwise PUT, `200 simConfig`, then
   `299 simStatus` within 1 s (2.4). No retry.
4. `setState()` (`src/rockblock_9704.c:226-271`):
   ```
   >> GET operationalState {}\r
   << 200 operationalState {"state":"active","reason":0}\r
   ```
   - `active`: success.
   - `inactive`: `>> PUT operationalState {"state": "active"}\r`, needs
     `200 operationalState`. A `299 operationalState` sent **before** that
     200 is taken as the reply (same target) and fails the begin, because
     its code is not 200. Send the 200 first.
   - any other state: `PUT ... "inactive"`, needs `200 operationalState`,
     then `PUT ... "active"` and reads the reply with the misspelt target
     `"operationState"` (`src/rockblock_9704.c:259`). No real line matches,
     so this path always fails (see 8).
   - `state` missing or not a string: fails.
5. `imtQueueInit()` empties both queues (`src/imt_queue.c:170-201`).

Minimal fake for a clean begin: answer the three GETs with `active_version`
set, `"interface":"internal"`, and `"state":"active"`. That is three
request/response pairs and nothing unsolicited.

---

## 4. Mobile originated (MO)

### 4.1 checkProvisioning (all send functions start with it)

`src/rockblock_9704.c:1150-1206`.

- Topic must be 64..65535 (`IMT_MIN_TOPIC_ID`/`IMT_MAX_TOPIC_ID`,
  `src/rockblock_9704.c:20-21`). Below 64 fails with nothing on the wire.
- First call: `GET messageProvisioning` (2.11). Needs `200` and the target.
  The topic must appear as a `topic_id` in the `provisioning` array.
- Later calls: checked against the cache only; nothing on the wire.
- Failure means the send returns false and nothing else is sent.

`rbSendMessage` (Python `send_message(msg)` with no topic) uses topic 244
(`RAW_TOPIC`, `src/rockblock_9704.h:160`, `src/rockblock_9704.c:347`).
Other named Cloudloop topics: 313..317 (`src/rockblock_9704.h:163-175`).

### 4.2 Payload and CRC

- Payload length 1..100000 bytes on Linux (`IMT_PAYLOAD_SIZE 100000U +
  IMT_CRC_SIZE`, `src/imt_queue.h:36-38`; check at
  `src/rockblock_9704.c:353,375,397,595`). 5000 on Arduino
  (`src/imt_queue.h:31-34`). Both overridable at build time
  (`CMakeLists.txt:14-20`).
- A 2-byte CRC is appended to the payload in the library's buffer
  (`appendCrc`, `src/rockblock_9704.c:331-341`):
  CRC-16 with polynomial 0x1021, initial value 0, no reflection, no final
  XOR (table at `src/rockblock_9704.c:99-133`, loop at
  `src/rockblock_9704.c:1122-1137`). This is CRC-16/XMODEM. Appended **high
  byte first**.
  - Check value: `"123456789"` gives `0x31C3`.
  - `"hello"` gives `0xC362`, so the 7 bytes sent are
    `68 65 6c 6c 6f c3 62`, base64 `aGVsbG/DYg==`.
- `message_length` on the wire is payload + 2.

### 4.3 The exchange

Example: `rbSendMessageAsync(244, "hello", 5)`, first send of the process.

```
>> GET messageProvisioning {}\r
<< 200 messageProvisioning {"provisioning":[{"topic_id":244,"topic_name":"RAW","priority":"Low"}]}\r
>> PUT messageOriginate {"topic_id":244, "message_length":7, "request_reference":1}\r
<< 200 messageOriginate {"topic_id":244,"request_reference":1,"message_id":17,"message_response":"message_accepted"}\r
        ... library returns; from here on it only reacts inside rbPoll() ...
<< 299 messageOriginateSegment {"topic_id":244,"message_id":17,"segment_length":7,"segment_start":0}\r
>> PUT messageOriginateSegment {"topic_id":244, "message_id":17, "segment_length":7, "segment_start":0, "data":"aGVsbG/DYg=="}\r
<< 200 messageOriginateSegment {"topic_id":244,"message_id":17,"segment_length":7,"segment_start":0}\r   (ignored by the library)
<< 299 messageOriginateStatus {"topic_id":244,"message_id":17,"final_mo_status":"mo_ack_received"}\r
```

Step by step:

1. **PUT messageOriginate** (`src/jspr_command.c:201-228`). `request_reference`
   is a global counter starting at 1, incremented after every PUT
   messageOriginate and wrapped from 101 to 1 (`src/jspr.c:16`,
   `src/jspr_command.c:212-216`).
2. **Reply** read with `receiveJspr(..., "messageOriginate")`
   (`src/rockblock_9704.c:429,562`), so it must come within 0.5 s and before
   any `299` line. Parse: `src/jspr.c:543-602`.
   - `topic_id` 64..65535, `request_reference` 1..100 (both stored, neither
     checked against the request).
   - `message_response` string: `message_accepted`, `subscription_invalid`,
     `message_discarded_on_overflow`.
   - `message_id` 0..255, read **only** when `message_response` is
     `message_accepted`.
   - **Code 200 alone counts as started** (`src/rockblock_9704.c:431-436,
     564-569`); `message_response` is never tested. Always send
     `message_accepted` with a `message_id` on 200, or the library keeps a
     garbage id. To refuse a message send a non-200 code, e.g.
     `406 messageOriginate {}`, and the library drops it.
   - The `moMessageStarted(id)` callback fires here in the async path
     (`src/rockblock_9704.c:571-574`).
3. **Segment request**, unsolicited, from the modem:
   `299 messageOriginateSegment` with `topic_id`, `message_id`,
   `segment_length` (1..1446), `segment_start` (0..100001)
   (`src/jspr.c:604-650`). The library answers only if both `message_id` and
   `topic_id` match the head of its MO queue (`src/rockblock_9704.c:721-727`).
   Out-of-range values leave the field as stack garbage.
4. **Segment reply** from the library (`src/rockblock_9704.c:728-739`,
   `src/jspr_command.c:230-252`): the bytes
   `buffer[segment_start .. segment_start+segment_length)` of payload+CRC,
   standard base64 with padding and no line breaks (vendored mbedtls,
   `src/third_party/base64/base64.c`), into a 2048-byte buffer
   (`BASE64_TEMP_BUFFER`, `src/rockblock_9704.h:112`). 1446 bytes encode to
   1928 characters, which fits. The library does **not** wait for a reply to
   this PUT; it is fire and forget.
   - The segment size is chosen by the modem. The library imposes the
     1..1446 range only. Nothing in the library computes segments; the fake
     must request every range covering `0 .. message_length` itself, in any
     order, one `299` per segment. Each request is served by one `rbPoll`.
   - The library does not check that the range lies within the message.
     Asking past `message_length` returns stale buffer bytes (zeros after
     init).
5. **Reply to the segment PUT**: a `200 messageOriginateSegment` is read by a
   later `rbPoll` and ignored. Any code other than 200 and 299 with target
   `messageOriginateSegment` and a matching `message_id` **fails the MO**
   (`src/rockblock_9704.c:742-761`): `moMessageComplete(id, -1)` or the
   sync failure flag, and the message is dropped. To exercise this:
   `<< 405 messageOriginateSegment {"topic_id":244,"message_id":17}\r`
   (it must carry `message_id`; the topic is not checked).
6. **Final status**, unsolicited: `299 messageOriginateStatus` with
   `message_id` matching the head (topic not checked) and `final_mo_status`
   (`src/jspr.c:790-880`, `src/rockblock_9704.c:762-795`).
   - `mo_ack_received` is the only success.
   - Failures: `message_discarded_on_overflow`, `message_expired`,
     `message_transfer_timeout`, `segment_not_supplied`, `segment_incorrect`,
     `network_error`, `message_cancelled_pre_transit`,
     `message_cancelled_in_transit`, `subscription_invalid`,
     `protocol_error`, `message_dropped_local_crc_error`,
     `crc_error_in_transfer`, `user_supplied_crc_error`.
   - Either way the message leaves the queue, and the next queued async MO
     (if any) is started at once with its own `PUT messageOriginate`
     (`checkMoQueue`, `src/rockblock_9704.c:691-702`).

The library never checks a CRC on the MO side itself; a fake that wants to
emulate `user_supplied_crc_error` should reassemble the segments, check the
last two bytes as CRC-16/XMODEM big-endian over the rest, and choose the
final status.

### 4.4 rbSendMessageAsync (Python `send_message_async(msg, topic)`)

`src/rockblock_9704.c:589-617`, `548-587`.

- Returns after the `200 messageOriginate`. Segments and status are handled
  only when the caller runs `rbPoll()` (Python `poll()`), one line per call.
  The upstream example polls every 10 ms (`examples/python/async_send_receive.py`).
- Outcome arrives through `moMessageComplete(id, 1 | -1)`.
- The MO queue holds 1 message (`IMT_QUEUE_SIZE 1`, `src/imt_queue.h:17-19`)
  and starts **locked** (`moLock = true`, `src/imt_queue.c:10`), so a second
  async send while one is in flight fails at once with nothing on the wire.
- The Python wrapper returns `None` and sends nothing if `topic` is omitted
  (`rockblock9704/rockblock9704.py:114-122`).

### 4.5 rbSendMessage / rbSendMessageAny (Python `send_message(msg[, topic][, timeout=30])`)

`src/rockblock_9704.c:343-363`, `387-407`, `409-470`.

- Same wire exchange. After the `200 messageOriginate` the library spins on
  `rbPoll()` with no sleep until success, failure or `timeout` seconds
  measured from before the PUT (`src/rockblock_9704.c:413,437-457`).
- The fake must deliver segment requests and the final status within that
  timeout, unprompted, because the library sends nothing in the meantime
  except segment PUTs.
- Success is seen only through a flag that is set **only when no
  `moMessageComplete` callback is registered** (`src/rockblock_9704.c:771-778`).
  See 8.

---

## 5. Mobile terminated (MT)

### 5.1 The exchange

Everything is unsolicited from the modem. **The library sends nothing at
all for an MT**: no request, no segment acknowledgement, no final ack.
`rbAcknowledgeReceiveHeadAsync` only removes the message from the
library's own queue (`src/rockblock_9704.c:681-689`, `src/imt_queue.c:149-168`).

Example: delivering `"hello"` on topic 244 as message id 5.

```
<< 299 messageTerminate {"topic_id":244,"message_id":5,"message_length_max":7}\r
<< 299 messageTerminateSegment {"topic_id":244,"message_id":5,"segment_length":7,"segment_start":0,"data":"aGVsbG/DYg=="}\r
<< 299 messageTerminateStatus {"topic_id":244,"message_id":5,"final_mt_status":"complete"}\r
```

1. **`299 messageTerminate`** (`src/jspr.c:652-690`, `src/rockblock_9704.c:798-818`):
   - `topic_id` 64..65535, `message_id` 0..255, `message_length_max` 3..100002.
   - The library adds an entry to its MT queue (one slot). If the queue is
     full and **unlocked** (the default, `mtLock = false`,
     `src/imt_queue.c:9`) the old entry is dropped to make room
     (`src/imt_queue.c:39-60`). If locked (`rbReceiveLockAsync`) the new MT is
     refused and `mtMessageComplete(id, -1)` fires.
   - `message_length_max` is used only as "length > 0" at this point; the
     final length comes from the segments.
2. **`299 messageTerminateSegment`** (`src/jspr.c:692-751`,
   `src/rockblock_9704.c:819-850`), zero or more:
   - Applied to the **last** MT queued, and only if its `message_id`
     matches. Topic not checked.
   - `segment_start` 0..100001, `segment_length` 1..1446.
   - `data`: standard base64 of the segment bytes. Decoded straight into the
     message buffer at `segment_start`, with `segment_length` as the output
     limit. If the decoded size exceeds `segment_length`, or the base64 has
     an invalid character, the MT fails (`mtMessageComplete(id, -1)`) and is
     removed.
   - **The `data` string is truncated to 1446 characters** before decoding
     (`JSPR_MAX_SEGMENT_LENGTH 1447`, `src/jspr.h:21`, `src/jspr.c:736-744`).
     So in practice keep each MT segment to **at most 1083 bytes** (1444
     base64 characters), even though the parser accepts `segment_length` up to
     1446. A 1446-byte segment (1928 characters) would be silently cut and
     decoded short. This is the library's limit; whether a real modem sends
     larger MT segments is not answerable from the source.
   - Each segment adds its `segment_length` to a running total
     (`messageLengthAsync`, `src/rockblock_9704.c:834`). Do not repeat or
     overlap segments, the total would be wrong.
3. **`299 messageTerminateStatus`** (`src/jspr.c:882-932`,
   `src/rockblock_9704.c:851-892`):
   - `message_id` must match the last MT. `final_mt_status`: `complete`
     (success), `message_timed_out`, `message_cancelled`,
     `crc_error_in_transfer`.
   - On `complete`: message length = sum of `segment_length`s, counter reset,
     `ready = true`, then `mtMessageComplete(id, 1)` or the sync flag.
   - On anything else: `mtMessageComplete(id, -1)` or the sync failure flag;
     the entry stays in the queue and the running total is **not** reset.

### 5.2 CRC on MT

The library treats the **last 2 bytes of the reassembled MT as a CRC and
strips them** without checking them (`src/rockblock_9704.c:484-485,
506-507, 650-651`). So the fake must send payload + 2 bytes. For realism
use the same CRC-16/XMODEM, high byte first, as MO (`"hello"` gives
`aGVsbG/DYg==` as above). The library writes a NUL over the first CRC byte
and returns `length - 2` bytes. Total length below 2 underflows; keep MTs to
at least 1 byte of payload (the parser's minimum `message_length_max` is 3).

### 5.3 rbReceiveMessageAsync and rbAcknowledgeReceiveHeadAsync

- `rbReceiveMessageAsync(&buf)` (`src/rockblock_9704.c:636-659`, Python
  `receive_message_async()` with no topic): does not touch the serial line.
  Returns the head MT if `ready`, otherwise 0. It can be called repeatedly and
  returns the same message until it is acknowledged.
- `rbAcknowledgeReceiveHeadAsync()` (Python `acknowledge_receive_head_async()`):
  local only, zeroes and removes the head. Nothing is sent to the modem.
- The lines themselves are processed by `rbPoll()`, one per call.

### 5.4 rbReceiveMessage (sync, Python `receive_message()`)

`src/rockblock_9704.c:472-546`. Calls `rbPoll()` once. If the head MT is
waiting for segments (`readyToProcess`), it then spins on `rbPoll()` **with
no timeout** until the MT succeeds or fails. Otherwise it returns 0 at once.
So the usual pattern is the caller looping on `receive_message()`; the call
that reads the `299 messageTerminate` then blocks until the status line.

`rbReceiveMessageWithTopic(&buf, topic)` (`src/rockblock_9704.c:494-515`)
does not filter by topic; the parameter is assigned to and discarded.

---

## 6. Unsolicited constellationState and messageProvisioning

### 6.1 `299 constellationState`

```
<< 299 constellationState {"constellation_visible":true,"signal_bars":3,"signal_level":-110}\r
```
Handled in `rbPoll` (`src/rockblock_9704.c:893-903`) with the same parser as
the GET (`src/jspr.c:753-788`): `constellation_visible` bool,
`signal_level` number (read only when visible), `signal_bars` 0..5. Calls the
`constellationState` callback (Python dict `constellationVisible`,
`signalBars`, `signalLevel`, `rockblock9704/rockblock.c:106-132`). Nothing is
stored; `rbGetSignal` always sends a fresh GET. The struct is uninitialised
(`src/rockblock_9704.c:895`), so with `constellation_visible:false` the
`signal_level` handed to the callback is garbage, and a missing
`signal_bars` is garbage too. Always send `signal_bars`.

### 6.2 `299 messageProvisioning`

**Not handled.** No code path looks at an unsolicited `messageProvisioning`;
`rbPoll` consumes and ignores it. Provisioning is learned only through the
one `GET messageProvisioning` in 2.11.

### 6.3 Other unsolicited lines

`299 simStatus` matters only inside `setSim` (2.4). `299 operationalState`
matters only inside `rbResyncServiceConfig` and `rbUpdateFirmware` (2.12).
Everything else is ignored.

---

## 7. Constants a fake must respect

| Constant | Value | Where | Meaning for the fake |
| --- | --- | --- | --- |
| line terminator | `\r` | `src/jspr.c:65`, `src/jspr_command.c` | both directions |
| first-byte / inter-byte timeout | 0.5 s | `serial_linux.c:153` | reply fast, write lines whole |
| `RX_BUFFER_SIZE` | 8192 | `src/jspr.h:13` | max line to the library |
| `JSPR_MIN_RESPONSE` | 9 | `src/jspr.h:18` | min line length |
| `JSPR_MAX_TARGET_LENGTH` | 30 | `src/jspr.h:16` | target < 30 chars |
| `JSPR_MAX_JSON_LENGTH` | 3500 | `src/jspr.h:20` | JSON < 3500 chars |
| `COMMAND_MAX_LEN` | 2048 | `src/jspr_command.h:12` | longest request line |
| `BASE64_TEMP_BUFFER` | 2048 | `src/rockblock_9704.h:112` | MO segment base64 |
| MO `segment_length` | 1..1446 | `src/jspr.c:624` | request no more than 1446 |
| MT `segment_length` | 1..1446 parsed; data cut at 1446 chars | `src/jspr.c:712,738-741` | use <= 1083 bytes |
| `segment_start` | 0..100001 | `src/jspr.c:632,720` | |
| `message_length_max` | 3..100002 | `src/jspr.c:672` | |
| `message_id` | 0..255 | `src/jspr.c:590,640,680,728,810,902` | |
| `request_reference` | 1..100, wraps | `src/jspr_command.c:212-216` | |
| topic id | 64..65535 | `src/rockblock_9704.c:20-21` | |
| `IMT_PAYLOAD_SIZE` | 100000 + 2 (Linux), 5000 + 2 (Arduino) | `src/imt_queue.h:31-39` | max payload + CRC |
| `IMT_CRC_SIZE` | 2 | `src/imt_queue.h:25` | CRC-16/XMODEM, big-endian |
| `IMT_QUEUE_SIZE` | 1 | `src/imt_queue.h:17-19` | one MO and one MT in flight |
| `JSPR_MAX_TOPICS` | 20 | `src/jspr.h:29` | provisioning entries used |
| `JSPR_TOPIC_NAME_MAX_LENGTH` | 57 | `src/jspr.h:28` | name < 57 chars |
| baud | 230400 | `src/rockblock_9704.h:119` | |

---

## 8. Surprises and quirks a fake should know

1. **Lines that arrive while the library waits for a reply are discarded**
   if their target differs (1.3). Unsolicited MT or segment requests sent
   during a GET/PUT exchange are lost for good. Order every exchange as:
   solicited `200` first, then `299`s.
2. **A line with no 3-digit code in 200..500** makes the code scan run off
   the end of the line through the zeroed buffer, then `memmove` with a
   negative length cast to `size_t` (`src/jspr.c:89-113`). Expect a crash.
   Never send banners, prompts or blank-ish lines of 9 bytes or more.
   On an M5Stamp-C3 it did crash (2026-10-25), when the UART's FIFO had
   overflowed and handed over fragments of old lines. rockblock9704-zephyr
   now keeps the FIFO empty from boot and gives the library no line it
   opened in the middle of (`src/rb9704_serial.c`).
3. **`resultCodeIndexStart` is not reset** after a line is discarded for the
   wrong target (`src/jspr.c:48,133-136`): if that line had leading junk,
   the next line is read from the same offset. Do not send leading junk.
4. **A pause of 0.5 s inside a line** after a discarded line makes the
   partial line be parsed as a response, because `validResponse` stays true
   (`src/jspr.c:80` after the `continue` at 136).
5. **setState misspells its target** as `"operationState"`
   (`src/rockblock_9704.c:259`), so `rbBegin` from any state other than
   `active` or `inactive` always fails.
6. **`299 operationalState` before `200 operationalState`** fails `rbBegin`
   from `inactive` (3, step 4).
7. **Sync send with a callback registered never reports success.** `moSent`
   and `mtReceived` are set only when the matching callback is absent
   (`src/rockblock_9704.c:771-778,868-875`). In Python, once
   `set_mo_message_complete_callback` has been called, `send_message()` always
   runs to its timeout and returns False, even though the message went. Same
   for `receive_message()` with an MT callback: it never returns.
8. **After a sync send times out the MO stays in the queue** (started, never
   removed; `src/rockblock_9704.c:452-456,464-467`), and the queue is locked
   by default. The next `rbSendMessage` cannot add (returns false with only the
   provisioning check, if any, on the wire); the next `rbSendMessageAny`
   ignores the failed add (`queued >= 0`, `src/rockblock_9704.c:400`) and
   **re-sends the old stuck message** with a new `PUT messageOriginate`. A late
   `299 messageOriginateStatus` for the old id, read by any later `rbPoll`,
   clears it. The async path has the same hole with no timeout at all: the
   MO is removed only by a final status, and `rbCancelMessage` is the one
   way to ask for one. `rbEnd` then `rbBegin` empties the queue
   (`imtQueueInit`) but not `moQueuedMessages`, so with the count still 1
   the next `rbSendMessageAsync` queues (`moQueuedMessages == 0` is false)
   and never transmits. The Zephyr module zeroes the count after `rbBegin`.
9. **`200 messageOriginate` alone means started**, whatever
   `message_response` says (4.3 step 2).
10. **`rbReceiveMessage` has no timeout** once an MT has been announced.
    If the fake announces an MT and never sends a status, the host blocks
    forever (with the GIL held, see 11).
11. **The Python binding never releases the GIL** (no
    `Py_BEGIN_ALLOW_THREADS` in `rockblock9704/rockblock.c`). Each blocking
    C call (`begin`, `send_message` for up to `timeout` s, `receive_message`,
    every 0.5 s read timeout) freezes every Python thread in that process.
    **Run the fake in a separate process**, not a thread beside the code
    under test.
12. **MT running length leaks** between messages: `messageLengthAsync` is
    reset only on a `complete` status (`src/rockblock_9704.c:865-866`). After a
    failed or abandoned MT, the next MT's reported length includes the old
    segments' lengths.
13. **MT CRC is stripped, never checked** (5.2). A fake that sends no CRC
    loses the last 2 bytes of every message.
14. **MT data longer than 1446 base64 characters is truncated silently** (5.1).
15. **Provisioning is fetched once per process** and cached even on a miss
    (2.11). Unsolicited `messageProvisioning` is ignored (6.2).
16. **Uninitialised structs everywhere.** Every parser fills a stack struct
    the caller did not zero (`src/rockblock_9704.c:169,197,235,433,566,723,
    800,826,858,895,915,1069,1178`). A missing or out-of-range field leaves
    garbage that may still be acted on (for example a garbage `message_id`
    that happens to match). Always send every field listed above, in range.
17. **Non-string where a string is expected crashes** in the firmware parser
    (2.9).
18. **`rbPoll` reads one line per call** and only when `FIONREAD > 0`. A burst
    of N lines needs N polls. Multi-segment MOs and MTs need the host to poll
    repeatedly; the sync paths do that themselves in a busy loop.
19. **The 9-byte minimum and `pos > 2` rule** mean short lines merge into the
    next one (1.2). Any real JSPR line is longer than 9 bytes, so this only
    bites junk.
20. **`clearLeftoverData` reads up to 32 bytes into a 32-byte buffer** and the
    Linux read writes a NUL after them (`src/rockblock_9704.c:137-149`,
    `serial_linux.c:178`), one byte past the end. Avoid sending anything
    before `rbBegin`.
21. **Cancellation** (`rbCancelMessage`, Python `cancel_message(topic, id)`):
    `>> PUT messageOriginateStatus {"topic_id": 244, "message_id": 17, "action": "cancel"}\r`
    (`src/rockblock_9704.c:619-634`). The reply is not read; the `200` is
    swallowed by a later `rbPoll`. The outcome must come as
    `299 messageOriginateStatus` with, for example,
    `"final_mo_status":"message_cancelled_pre_transit"`.

---

## 9. A minimal session, end to end

```
(host opens port, drains)
>> GET apiVersion {}\r
<< 200 apiVersion {"supported_versions":[{"major":1,"minor":0,"patch":0}],"active_version":{"major":1,"minor":0,"patch":0}}\r
>> GET simConfig {}\r
<< 200 simConfig {"interface":"internal"}\r
>> GET operationalState {}\r
<< 200 operationalState {"state":"active","reason":0}\r
(rbBegin returns true)

>> GET hwInfo {}\r
<< 200 hwInfo {"hw_version":"1.0","serial_number":"123456","imei":"300434030000000","board_temp":27}\r

>> GET constellationState {}\r
<< 200 constellationState {"constellation_visible":true,"signal_bars":5,"signal_level":-100}\r

(send_message(b"hello"), topic 244, sync)
>> GET messageProvisioning {}\r
<< 200 messageProvisioning {"provisioning":[{"topic_id":244,"topic_name":"RAW","priority":"Low"}]}\r
>> PUT messageOriginate {"topic_id":244, "message_length":7, "request_reference":1}\r
<< 200 messageOriginate {"topic_id":244,"request_reference":1,"message_id":1,"message_response":"message_accepted"}\r
<< 299 messageOriginateSegment {"topic_id":244,"message_id":1,"segment_length":7,"segment_start":0}\r
>> PUT messageOriginateSegment {"topic_id":244, "message_id":1, "segment_length":7, "segment_start":0, "data":"aGVsbG/DYg=="}\r
<< 200 messageOriginateSegment {"topic_id":244,"message_id":1}\r
<< 299 messageOriginateStatus {"topic_id":244,"message_id":1,"final_mo_status":"mo_ack_received"}\r
(send_message returns True, if no MO callback is registered)

(receive_message() in a loop)
<< 299 messageTerminate {"topic_id":244,"message_id":2,"message_length_max":7}\r
<< 299 messageTerminateSegment {"topic_id":244,"message_id":2,"segment_length":7,"segment_start":0,"data":"aGVsbG/DYg=="}\r
<< 299 messageTerminateStatus {"topic_id":244,"message_id":2,"final_mt_status":"complete"}\r
(receive_message returns b"hello")
```

The JSON field order and spacing in the `<<` lines are free; the library
looks fields up by name. Field names the library reads are exactly those
listed in section 2; any extra fields a real modem sends are ignored.
