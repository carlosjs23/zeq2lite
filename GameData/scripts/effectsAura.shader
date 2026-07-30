Aura_Spike
{
	{
		map effects/aura/auraSpike.png
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
	{
		map effects/aura/auraSpikeWhite.png
		blendfunc add
	}
}
Aura_Swirl
{
	cull none
	{
		animmap 10 effects/aura/AuraSwirl_1of14.png effects/aura/AuraSwirl_2of14.png effects/aura/AuraSwirl_3of14.png effects/aura/AuraSwirl_4of14.png effects/aura/AuraSwirl_5of14.png effects/aura/AuraSwirl_6of14.png effects/aura/AuraSwirl_7of14.png effects/aura/AuraSwirl_8of14.png effects/aura/AuraSwirl_9of14.png effects/aura/AuraSwirl_10of14.png effects/aura/AuraSwirl_11of14.png effects/aura/AuraSwirl_12of14.png effects/aura/AuraSwirl_13of14.png effects/aura/AuraSwirl_14of14.png
		blendFunc add
		tcMod scroll 1.0 1.0
		tcMod scale 1.0 1.0
	}
}
AuraSwirl
{
	cull none
	{
		animmap 10 effects/aura/auraSwirl.png
		blendFunc GL_ONE GL_ONE_MINUS_SRC_ALPHA
	}
}
Aura_Trail
{
	cull none
	{
		clampmap effects/aura/auraTrail.png
		blendFunc add
		rgbGen vertex
	}
}
boltEffect
{
	entityMergable
	{
		animmap 8 effects/aura/bolt4.png effects/aura/bolt5.png effects/aura/bolt6.png effects/aura/bolt7.png effects/aura/bolt8.png 
		blendfunc blend
		rgbGen entity
		alphaGen entity
	}
}
AuraLightningSparks1
{
	entityMergable
	nomipmaps
	{
		animmap 10 effects/aura/auraLightningSpark1.png effects/aura/auraLightningSpark2.png effects/aura/auraLightningSpark3.png
		blendfunc blend
		rgbGen entity
		alphaGen entity
	}
}
AuraLightningSparks2
{
	entityMergable
	nomipmaps
	{
		animmap 10 effects/aura/auraLightningSpark4.png effects/aura/auraLightningSpark5.png effects/aura/auraLightningSpark6.png
		blendfunc blend
		rgbGen entity
		alphaGen entity
	}
}
// Screen-space aura: a flat ring of quads reshaped entirely in the vertex
// program against the player's screen-space bounding box. See glsl/aura_vp.glsl
// for the parameter layout, which cg_auras.c fills in per entity.
//
// No rgbGen: the GLSL path binds the raw vertex colours rather than the
// rgbGen-processed ones, and this mesh carries per-vertex data in that channel
// rather than colour. The tint comes from the entity colour in the fragment
// program instead.
Aura_ScreenSpace
{
	cull none
	{
		// A tiling spike strip, not the single-blob auraSpike.png: this
		// technique wraps one texture around the whole ring and scrolls
		// it, so it has to repeat seamlessly along U.
		// clampmapT: S repeats around the ring, T clamps so the
		// transparent spike tips never blend into the opaque body.
		clampmapT effects/aura/auraSpikeStrip.png
		program aura
		vertexProgram glsl/aura_vp.glsl
		fragmentProgram glsl/aura_fp.glsl
		blendFunc GL_ONE GL_ONE_MINUS_SRC_ALPHA
	}
}
