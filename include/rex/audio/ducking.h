// rexglue - shared audio ducking factor.
//
// Lets one audio source (e.g. a mod's one-shot sound effect via
// RawPcmPlayer) temporarily lower the output of every other source sharing
// this process (background music via WmaPlayer) without either one needing
// a reference to the other. There's exactly one duck factor for the whole
// process, not a per-pair mixing graph -- deliberately simple, matching the
// "duck everything else while my stinger plays" use case rather than
// building a full audio mixer.
#pragma once

namespace rex::audio {

// Sets the process-wide duck multiplier applied to every other player's
// gain (see WmaPlayer/RawPcmPlayer). 1.0 = no ducking (default), 0.0 =
// fully silent. Clamped to [0, 1].
void SetDuckFactor(float factor);

// Returns the current duck multiplier. WmaPlayer applies this to its own
// gain computation; RawPcmPlayer does not apply it to itself (so a mod
// ducking music for its own stinger doesn't also duck the stinger). A mod
// that wants to duck-then-restore around a one-shot clip calls
// SetDuckFactor before PlayInt16 and again (back to 1.0) after roughly the
// clip's duration -- there's no automatic playback-finished callback here,
// see RawPcmPlayer for why (SDL_PutAudioStreamData is fire-and-forget).
float GetDuckFactor();

}  // namespace rex::audio
