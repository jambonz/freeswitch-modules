# mod_vad_detect

mod_vad_detect is a Freeswitch module designed to detect the start and end points of speech in a conversation.

## API

### Commands
This Freeswitch module provides the following API commands:

```
uuid_vad_detect start [one-shot|continuous] mode silence-ms voice-ms [bugname]
```
This command attaches a media bug to a channel and starts Voice Activity Detection (VAD).
- `uuid`: Unique identifier of the Freeswitch channel.
- `one-shot`: Detects the start of speech, sends an event titled `vad_detect:start_talking`, and ceases listening.
- `continuous`: Continuously listens and reports all events, including `vad_detect:start_talking` and `vad_detect:stop_talking`.
- `mode`:
    - -1 ("disable fvad, use native")
    - 0 ("quality")
    - 1 ("low bitrate")
    - 2 ("aggressive")
    - 3  ("very aggressive")
- `silence-ms`: number of milliseconds of silence that must come to transition from talking to stop talking
- `voice-ms`: number of milliseconds of voice that must come to transition to start talking

```
uuid_vad_detect stop [bugname]
```
This command halts VAD detection on the specified channel.

### Channel Variables

### Events
- `vad_detect:start_talking`: Indicates the detection of the start of speech.
- `vad_detect:stop_talking`: Indicates the detection of the end of speech.

## Usage
When utilizing [drachtio-fsrmf](https://www.npmjs.com/package/drachtio-fsmrf), you can employ this API command via the api method found on the 'endpoint' object. Here is an example:
```js
ep.api('uuid_vad_detect', `${ep.uuid} start one-shot 2 150 250 vad_detect`);
```