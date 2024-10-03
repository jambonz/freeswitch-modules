# mod_google_tts

A Freeswitch module that allows Eleven Labs' Text-to-Speech API to be used as a tts provider.

## API

### Commands
This freeswitch module does not add any new commands, per se.  Rather, it integrates into the Freeswitch TTS interface such that it is invoked when an application uses the mod_dptools `speak` command with a tts engine of `elevenlabs` and a voice equal to the language code associated to one of the [supported Eleven Labs voices](https://elevenlabs.io/docs/api-reference/query-library)

### Events
None.

## Usage
When using [drachtio-fsrmf](https://www.npmjs.com/package/drachtio-fsmrf), you can access this functionality via the speak method on the 'endpoint' object.
```js
var text = "Hello World";
await endpoint.speak({
    "ttsEngine": 'elevenlabs',
    "voice": "W9OIfHh5DtdYiZUcFiql",
    "text": `{use_speaker_boost=1,optimize_streaming_latency=4,style=0.5,stability=0.5,similarity_boost=0.75,api_key=XXYYZZ,model_id=eleven_turbo_v2}${text}`,
});
```
## Options

Documentation on these options can be found in Eleven Labs API docs: [Voice Settings](https://elevenlabs.io/docs/speech-synthesis/voice-settings)

- use_speaker_boost
- optimize_streaming_latency
- style
- stability
- similarity_boost
