# Aura animation variants

Complete vertex/fragment pairs for `zeq2clip.sh --vp/--fp`, so the aura's
animation candidates stay comparable against one recorded demo:

| variant | motion |
| --- | --- |
| `a-scroll` | the field rotates continuously around the ring (`u += time * scrollSpeed`) |
| `b-sway` | the field sways around home through two incommensurate sines - **the shipped default** |
| `c-flipbook` | b, plus four strip frames hard-cutting at 8 fps (the anime's redrawn-licks flicker) |
| `d-rim-sway` | b, scaled by band depth: the interior veil pinned, only the outer flame moves |

`GameData/glsl/aura_vp.glsl` / `aura_fp.glsl` are the authoritative shaders;
`b-sway` is a snapshot of them. After changing the defaults, regenerate the
variants (the other three differ from the defaults by one substitution each -
see the generation block in the commit that added this directory) or the A/B
compares against stale code.

The A/B loop itself:

```
Tools/dev/zeq2clip.sh --record abtest --set auraExists=True --set auraAlways=True \
    -- +cg_auraScreenSpace 1 +model goku/default
for v in a-scroll b-sway c-flipbook d-rim-sway; do
    Tools/dev/zeq2clip.sh --play abtest --out /tmp/$v.gif \
        --vp Tools/dev/aura_variants/$v.vp.glsl \
        --fp Tools/dev/aura_variants/$v.fp.glsl \
        -- +cg_auraScreenSpace 1
done
```
