# Tenkaichi Budokai audio — sources and licences

Three files, all built from CC0 or public-domain recordings. Licences were read
from each source page individually, not inferred from its category. Nothing here
is synthesised and no source is mixed with anything not listed below.

| File | Built from | Source | Licence (as stated) |
| --- | --- | --- | --- |
| `gong.ogg` | `gong_01.ogg` (in `100-CC0-SFX.zip`, author **rubberduck**) layered with `Bienenkorbglocke.1133.Hz.ogg` | https://opengameart.org/content/100-cc0-sfx and https://commons.wikimedia.org/wiki/File:Bienenkorbglocke.1133.Hz.ogg | CC0 (both) |
| `crowd.ogg` | `Sound Effects - Applause after a concert.ogg`, author **Amada44** | https://commons.wikimedia.org/wiki/File:Sound_Effects_-_Applause_after_a_concert.ogg | CC0 1.0 Universal Public Domain Dedication |
| `crowdSwell.ogg` | `Clapping hurray.ogg` | https://commons.wikimedia.org/wiki/File:Clapping_hurray.ogg | Public domain |

## What was done to them

Trim, gain, fade, resample-pitch and crossfade on the real recordings only.

- **gong** — the strike's attack from the first source laid over the bell's ring
  from the second, aligned at the strike, the bell pitched down an octave first
  because doubling its decay is what turns a 1133 Hz chime into a gong.
- **crowd** — a 12.0 s seamless loop taken from 8.0 s in, with a 2.0 s
  equal-power crossfade at the seam. This is the bed the arena is heard through,
  so it has to loop without a detectable joint.
- **crowdSwell** — trimmed, normalised, 0.8 s tail fade. A one-shot, played over
  the bed on a ring-out.

Every file is mono Vorbis at peak −3.0 dBFS, matching the shipped player SFX.
Mono is not a preference: the engine will not spatialise a stereo sound.

## Not used, and why

A true crowd *murmur* bed was wanted and is not here. Every murmur recording
located was CC BY-SA (Beeld en Geluid and Concertgebouw items on Wikimedia) or
CC-BY (Gregor Quendel's set on OpenGameArt, which is the best crowd material
found). One was produced by low-passing the applause bed into a murmur; it is
not shipped, because a derived murmur is a substitute for a recording rather
than a recording. If a murmur matters, it needs a CC BY-SA source with
attribution accepted, or a commissioned take.
